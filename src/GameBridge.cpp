#include "GameBridge.h"
#include "../vendor/MinHook/include/MinHook.h"
#include <atomic>
#include <array>
#include <cstdio>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace game {
namespace {
using UiFn=int64_t(__fastcall*)(void*,int,void*);
using ShortFn=int64_t(__fastcall*)(void*,void*,int,int,uint8_t,void*);
using EmitOld=int64_t(__fastcall*)(void*,uint32_t,int,int,void*);
using EmitNew=int64_t(__fastcall*)(void*,uint32_t,int,int,uint8_t,void*);
using PlayOld=uint32_t(__fastcall*)(int,int,uint8_t,void*);
using PlayNew=uint32_t(__fastcall*)(int,int,uint8_t,uint8_t,void*);
using StopOld=uint8_t(__fastcall*)(uint32_t,uint8_t,void*);
using StopNew=uint8_t(__fastcall*)(uint32_t,uint8_t,uint8_t,void*);
using PumpFn=int64_t(__fastcall*)(void*);
struct UiContext { void* owner=nullptr; void* klass=nullptr; void* method=nullptr; int index=0,slot=-1; bool local=false; };
struct Replay {
    int instrument,key; bool entered=false,synced=false;
    Replay(int i,int k):instrument(i),key(k) {}
};
struct Voice { uint8_t extra; DWORD thread; };
std::mutex mutex;
Calibration calibration;
bool manualLogging=false;
std::deque<ManualNote> manualNotes;
bool inputCapture=false;
size_t lostInput=0;
std::deque<InputEvent> inputEvents;
std::map<uint32_t,InputEvent> inputVoices;
locator::Result resolved;
std::array<UiFn,8> uiOriginal{};
ShortFn shortOriginal=nullptr;
void* emitOriginal=nullptr; void* playOriginal=nullptr; void* stopOriginal=nullptr;
PumpFn pumpOriginal=nullptr;
std::function<void(double)> onTick;
std::vector<void*> targets;
UiContext captured;
std::map<uint32_t,Voice> voices;
std::map<int,uint8_t> instrumentExtras;
std::atomic<DWORD> inputThread{0};
std::atomic<bool> pumping{false},installed{false};
__declspec(thread) Replay* replay=nullptr;
__declspec(thread) UiContext* uiActive=nullptr;
__declspec(thread) bool inPlugin=false;
__declspec(thread) bool manualPlayback=false;
struct ManualScope {
    bool previous;
    ManualScope():previous(manualPlayback) {manualPlayback=true;}
    ~ManualScope() {manualPlayback=previous;}
};
bool ClassOf(void* object,void** klass) {
    if (!object) return false;
    __try { *klass=*reinterpret_cast<void**>(object); return *klass!=nullptr; }
    __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}
void EnqueueInput(const InputEvent& event) {
    if (inputEvents.size()==4096) {inputEvents.pop_front();++lostInput;}
    inputEvents.push_back(event);
}
void CaptureInput(const ManualNote& note,uint32_t handle,double time,UiContext* ui=nullptr) {
    if (inPlugin || (!note.sustained && !ui) || (note.sustained && !handle)) return;
    std::lock_guard<std::mutex> lock(mutex);if (!inputCapture) return;
    InputEvent event;event.instrument=note.instrument;event.noteKey=note.noteKey;event.sustained=note.sustained;event.handle=handle;event.timeMs=time;
    if (note.sustained) {
        auto existing=inputVoices.find(handle);
        if (existing!=inputVoices.end()) {auto off=existing->second;off.on=false;off.timeMs=time;EnqueueInput(off);}
        else if (inputVoices.size()>=4096) {++lostInput;return;}
        inputVoices[handle]=event;
    }
    EnqueueInput(event);
}
void CaptureStop(uint32_t handle,double time) {
    if (inPlugin) return;std::lock_guard<std::mutex> lock(mutex);if (!inputCapture) return;
    auto it=inputVoices.find(handle);if (it==inputVoices.end()) return;
    auto event=it->second;event.on=false;event.timeMs=time;EnqueueInput(event);inputVoices.erase(it);
}
void Record(ManualNote note,UiContext* ui=nullptr) {
    if (inPlugin && !manualPlayback) return;
    std::lock_guard<std::mutex> lock(mutex);
    note.fromPlugin=inPlugin;
    if (!inPlugin) {
        auto tid=GetCurrentThreadId();inputThread.store(tid);
        calibration.threadId=tid;calibration.lastInstrument=note.instrument;calibration.lastNoteKey=note.noteKey;calibration.extraFlag=note.extraFlag!=0;
        if (note.sustained) instrumentExtras[note.instrument]=note.extraFlag;
        if (ui) {ui->local=true;captured=*ui;calibration.shortReady=true;}
    }
    if (manualLogging && (ui || note.sustained || note.fromPlugin)) {
        GetLocalTime(&note.localTime);
        if (manualNotes.size()==200) manualNotes.pop_front();
        manualNotes.push_back(note);
    }
}
void RecordPlay(int instrument,int key,uint8_t sync,uint8_t extra,void* method) {
    ManualNote note;note.instrument=instrument;note.noteKey=key;note.sustained=true;
    note.synchronize=sync;note.hasExtraFlag=resolved.extraFlag;note.extraFlag=extra;note.method=method;Record(note);
}
template<int Slot> int64_t __fastcall UiHook(void* owner,int index,void* method) {
    UiContext ui; ui.owner=owner; ui.index=index; ui.method=method; ui.slot=Slot;
    ClassOf(owner,&ui.klass);
    auto previous=uiActive; uiActive=&ui;
    int64_t result=uiOriginal[Slot](owner,index,method); uiActive=previous; return result;
}
int64_t __fastcall ShortHook(void* owner,void* position,int instrument,int key,uint8_t flag,void* method) {
    if (replay) { instrument=replay->instrument; key=replay->key; replay->entered=true; }
    ManualNote note;note.instrument=instrument;note.noteKey=key;note.owner=owner;note.position=position;note.flag=flag;note.method=method;Record(note,uiActive);
    CaptureInput(note,0,NowMs(),uiActive);
    return shortOriginal(owner,position,instrument,key,flag,method);
}
int64_t __fastcall EmitOldHook(void* owner,uint32_t entity,int instrument,int key,void* method) {
    if (replay) { instrument=replay->instrument; key=replay->key; replay->synced=true; }
    return reinterpret_cast<EmitOld>(emitOriginal)(owner,entity,instrument,key,method);
}
int64_t __fastcall EmitNewHook(void* owner,uint32_t entity,int instrument,int key,uint8_t extra,void* method) {
    if (replay) { instrument=replay->instrument; key=replay->key; replay->synced=true; }
    return reinterpret_cast<EmitNew>(emitOriginal)(owner,entity,instrument,key,extra,method);
}
uint32_t __fastcall PlayOldHook(int instrument,int key,uint8_t sync,void* method) {
    double time=NowMs();RecordPlay(instrument,key,sync,0,method);auto handle=reinterpret_cast<PlayOld>(playOriginal)(instrument,key,sync,method);
    ManualNote note;note.instrument=instrument;note.noteKey=key;note.sustained=true;CaptureInput(note,handle,time);return handle;
}
uint32_t __fastcall PlayNewHook(int instrument,int key,uint8_t sync,uint8_t extra,void* method) {
    double time=NowMs();RecordPlay(instrument,key,sync,extra,method);auto handle=reinterpret_cast<PlayNew>(playOriginal)(instrument,key,sync,extra,method);
    ManualNote note;note.instrument=instrument;note.noteKey=key;note.sustained=true;CaptureInput(note,handle,time);return handle;
}
uint8_t __fastcall StopOldHook(uint32_t handle,uint8_t sync,void* method) {
    double time=NowMs();auto ok=reinterpret_cast<StopOld>(stopOriginal)(handle,sync,method);if (ok) CaptureStop(handle,time);return ok;
}
uint8_t __fastcall StopNewHook(uint32_t handle,uint8_t sync,uint8_t extra,void* method) {
    double time=NowMs();auto ok=reinterpret_cast<StopNew>(stopOriginal)(handle,sync,extra,method);if (ok) CaptureStop(handle,time);return ok;
}
int64_t __fastcall PumpHook(void* method) {
    auto result=pumpOriginal(method);
    if (!pumping.exchange(true)) {
        if (GetCurrentThreadId()==inputThread.load() && onTick) {
            try {onTick(NowMs());}
            catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                calibration.status="Playback dispatcher exception; stop and reload the plugin";
            }
        }
        pumping.store(false);
    }
    return result;
}
bool Thread(std::string& error) {
    if (!installed.load()) {error="Game signatures are not ready";return false;}
    if (!inputThread.load()) {error="Play one note manually in the instrument screen first";return false;}
    if (GetCurrentThreadId()!=inputThread.load()) {error="Refusing playback outside the captured game thread";return false;}
    return true;
}
bool CallUi(UiFn fn,UiContext* ui,int64_t* result) {
    __try { *result=fn(ui->owner,ui->index,ui->method); return true; }
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool CallPlay(int instrument,int key,uint8_t extra,uint32_t* handle) {
    __try {
        *handle=resolved.extraFlag ? reinterpret_cast<PlayNew>(playOriginal)(instrument,key,1,extra,nullptr)
            : reinterpret_cast<PlayOld>(playOriginal)(instrument,key,1,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
bool CallStop(uint32_t handle,uint8_t extra,uint8_t* ok) {
    __try {
        *ok=resolved.extraFlag ? reinterpret_cast<StopNew>(stopOriginal)(handle,1,extra,nullptr)
            : reinterpret_cast<StopOld>(stopOriginal)(handle,1,nullptr);
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
}
}
double NowMs() {
    LARGE_INTEGER counter,frequency; QueryPerformanceCounter(&counter); QueryPerformanceFrequency(&frequency);
    return double(counter.QuadPart)*1000.0/frequency.QuadPart;
}
std::string FormatManualNote(const ManualNote& note) {
    const auto& time=note.localTime;char timestamp[32];
    sprintf_s(timestamp,"[%02u:%02u:%02u.%03u] ",unsigned(time.wHour),unsigned(time.wMinute),unsigned(time.wSecond),unsigned(time.wMilliseconds));
    return std::string(timestamp)+(note.sustained ? "[长音]":"[短音]")+" 乐器id="+std::to_string(note.instrument)+" noteKey="+std::to_string(note.noteKey);
}
bool Bridge::Initialize(const std::function<void(double)>& tick,const std::function<void(const std::string&)>& progress) {
    try {
        auto found=locator::Discover(progress); std::string error;
        if (!Install(found,reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr)),tick,error)) throw std::runtime_error(error);
        return true;
    } catch (const std::exception& e) {
        std::lock_guard<std::mutex> lock(mutex); calibration.status=e.what(); return false;
    }
}
bool Bridge::Install(const locator::Result& result,uintptr_t base,const std::function<void(double)>& tick,std::string& error) {
    if (installed.load()) {error="Instrument hooks are already installed";return false;}
    if (result.shortUi.empty() || result.shortUi.size()>uiOriginal.size()) {error="Unsupported number of UI callbacks";return false;}
    auto mh=MH_Initialize();
    if (mh!=MH_OK && mh!=MH_ERROR_ALREADY_INITIALIZED) {error=MH_StatusToString(mh);return false;}
    resolved=result; onTick=tick;
    auto hook=[&](uint32_t rva,void* detour,void** original) {
        auto target=reinterpret_cast<void*>(base+rva);
        auto status=MH_CreateHook(target,detour,original);
        if (status!=MH_OK) throw std::runtime_error(std::string("CreateHook: ")+MH_StatusToString(status));
        targets.push_back(target);
    };
    void* uiHooks[]={reinterpret_cast<void*>(UiHook<0>),reinterpret_cast<void*>(UiHook<1>),reinterpret_cast<void*>(UiHook<2>),reinterpret_cast<void*>(UiHook<3>),reinterpret_cast<void*>(UiHook<4>),reinterpret_cast<void*>(UiHook<5>),reinterpret_cast<void*>(UiHook<6>),reinterpret_cast<void*>(UiHook<7>)};
    try {
        hook(result.shortPlay,reinterpret_cast<void*>(ShortHook),reinterpret_cast<void**>(&shortOriginal));
        hook(result.emitShort,result.extraFlag ? reinterpret_cast<void*>(EmitNewHook):reinterpret_cast<void*>(EmitOldHook),&emitOriginal);
        hook(result.gamePlay,result.extraFlag ? reinterpret_cast<void*>(PlayNewHook):reinterpret_cast<void*>(PlayOldHook),&playOriginal);
        hook(result.gameStop,result.extraFlag ? reinterpret_cast<void*>(StopNewHook):reinterpret_cast<void*>(StopOldHook),&stopOriginal);
        for (size_t i=0;i<result.shortUi.size();++i) hook(result.shortUi[i],uiHooks[i],reinterpret_cast<void**>(&uiOriginal[i]));
        hook(result.mainThread,reinterpret_cast<void*>(PumpHook),reinterpret_cast<void**>(&pumpOriginal));
        // Only enable hooks owned by this DLL. No MH_ALL_HOOKS operation.
        for (auto target:targets) {
            auto status=MH_QueueEnableHook(target); if (status!=MH_OK) throw std::runtime_error(MH_StatusToString(status));
        }
        if (MH_ApplyQueued()!=MH_OK) throw std::runtime_error("Enable instrument hooks failed");
        installed.store(true);
        std::lock_guard<std::mutex> lock(mutex); calibration.ready=true;
        calibration.status=result.extraFlag ? "Ready: current ABI; play a note to calibrate" : "Ready: legacy ABI; play a note to calibrate";
        return true;
    } catch (const std::exception& e) {error=e.what();Shutdown();return false;}
}
void Bridge::Shutdown() {
    installed.store(false);
    for (auto target:targets) MH_DisableHook(target);
    for (auto target:targets) MH_RemoveHook(target);
    targets.clear(); onTick=nullptr; inputThread.store(0); voices.clear(); captured=UiContext{};
    std::lock_guard<std::mutex> lock(mutex); calibration=Calibration{};
    instrumentExtras.clear();
    manualLogging=false; manualNotes.clear();
    inputCapture=false;inputEvents.clear();inputVoices.clear();lostInput=0;
}
Calibration Bridge::Status() const {std::lock_guard<std::mutex> lock(mutex); return calibration;}
void Bridge::SetManualLogging(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex);
    if (manualLogging!=enabled) manualNotes.clear();
    manualLogging=enabled;
}
std::vector<ManualNote> Bridge::ManualNotes() const {
    std::lock_guard<std::mutex> lock(mutex); return {manualNotes.begin(),manualNotes.end()};
}
void Bridge::SetInputCapture(bool enabled) {
    std::lock_guard<std::mutex> lock(mutex);inputCapture=enabled;inputEvents.clear();inputVoices.clear();lostInput=0;
}
InputBatch Bridge::TakeInput() {
    std::lock_guard<std::mutex> lock(mutex);InputBatch batch;batch.events.assign(inputEvents.begin(),inputEvents.end());batch.lost=lostInput;inputEvents.clear();lostInput=0;return batch;
}
bool Bridge::ManualShort(int instrument,int key,std::string& error) {ManualScope scope;return Short(instrument,key,error);}
player::StartResult Bridge::ManualStart(int instrument,int key,uint32_t& handle,std::string& error) {ManualScope scope;return Start(instrument,key,handle,error);}
bool Bridge::Short(int instrument,int key,std::string& error) {
    if (!Thread(error)) return false;
    UiContext ui;
    {std::lock_guard<std::mutex> lock(mutex);ui=captured;}
    void* klass=nullptr;
    if (!ui.owner || ui.slot<0 || !ClassOf(ui.owner,&klass) || klass!=ui.klass) {
        error="Short-note UI is unavailable; play one short note manually in the current instrument screen"; return false;
    }
    Replay request{instrument,key}; auto previous=replay; replay=&request; inPlugin=true;
    int64_t raw=0; bool ok=CallUi(uiOriginal[ui.slot],&ui,&raw);
    inPlugin=false; replay=previous;
    if (!ok) { error="Original short-note UI callback raised a native exception; recalibrate"; return false; }
    if (!request.entered) { error="Original UI callback skipped local playback; return to the instrument screen and recalibrate";return false; }
    if (!request.synced) { error="Short note entered local playback but no game synchronization entry; playback stopped"; return false; }
    return true;
}
player::StartResult Bridge::Start(int instrument,int key,uint32_t& handle,std::string& error) {
    handle=0;
    if (!Thread(error)) return player::StartResult::Error;
    uint8_t extra=0;
    {std::lock_guard<std::mutex> lock(mutex);auto it=instrumentExtras.find(instrument);if (it!=instrumentExtras.end()) extra=it->second;}
    inPlugin=true;if (manualPlayback) RecordPlay(instrument,key,1,extra,nullptr);
    bool ok=CallPlay(instrument,key,extra,&handle);inPlugin=false;
    if (!ok) {error="长音播放发生原生异常，请检查演奏界面和配置";return player::StartResult::Error;}
    if (!handle) {error="游戏返回空句柄，可能无此音符";return player::StartResult::Rejected;}
    if (voices.count(handle)) {error="Game reused an active playback handle";return player::StartResult::Error;}
    voices[handle]={extra,GetCurrentThreadId()};return player::StartResult::Started;
}
bool Bridge::Stop(uint32_t handle,std::string& error) {
    if (!Thread(error)) return false;
    auto it=voices.find(handle); if (it==voices.end()) {error="Handle is not owned by this plugin";return false;}
    if (it->second.thread!=GetCurrentThreadId()) {error="Wrong voice thread";return false;}
    uint8_t ok=0; inPlugin=true; bool safe=CallStop(handle,it->second.extra,&ok); inPlugin=false;
    if (!safe || !ok) {error="Game refused to stop a plugin-owned note";return false;}
    voices.erase(it);return true;
}
}
