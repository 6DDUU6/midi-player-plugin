#include "App.h"
#include <deque>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <array>

namespace app {
namespace {
game::Bridge bridge;
class PreviewBackend:public player::Backend {
    uint32_t next=1;
    bool Short(int,int,std::string&) override {return true;}
    player::StartResult Start(int,int,uint32_t& handle,std::string&) override {handle=next++;return player::StartResult::Started;}
    bool Stop(uint32_t,std::string&) override {return true;}
} previewBackend;
std::unique_ptr<player::Transport> transport;
std::deque<Command> commands;
std::mutex queueMutex,statusMutex;
Status status;
std::atomic<bool> preview{false};
std::string message;
struct ManualVoice {bool held=false;uint32_t handle=0;int instrument=0,key=0;};
std::array<ManualVoice,21> manualVoices{};
std::string manualError;
player::Backend& Backend() {return preview.load() ? static_cast<player::Backend&>(previewBackend):static_cast<player::Backend&>(bridge);}
bool ReleaseManual(int slot) {
    auto& voice=manualVoices[slot];voice.held=false;if (!voice.handle) return true;std::string error;
    if (!Backend().Stop(voice.handle,error)) {manualError=error;return false;}voice.handle=0;return true;
}
bool ReleaseAllManual() {bool ok=true;for (int i=0;i<21;++i) if (!ReleaseManual(i)) ok=false;return ok;}
bool TransportActive() {auto state=transport->Status().state;return state==player::State::Playing || state==player::State::Countdown || state==player::State::Paused;}
void StartManual(ManualVoice& voice) {
    uint32_t handle=0;std::string error;auto result=preview.load() ? Backend().Start(voice.instrument,voice.key,handle,error):bridge.ManualStart(voice.instrument,voice.key,handle,error);
    if (result!=player::StartResult::Started || !handle) {manualError=error.empty() ? "游戏未创建长音，请检查乐器音域和校准。":error;return;}
    voice.handle=handle;
}
void ManualOn(const Command& c) {
    if (c.slot<0 || c.slot>=21 || c.instrument<1 || c.instrument>1000000 || c.key<1 || c.key>1000000 || TransportActive()) return;
    auto& voice=manualVoices[c.slot];if (voice.held || !ReleaseManual(c.slot)) return;voice.held=true;voice.instrument=c.instrument;voice.key=c.key;manualError.clear();
    std::string error;
    if (player::UsesShort(c.instrument,c.sustained)) {
        const bool ok=preview.load() ? Backend().Short(c.instrument,c.key,error):bridge.ManualShort(c.instrument,c.key,error);
        if (!ok) {manualError=error;return;}
    }
    if (player::UsesSustained(c.instrument,c.sustained)) StartManual(voice);
}
void Tick(double now) {
    if (!transport) return;
    std::deque<Command> work;
    { std::unique_lock<std::mutex> lock(queueMutex,std::try_to_lock); if (lock.owns_lock()) work.swap(commands); }
    for (const auto& c:work) {
        switch (c.action) {
        case Action::Load: if (ReleaseAllManual()) transport->Load(c.plan); break;
        case Action::Play: if (ReleaseAllManual()) transport->Play(now,c.value); break;
        case Action::Pause: ReleaseAllManual();transport->Pause(now); break;
        case Action::Stop: ReleaseAllManual();transport->Stop(); break;
        case Action::Seek: if (ReleaseAllManual()) transport->Seek(c.value,now); break;
        case Action::ManualOn: ManualOn(c);break;
        case Action::ManualOff: if (c.slot>=0 && c.slot<21) ReleaseManual(c.slot);break;
        case Action::ManualRelease: ReleaseAllManual();break;
        case Action::Test: {
            if (!ReleaseAllManual()) break;
            if (c.instrument<1 || c.instrument>1000000 || c.key<1 || c.key>1000000) {manualError="试听乐器或音高超出范围。";break;}
            const bool sustained=player::UsesSustained(c.instrument,c.sustained);
            midi::Song song;song.durationMs=sustained ? 600:300;song.tracks.resize(1);
            midi::Note note;note.pitch=60;note.endMs=song.durationMs;song.tracks[0].notes.push_back(note);
            player::TrackConfig config;config.instrument=c.instrument;config.middleDo=c.key;config.sustained=sustained;
            transport->Load(player::Build(song,{config},1));transport->Play(now,c.value);break;
        }
        }
    }
    transport->Tick(now);
    for (int i=0;i<21;++i) if (!manualVoices[i].held && manualVoices[i].handle) ReleaseManual(i);
    Status current; current.transport=transport->Status(); current.preview=preview.load(); current.game=bridge.Status();
    current.manualError=manualError;for (const auto& voice:manualVoices) if (voice.handle) ++current.manualVoices;
    std::unique_lock<std::mutex> lock(statusMutex,std::try_to_lock);
    if (lock.owns_lock()) {current.message=status.message;status=std::move(current);}
}
}
void Initialize(bool isPreview) {
    preview.store(isPreview); transport.reset(new player::Transport(isPreview ? static_cast<player::Backend&>(previewBackend):static_cast<player::Backend&>(bridge)));
    std::lock_guard<std::mutex> lock(statusMutex); status.preview=isPreview;status.message=isPreview ? "界面预览：调度模拟运行，不产生游戏声音" : "正在定位游戏乐器入口…";
}
void StartGameHooks() {
    bool ok=bridge.Initialize(Tick,[](const std::string& text) {std::lock_guard<std::mutex> lock(statusMutex);status.message=text;});
    std::lock_guard<std::mutex> lock(statusMutex);status.game=bridge.Status();
    status.message=ok ? "入口已就绪。进入乐器界面手动弹一个音；使用短音乐器时先弹一个短音完成校准。" : "定位失败，请查看入口状态。";
}
bool Send(const Command& c) {
    std::lock_guard<std::mutex> lock(queueMutex);
    if (c.action==Action::Stop) commands.clear();
    if (c.action==Action::ManualRelease) {
        commands.erase(std::remove_if(commands.begin(),commands.end(),[](const Command& pending) {return pending.action==Action::ManualOn || pending.action==Action::ManualOff || pending.action==Action::ManualRelease;}),commands.end());
        commands.push_front(c);return true;
    }
    if (commands.size()>=32) return false;
    commands.push_back(c);return true;
}
Status GetStatus() {
    std::lock_guard<std::mutex> lock(statusMutex);auto copy=status;
    if (!preview.load()) copy.game=bridge.Status();return copy;
}
void PreviewTick() {if (preview.load()) Tick(game::NowMs());}
void PreviewTick(double nowMs) {if (preview.load()) Tick(nowMs);}
void SetManualLogging(bool enabled) {bridge.SetManualLogging(enabled);}
std::vector<game::ManualNote> ManualNotes() {return bridge.ManualNotes();}
void SetGameInputCapture(bool enabled) {bridge.SetInputCapture(enabled);}
game::InputBatch TakeGameInput() {return bridge.TakeInput();}
}
