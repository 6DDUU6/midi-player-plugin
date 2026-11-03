#include "Composer.h"
#include "NotePalette.h"
#include "InstrumentConfig.h"
#include "ImGuiHost.h"
#include <commdlg.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <stdexcept>

namespace composer {
namespace {
float S(float v) {return ui::Scale(v);}
bool Active(player::State state) {return state==player::State::Playing || state==player::State::Countdown || state==player::State::Paused;}
ImU32 Color(const composition::Track& t,int alpha=255) {return ImGui::ColorConvertFloat4ToU32(ImVec4(t.color[0],t.color[1],t.color[2],alpha/255.f));}
void Write(const std::wstring& path,const std::vector<uint8_t>& bytes) {
    auto temp=path+L".tmp."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(GetTickCount64());
    {std::ofstream out(temp,std::ios::binary|std::ios::trunc);out.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());out.close();if (!out) {DeleteFileW(temp.c_str());throw std::runtime_error("无法完整写入文件，请检查目录权限和剩余空间。");}}
    if (!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) {DeleteFileW(temp.c_str());throw std::runtime_error("无法保存文件，请检查目录权限或文件是否被占用。");}
}
}
void View::Select(int track,bool clear) {
    if (track<0 || track>=int(project.tracks.size())) return;selected_=track;note_=0;inspectorRequest_=0;
    if (clear) selection_.clear();
    snprintf(name_,sizeof(name_),"%s",project.tracks[track].name.c_str());instrumentui::SetInstrumentText(instrument_,sizeof(instrument_),project.tracks[track].instrument);
}
composition::Note* View::Note() {if (selected_<0 || selected_>=int(project.tracks.size())) return nullptr;for (auto& n:project.tracks[selected_].notes) if (n.id==note_) return &n;return nullptr;}
void View::FocusSelection(unsigned preferred) {
    note_=0;
    for (size_t i=0;i<project.tracks.size();++i) for (const auto& n:project.tracks[i].notes) if (selection_.count(n.id) && (!preferred || n.id==preferred)) {
        Select(int(i),false);note_=n.id;newLength_=n.length;inspectorRequest_=1;return;
    }
}
void View::PruneSelection() {
    std::set<unsigned> visible;
    for (const auto& track:project.tracks) if (track.visible) for (const auto& n:track.notes) if (selection_.count(n.id)) visible.insert(n.id);
    selection_=std::move(visible);if (!selection_.count(note_)) FocusSelection();
}
void View::BeginDrag(bool resize,bool remembered) {
    auto n=Note();if (!n) return;original_=*n;dragging_=true;resizing_=resize && selection_.size()==1;
    dragRemembered_=remembered;deselectOnRelease_=false;dragTick_=dragRow_=0;dragFirstTick_=project.beats*composition::PPQ;dragEndTick_=0;dragLowRow_=20;dragHighRow_=0;
    for (const auto& track:project.tracks) for (const auto& item:track.notes) if (selection_.count(item.id)) {
        dragFirstTick_=std::min(dragFirstTick_,item.start);dragEndTick_=std::max(dragEndTick_,item.start+item.length);
        dragLowRow_=std::min(dragLowRow_,item.row);dragHighRow_=std::max(dragHighRow_,item.row);
    }
    dragX_=ImGui::GetIO().MousePos.x;dragY_=ImGui::GetIO().MousePos.y;
}
void View::Load(const std::wstring& path) {
    draftPath_=path;
    if (GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) try {
        std::ifstream in(path,std::ios::binary|std::ios::ate);auto size=in.tellg();if (!in || size<0 || size>8*1024*1024) throw std::runtime_error("无法读取创作草稿。");
        in.seekg(0);std::vector<uint8_t> bytes(static_cast<size_t>(size));if (!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("创作草稿读取不完整。");project=composition::Project::ReadDraft(bytes);
    }catch (const std::exception& e) {error_=e.what();}
    Select(0);
}
void View::Remember() {if (undo_.size()>=40) undo_.erase(undo_.begin());undo_.push_back(project);redo_.clear();transportLoaded_=false;}
void View::Save() {if (draftPath_.empty()) return;try {Write(draftPath_,project.Draft());}catch (const std::exception& e) {error_=e.what();}}
void View::Undo(bool redo) {
    auto& from=redo ? redo_:undo_;auto& to=redo ? undo_:redo_;if (from.empty()) return;
    to.push_back(project);project=std::move(from.back());from.pop_back();Select(std::min(selected_,int(project.tracks.size())-1));dragging_=false;transportLoaded_=false;positionBeat_=std::min(positionBeat_,double(project.beats));error_.clear();Save();
}
void View::Remove() {
    if (selection_.empty()) return;Remember();
    for (auto& track:project.tracks) {auto& notes=track.notes;notes.erase(std::remove_if(notes.begin(),notes.end(),[&](const composition::Note& n) {return selection_.count(n.id)!=0;}),notes.end());}
    selection_.clear();note_=0;error_.clear();Save();
}
void View::Copy(bool cut) {
    if (selection_.empty()) return;
    try {
        std::vector<ClipboardTrack> copied;int firstTrack=-1,firstTick=project.beats*composition::PPQ;size_t count=0;
        for (size_t i=0;i<project.tracks.size();++i) {
            auto track=project.tracks[i];auto& notes=track.notes;
            notes.erase(std::remove_if(notes.begin(),notes.end(),[&](const composition::Note& n) {return !selection_.count(n.id);}),notes.end());
            if (notes.empty()) continue;if (firstTrack<0) firstTrack=int(i);
            for (const auto& n:notes) firstTick=std::min(firstTick,n.start);
            count+=notes.size();copied.push_back({int(i)-firstTrack,std::move(track)});
        }
        if (copied.empty()) return;
        for (auto& track:copied) for (auto& n:track.track.notes) {n.id=0;n.start-=firstTick;}
        clipboard_=std::move(copied);error_.clear();if (cut) Remove();
        notice_=std::string(cut ? "已剪切 ":"已复制 ")+std::to_string(count)+" 个音符 · 移动黄色指针后按 Ctrl+V 粘贴";
    }catch (const std::exception& e) {error_=e.what();}
}
void View::Paste() {
    if (clipboard_.empty()) {error_="请先选中音符并按 Ctrl+C 或 Ctrl+X。";return;}
    try {
        int firstTick=int(std::llround(positionBeat_*composition::PPQ)),lastTrack=selected_+clipboard_.back().offset;
        if (lastTrack>=composition::MaxTracks) throw std::runtime_error("粘贴后音轨数将超过 128 条，请选择更靠前的目标音轨。");
        auto pasted=project;size_t originalTracks=pasted.tracks.size();std::set<unsigned> selected;unsigned firstNote=0;
        while (int(pasted.tracks.size())<=lastTrack) pasted.AddTrack();
        for (const auto& source:clipboard_) {
            int target=selected_+source.offset;
            if (size_t(target)>=originalTracks) {auto id=pasted.tracks[target].id;pasted.tracks[target]=source.track;pasted.tracks[target].id=id;pasted.tracks[target].notes.clear();}
            for (const auto& n:source.track.notes) {
                int start=firstTick+n.start;
                if (start>project.beats*composition::PPQ-n.length) throw std::runtime_error("粘贴音符将超出时间轴，整组未粘贴；请移动黄色指针或增加总拍数。");
                auto id=pasted.AddNote(target,n.row,start,n.length,n.velocity);selected.insert(id);if (!firstNote) firstNote=id;
            }
            pasted.tracks[target].visible=true;
        }
        Remember();project=std::move(pasted);selection_=std::move(selected);FocusSelection(firstNote);
        error_.clear();notice_="已粘贴 "+std::to_string(selection_.size())+" 个音符 · 可撤销整次粘贴";Save();
    }catch (const std::exception& e) {error_=std::string("粘贴失败：")+e.what();}
}
void View::StopAudition() {
    if (std::any_of(auditionEnds_.begin(),auditionEnds_.end(),[](double end) {return end>0;})) {
        app::Command command;command.action=app::Action::ManualRelease;app::Send(command);auditionEnds_.fill(0);
    }
}
void View::Release() {
    StopRecording();StopAudition();seeking_=false;panning_=scrolling_=false;
    if (dragging_) {dragging_=false;Save();}
}
void View::LoseFocus() {
    if (!recording_ || !receiveGame_) {Release();return;}
    Tick();ReleaseRecordingVoices();recordHeld_.fill(false);StopAudition();panning_=scrolling_=false;
    if (seeking_) {recordStartMs_=project.Time(positionBeat_);recordClock_=game::NowMs();seeking_=false;}
    if (dragging_) {dragging_=false;Save();}
}
void View::Tick() {
    if (!recording_) return;RecordGameInput();UpdateRecording();
    if (positionBeat_>=project.beats) StopRecording();
}
void View::ResetPlayback() {StopRecording();positionBeat_=0;seeking_=false;transportLoaded_=false;}
void View::Seek(double beat,const app::Status& status) {
    positionBeat_=std::max(0.,std::min(double(project.beats),beat));
    if (recording_) {recordStartMs_=project.Time(positionBeat_);recordClock_=game::NowMs();gameVoices_.clear();app::SetGameInputCapture(receiveGame_);return;}
    if (!Active(status.transport.state)) {transportLoaded_=false;return;}
    if (!transportLoaded_) return;
    app::Command command;command.action=app::Action::Seek;command.value=project.Time(positionBeat_);
    if (!app::Send(command)) error_="跳转命令队列已满。";else error_.clear();
}
void View::StartRecording() {
    Release();app::Command stop;stop.action=app::Action::Stop;
    if (!app::Send(stop)) {error_="命令队列已满。";return;}
    if (positionBeat_>=project.beats) positionBeat_=0;
    recording_=true;transportLoaded_=false;recordRemembered_=false;recordedNotes_=0;skippedGameNotes_=0;recordMouseRow_=-1;gameVoices_.clear();
    recordHeld_.fill(false);recordVoices_.fill(RecordVoice{});recordStartMs_=project.Time(positionBeat_);recordClock_=game::NowMs();error_.clear();notice_.clear();
    app::SetGameInputCapture(receiveGame_);
}
void View::ExtendRecordedNote(const RecordVoice& voice,int endTick) {
    if (!voice.note) return;auto& notes=project.tracks[voice.track].notes;composition::Note* note=nullptr;
    for (auto& n:notes) if (n.id==voice.note) {note=&n;break;}
    if (!note) return;int end=project.beats*composition::PPQ;
    for (const auto& n:notes) if (n.id!=note->id && n.row==note->row && n.start>note->start) end=std::min(end,n.start);
    note->length=std::max(1,std::min(end,endTick)-note->start);
}
void View::UpdateRecording() {
    if (!recording_ || seeking_) return;
    positionBeat_=project.BeatAt(recordStartMs_+std::max(0.,game::NowMs()-recordClock_));
    int tick=std::min(project.beats*composition::PPQ,int(std::llround(positionBeat_*composition::PPQ)));
    for (const auto& voice:recordVoices_) ExtendRecordedNote(voice,tick);
    for (const auto& voice:gameVoices_) ExtendRecordedNote(voice.second,tick);
}
void View::ReleaseRecordingVoices() {
    app::Command release;release.action=app::Action::ManualRelease;app::Send(release);recordVoices_.fill(RecordVoice{});recordMouseRow_=-1;
}
void View::StopRecording() {
    if (!recording_) return;RecordGameInput();UpdateRecording();app::SetGameInputCapture(false);gameVoices_.clear();ReleaseRecordingVoices();recording_=false;recordHeld_.fill(false);
    if (auto note=Note()) newLength_=note->length;
    if (recordRemembered_) Save();notice_=recordedNotes_ ? "即时创作已写入 "+std::to_string(recordedNotes_)+" 个音符 · 可撤销整次录入":"即时创作已结束，未录入音符。";
    if (skippedGameNotes_) notice_+=" · 游戏音符跳过 "+std::to_string(skippedGameNotes_);
}
unsigned View::AddRecordedNote(int row,int start,int length) {
    composition::Note candidate;candidate.row=row;candidate.start=start;candidate.length=std::min(length,project.beats*composition::PPQ-start);candidate.velocity=velocity_;
    if (!project.Place(selected_,candidate)) {error_="此位置已有同轨的同音高音符，本次录入已跳过。";return 0;}
    try {
        if (!recordRemembered_) {Remember();recordRemembered_=true;}
        auto id=project.AddNote(selected_,row,start,candidate.length,velocity_);project.tracks[selected_].visible=true;++recordedNotes_;
        selection_.clear();selection_.insert(id);note_=id;inspectorRequest_=1;return id;
    }catch (const std::exception& e) {error_=e.what();return 0;}
}
void View::RecordGameInput() {
    if (!recording_ || !receiveGame_) return;auto batch=app::TakeGameInput();
    if (batch.lost) {skippedGameNotes_+=batch.lost;error_="游戏输入缓存超限，部分输入已遗漏。";}
    std::stable_sort(batch.events.begin(),batch.events.end(),[](const game::InputEvent& a,const game::InputEvent& b) {return a.timeMs<b.timeMs;});
    bool changed=false;
    for (const auto& event:batch.events) {
        if (seeking_ || event.timeMs<recordClock_) continue;
        double ms=recordStartMs_+event.timeMs-recordClock_;int tick=int(std::llround(project.BeatAt(ms)*composition::PPQ));
        if (!event.on) {
            auto voice=gameVoices_.find(event.handle);if (voice!=gameVoices_.end()) {ExtendRecordedNote(voice->second,tick);gameVoices_.erase(voice);changed=true;}continue;
        }
        if (tick>=project.beats*composition::PPQ) continue;
        int pitch=60+event.noteKey-project.tracks[selected_].middleDo,row=notepalette::Row(pitch);
        if (row<0) {++skippedGameNotes_;continue;}
        auto id=AddRecordedNote(row,tick,event.sustained ? 1:newLength_);
        if (!id) {++skippedGameNotes_;continue;}changed=true;
        if (event.sustained) gameVoices_[event.handle]=RecordVoice(id,selected_);
    }
    if (changed) Save();
}
void View::RecordInput(const app::Status& status,bool input) {
    if (!recording_) return;const auto& io=ImGui::GetIO();bool released=false;
    for (int row=0;row<notepalette::Count;++row) {
        auto key=ImGuiKey(ImGuiKey_A+notepalette::RowKey(row)-'A');
        bool down=input && !io.WantTextInput && (ImGui::IsKeyDown(key) || (recordMouseRow_==row && ImGui::IsMouseDown(0)));
        if (down && !recordHeld_[row] && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper && !seeking_) {
            auto& track=project.tracks[selected_];int start=int(std::llround(positionBeat_*composition::PPQ));
            if (start<project.beats*composition::PPQ) {
                auto id=AddRecordedNote(row,start,1);if (id) recordVoices_[row]=RecordVoice(id,selected_);
                bool ready=status.preview || (status.game.ready && status.game.threadId && (!player::UsesShort(track.instrument,track.sustained) || status.game.shortReady));
                if (ready) {app::Command command;command.action=app::Action::ManualOn;command.slot=row;command.instrument=track.instrument;command.sustained=track.sustained;command.key=track.middleDo+notepalette::RowPitch(row)-60;if (!app::Send(command)) error_="试听命令队列已满。";}
                else if (recordVoices_[row].note) error_="音符已记录；发声前请先在游戏短音乐器界面弹一个音完成校准。";
            }
        }
        if (!down && recordHeld_[row]) {app::Command off;off.action=app::Action::ManualOff;off.slot=row;app::Send(off);recordVoices_[row]=RecordVoice{};released=true;}
        recordHeld_[row]=down;
    }
    if (released && recordRemembered_) Save();
}
void View::KeyboardInput(const app::Status& status,bool input) {
    const auto& io=ImGui::GetIO();
    if (!input || io.WantTextInput || io.KeyCtrl || io.KeyAlt || io.KeySuper || ImGui::IsKeyDown(ImGuiKey_Space)) return;
    int start=int(std::llround(positionBeat_*composition::PPQ));
    int length=std::min(newLength_,project.beats*composition::PPQ-start);bool remembered=false;
    for (int row=0;row<notepalette::Count;++row) {
        auto key=ImGuiKey(ImGuiKey_A+notepalette::RowKey(row)-'A');
        if (!ImGui::IsKeyPressed(key,false)) continue;
        if (!project.tracks[selected_].visible) {error_="请先打开此轨的显示开关，再添加音符。";continue;}
        if (length<=0) {error_="指针已到时间轴末尾，请先选择添加音符的位置。";continue;}
        composition::Note candidate;candidate.row=row;candidate.start=start;candidate.length=length;candidate.velocity=velocity_;
        if (!project.Place(selected_,candidate)) {error_="此位置已有同轨的同音高音符，请移动黄色指针后再录入。";continue;}
        try {
            if (!remembered) {Remember();selection_.clear();remembered=true;}
            note_=project.AddNote(selected_,row,start,length,velocity_);newLength_=length;
            selection_.insert(note_);inspectorRequest_=1;error_.clear();Audition(status,row,true);
        }catch (const std::exception& e) {error_=e.what();}
    }
    if (remembered) Save();
}
void View::Audition(const app::Status& status,int row,bool chord) {
    const auto& track=project.tracks[selected_];
    bool ready=status.preview || (status.game.ready && status.game.threadId && (!player::UsesShort(track.instrument,track.sustained) || status.game.shortReady));
    if (!ready) {error_="音符已保留；试听前请先在游戏短音乐器界面弹一个音完成校准。";return;}
    if (!chord) StopAudition();
    if (auditionEnds_[row]>0) {app::Command off;off.action=app::Action::ManualOff;off.slot=row;if (!app::Send(off)) {error_="试听命令队列已满。";return;}}
    app::Command command;command.action=app::Action::ManualOn;command.slot=row;command.instrument=track.instrument;
    command.sustained=track.sustained;command.key=track.middleDo+notepalette::RowPitch(row)-60;
    if (!app::Send(command)) {error_="试听命令队列已满。";return;}
    auditionEnds_[row]=game::NowMs()+250;
}
void View::Export(HWND owner) {
    try {
        auto bytes=project.Midi();wchar_t path[32768]=L"创作.mid";OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;
        dialog.lpstrFilter=L"MIDI 文件 (*.mid)\0*.mid\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.lpstrDefExt=L"mid";dialog.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
        if (!GetSaveFileNameW(&dialog)) return;Write(path,bytes);notice_="已导出 MIDI："+ui::Utf8(path);error_.clear();Save();
    }catch (const std::exception& e) {error_=e.what();}
}
void View::Import(HWND owner) {
    wchar_t path[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=owner;
    dialog.lpstrFilter=L"MIDI 文件 (*.mid;*.midi)\0*.mid;*.midi\0所有文件\0*.*\0\0";dialog.lpstrFile=path;dialog.nMaxFile=32768;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&dialog)) return;
    OpenMidi(path);
}
bool View::OpenMidi(const std::wstring& path) {
    try {
        std::ifstream in(path,std::ios::binary|std::ios::ate);auto length=in.tellg();if (!in || length<0 || length>64*1024*1024) throw std::runtime_error("无法读取 MIDI，或文件超过 64 MiB。");
        in.seekg(0);std::vector<uint8_t> bytes(static_cast<size_t>(length));if (!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("MIDI 文件读取不完整。");
        auto parsed=midi::Parse(bytes);for (auto& t:parsed.tracks) t.name=ui::Utf8(ui::Widen(t.name));composition::ImportStats stats;auto imported=composition::Project::Import(parsed,stats);
        Release();Remember();project=std::move(imported);Select(0);ResetPlayback();pan_=0;zoom_=1;error_.clear();Save();
        notice_="已导入 "+std::to_string(stats.imported)+" 音符 · 半音上调 "+std::to_string(stats.converted)+" · 超出音域跳过 "+std::to_string(stats.skipped)+" · 重叠同音高合并 "+std::to_string(stats.merged)+" · 可撤销恢复原草稿";
        return true;
    }catch (const std::exception& e) {error_=e.what();return false;}
}
void View::Play(const app::Status& status) {
    if (!status.preview && (!status.game.ready || !status.game.threadId)) {error_="请先进入游戏短音乐器界面，手动弹一个音完成校准。";return;}
    bool shortNotes=false;for (const auto& t:project.tracks) if (!t.notes.empty() && player::UsesShort(t.instrument,t.sustained)) shortNotes=true;
    if (!status.preview && shortNotes && !status.game.shortReady) {error_="请先在游戏短音乐器界面手动弹一个音。";return;}
    if (!project.NoteCount()) {error_="先在时间轴上添加音符。";return;}
    try {
        auto song=midi::Parse(project.Midi());std::vector<player::TrackConfig> configs(song.tracks.size());configs[0].enabled=false;
        for (size_t i=0;i<project.tracks.size();++i) {auto& c=configs[i+1];const auto& t=project.tracks[i];c.instrument=t.instrument;c.middleDo=t.middleDo;c.sustained=t.sustained;}
        app::Command load;load.action=app::Action::Load;load.plan=player::Build(song,configs,1);
        if (!app::Send(load)) throw std::runtime_error("命令队列已满。");
        if (positionBeat_>=project.beats) positionBeat_=0;
        if (positionBeat_>0) {app::Command seek;seek.action=app::Action::Seek;seek.value=project.Time(positionBeat_);if (!app::Send(seek)) throw std::runtime_error("命令队列已满。");}
        app::Command play;play.action=app::Action::Play;play.value=status.preview ? 0:3000;
        if (!app::Send(play)) throw std::runtime_error("命令队列已满。");transportLoaded_=true;error_.clear();Release();
    }catch (const std::exception& e) {error_=e.what();}
}
void View::TogglePlayback(const app::Status& status) {
    if (recording_) {StopRecording();return;}
    app::Command command;
    if (status.transport.state==player::State::Playing || status.transport.state==player::State::Countdown) {command.action=app::Action::Pause;app::Send(command);}
    else if (transportLoaded_ && status.transport.state==player::State::Paused) {command.action=app::Action::Play;app::Send(command);}
    else Play(status);
}
void View::Sidebar(bool locked) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(S(8),S(5)));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(S(6),S(3)));
    ui::Heading("创作音轨");ImGui::BeginDisabled(locked);
    if (ImGui::Button("+ 添加音轨",ImVec2(-1,0))) {try {Remember();Select(project.AddTrack());error_.clear();Save();}catch (const std::exception& e) {error_=e.what();}}ui::Mark("composer_add_track");
    if (ImGui::SmallButton("显示全部")) {Remember();for (auto& t:project.tracks) t.visible=true;Save();}ui::Mark("composer_show_all");ImGui::SameLine();
    if (ImGui::SmallButton("只显示此轨")) {Remember();for (size_t i=0;i<project.tracks.size();++i) project.tracks[i].visible=int(i)==selected_;PruneSelection();Save();}ui::Mark("composer_solo");
    ImGui::EndDisabled();
    ImGui::BeginChild("composer_track_list",ImVec2(0,std::max(S(100),std::min(S(130),ImGui::GetContentRegionAvail().y*.24f))),ImGuiChildFlags_Borders);
    for (size_t i=0;i<project.tracks.size();++i) {
        auto& t=project.tracks[i];ImGui::PushID(int(t.id));bool visible=t.visible;ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("##visible",&visible)) {Remember();t.visible=visible;PruneSelection();Save();}ImGui::EndDisabled();ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(t.color[0],t.color[1],t.color[2],1));
        auto label=t.name+"  ("+std::to_string(t.notes.size())+")";if (ImGui::Selectable(label.c_str(),selected_==int(i))) Select(int(i));ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s · %s\n显示开关只影响时间轴，不影响导出或播放",t.name.c_str(),ui::Utf8(instrumentui::InstrumentLabel(t.instrument)).c_str(),t.sustained ? "长音":"短音");ImGui::PopID();
    }ImGui::EndChild();ImGui::BeginDisabled(locked);auto& t=project.tracks[selected_];
    if (ImGui::BeginTabBar("composer_inspector")) {
    if (ImGui::BeginTabItem("音轨设置",nullptr,inspectorRequest_==0 ? ImGuiTabItemFlags_SetSelected:0)) {
    ImGui::SetNextItemWidth(-1);if (ImGui::InputTextWithHint("##composer_name","音轨名称",name_,sizeof(name_)) && name_[0]) {Remember();t.name=name_;Save();}
    auto c=player::TrackConfig{};c.instrument=t.instrument;c.middleDo=t.middleDo;c.sustained=t.sustained;
    ImGui::TextUnformatted("游戏乐器");ImGui::SetNextItemWidth(-1);if (instrumentui::InstrumentInput(c,instrument_,sizeof(instrument_))) {
        int id;if (instrumentui::ReadInstrument(instrument_,id)) {Remember();t.instrument=id;t.sustained=c.sustained;error_.clear();Save();}else error_="请选择预设乐器，或输入 1–1000000 的乐器 ID。";
    }
    c.sustained=t.sustained;if (instrumentui::SustainedInput(c)) {Remember();t.sustained=c.sustained;Save();}
    if (ImGui::CollapsingHeader("更多设置")) {
    float color[3]={t.color[0],t.color[1],t.color[2]};if (ImGui::ColorEdit3("轨色",color,ImGuiColorEditFlags_NoInputs)) {Remember();std::copy(color,color+3,t.color);Save();}
    int middle=t.middleDo;ImGui::SetNextItemWidth(S(85));if (ImGui::InputInt("中音 Do 键",&middle,0) && middle>=13 && middle<=999977) {Remember();t.middleDo=middle;Save();}
    if (ImGui::Button("删除此轨")) ImGui::OpenPopup("delete_track");
    if (ImGui::BeginPopup("delete_track")) {
        ImGui::TextWrapped("删除此轨及其全部音符？");if (ImGui::Button("确认删除")) {Remember();project.tracks.erase(project.tracks.begin()+selected_);if (project.tracks.empty()) project.AddTrack();Select(std::min(selected_,int(project.tracks.size())-1));Save();ImGui::CloseCurrentPopup();}ImGui::EndPopup();
    }
    }
    ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("音符设置",nullptr,inspectorRequest_==1 ? ImGuiTabItemFlags_SetSelected:0)) {
    ImGui::Separator();auto note=Note();if (selection_.size()>1) {
        ImGui::TextColored(ImVec4(.7f,.84f,1,1),"已选 %u 个音符",unsigned(selection_.size()));
        ui::Muted("Ctrl 点选添加 / 取消选择。\n拖动已选音符，整体移动时间和音高。");
        if (ImGui::Button("删除所选音符 / Delete")) Remove();ui::Mark("composer_delete_note");
    }else if (note) {
        ImGui::TextColored(ImVec4(.7f,.84f,1,1),"选中：%s · %c",notepalette::RowLabel(note->row),notepalette::RowKey(note->row));
        float start=note->start/float(composition::PPQ),length=note->length/float(composition::PPQ);int velocity=note->velocity;
        ImGui::SetNextItemWidth(S(135));bool change=ImGui::InputFloat("开始 / 拍",&start,.25f,1,"%.2f");
        ImGui::SetNextItemWidth(S(135));change=ImGui::InputFloat("时长 / 拍",&length,.25f,1,"%.2f") || change;
        ImGui::SetNextItemWidth(S(85));change=ImGui::SliderInt("力度",&velocity,1,127) || change;
        if (change && std::isfinite(start) && std::isfinite(length) && start>=0 && length>0 && start+length<=project.beats) {
            auto n=*note;n.start=int(std::llround(start*composition::PPQ));n.length=int(std::llround(length*composition::PPQ));n.velocity=velocity;
            if (project.Place(selected_,n)) {Remember();*note=n;newLength_=n.length;error_.clear();Save();}else error_="同一轨的同音高音符不能重叠。";
        }
        if (ImGui::Button("删除音符 / Delete")) Remove();ui::Mark("composer_delete_note");
    }else ui::Muted("点击空白添加音符。\n点击音符查看开始、时长和力度。");
    ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
    }
    inspectorRequest_=-1;
    ImGui::EndDisabled();
    ImGui::PopStyleVar(2);
}
void View::Scrollbar(bool input,float width,float keyWidth) {
    const auto& io=ImGui::GetIO();float span=std::max(1.f,project.beats/zoom_),maxPan=std::max(0.f,project.beats-span);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX()+keyWidth);
    float barWidth=std::max(1.f,width-keyWidth),barHeight=S(16);
    ImGui::InvisibleButton("composer_horizontal_scrollbar",ImVec2(barWidth,barHeight));ui::Mark("composer_scrollbar");
    auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();bool hovered=ImGui::IsItemHovered();
    float thumbWidth=std::min(barWidth,std::max(S(24),barWidth*span/project.beats)),travel=barWidth-thumbWidth;
    float thumbX=a.x+(maxPan>0 ? pan_/maxPan*travel:0);
    if (input && hovered && ImGui::IsMouseClicked(0) && maxPan>0 && travel>0) {
        scrollOffset_=io.MousePos.x>=thumbX && io.MousePos.x<=thumbX+thumbWidth ? io.MousePos.x-thumbX:thumbWidth*.5f;scrolling_=true;
    }
    if (scrolling_) {
        if (!input || !ImGui::IsMouseDown(0) || maxPan<=0 || travel<=0) scrolling_=false;
        else pan_=std::max(0.f,std::min(maxPan,(io.MousePos.x-a.x-scrollOffset_)/travel*maxPan));
    }
    thumbX=a.x+(maxPan>0 ? pan_/maxPan*travel:0);auto draw=ImGui::GetWindowDrawList();
    draw->AddRectFilled(a,b,IM_COL32(25,33,42,255),S(5));
    draw->AddRectFilled(ImVec2(thumbX,a.y+S(2)),ImVec2(thumbX+thumbWidth,b.y-S(2)),maxPan<=0 ? IM_COL32(55,67,79,255):scrolling_ ? IM_COL32(97,151,189,255):hovered ? IM_COL32(82,120,151,255):IM_COL32(66,91,114,255),S(4));
    if (hovered) ImGui::SetTooltip("横向滚动时间轴 · %.2f–%.2f 拍\n也可按住空格，用鼠标拖动时间轴",pan_,pan_+span);
}
void View::Grid(const app::Status& status,bool focused,bool locked) {
    const auto& io=ImGui::GetIO();float span=project.beats/zoom_;span=std::max(1.f,span);pan_=std::max(0.f,std::min(float(project.beats)-span,pan_));
    ImGui::BeginChild("composer_roll",ImVec2(0,0),ImGuiChildFlags_Borders,ImGuiWindowFlags_NoNavInputs|ImGuiWindowFlags_AlwaysVerticalScrollbar|((io.KeyCtrl || io.KeyShift) ? ImGuiWindowFlags_NoScrollWithMouse:0));
    auto origin=ImGui::GetCursorScreenPos();float width=ImGui::GetContentRegionAvail().x,keyWidth=S(100),rulerHeight=S(30);
    float rowHeight=std::max(S(14),std::min(S(24),(ImGui::GetContentRegionAvail().y-rulerHeight-S(16)-ImGui::GetStyle().ItemSpacing.y)/21));
    float left=origin.x+keyWidth,right=origin.x+width,top=origin.y+rulerHeight,bottom=top+21*rowHeight;
    float beatWidth=std::max(1.f,(right-left)/span);auto draw=ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("composer_canvas",ImVec2(width,rulerHeight+21*rowHeight),ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);ui::Mark("composer_grid");
    bool hovered=ImGui::IsItemHovered();ui::MarkRect("composer_ruler",ImVec2(left,origin.y),ImVec2(right,top));
    bool transportInput=focused && !io.AppFocusLost && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
    bool panMode=transportInput && !io.WantTextInput && ImGui::IsKeyDown(ImGuiKey_Space) && !dragging_ && !seeking_;
    if (panMode && hovered && ImGui::IsMouseClicked(0)) {panning_=true;panMouseX_=io.MousePos.x;panAnchor_=pan_;}
    if (panning_) {
        if (!panMode || !ImGui::IsMouseDown(0)) panning_=false;
        else pan_=std::max(0.f,std::min(float(project.beats)-span,panAnchor_-(io.MousePos.x-panMouseX_)/beatWidth));
    }
    bool rulerHovered=hovered && io.MousePos.x>=left && io.MousePos.y<top;
    if (transportInput && rulerHovered && !dragging_ && !panMode && ImGui::IsMouseClicked(0)) {if (recording_) {ReleaseRecordingVoices();gameVoices_.clear();app::SetGameInputCapture(receiveGame_);}seeking_=true;}
    if (seeking_) {
        if (!transportInput) seeking_=false;
        else {
            positionBeat_=std::max(0.,std::min(double(project.beats),double(pan_+(io.MousePos.x-left)/beatWidth)));
            if (!ImGui::IsMouseDown(0)) {seeking_=false;Seek(positionBeat_,status);}
        }
    }
    if (rulerHovered || seeking_) {ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);ImGui::SetTooltip("点击 / 拖动选择播放位置，松开后跳转\n%.2f 拍 · %s",positionBeat_,ui::Time(project.Time(positionBeat_),true).c_str());}
    draw->PushClipRect(origin,ImVec2(right,bottom),true);
    draw->AddRectFilled(origin,ImVec2(right,bottom),IM_COL32(20,26,33,255));
    for (int row=0;row<21;++row) {
        float y=top+row*rowHeight;draw->AddRectFilled(ImVec2(origin.x,y),ImVec2(right,y+rowHeight),(row/7)%2 ? IM_COL32(26,34,43,255):IM_COL32(23,29,38,255));
        draw->AddRectFilled(ImVec2(origin.x,y),ImVec2(left,y+rowHeight),auditionEnds_[row]>0 || recordVoices_[row].note ? IM_COL32(53,104,113,255):IM_COL32(34,44,54,255));
        if (row==13) ui::MarkRect("composer_piano",ImVec2(origin.x,y),ImVec2(left,y+rowHeight));
        char label[64];sprintf_s(label,"%c  %s",notepalette::RowKey(row),notepalette::RowLabel(row));
        draw->AddText(ImGui::GetFont(),std::min(ImGui::GetFontSize(),rowHeight-S(2)),ImVec2(origin.x+S(6),y+S(1)),IM_COL32(180,199,213,255),label);draw->AddLine(ImVec2(origin.x,y+rowHeight),ImVec2(right,y+rowHeight),IM_COL32(44,53,63,255));
    }
    int snapTicks=composition::PPQ/(1<<snap_);float gridStep=1.f/(1<<snap_);
    for (float b=std::ceil(pan_/gridStep)*gridStep;b<=pan_+span+.001f;b+=gridStep) {
        float x=left+(b-pan_)*beatWidth;bool whole=std::abs(b-std::round(b))<.001f,bar=whole && int(std::round(b))%4==0;
        draw->AddLine(ImVec2(x,whole ? origin.y:top),ImVec2(x,bottom),bar ? IM_COL32(95,113,130,200):whole ? IM_COL32(66,82,98,200):IM_COL32(43,53,64,100));
        if (whole && beatWidth>=S(26)) {char label[32];sprintf_s(label,"%d.%d",int(b)/4+1,int(b)%4+1);draw->AddText(ImVec2(x+S(3),origin.y+S(5)),IM_COL32(172,188,206,255),label);}
    }
    int hitTrack=-1;unsigned hit=0;bool edge=false;
    double playbackTick=project.BeatAt(status.transport.positionMs)*composition::PPQ;
    for (int pass=0;pass<2;++pass) for (size_t i=0;i<project.tracks.size();++i) {
        const auto& track=project.tracks[i];if (!track.visible || ((int(i)==selected_)!=(pass==1))) continue;
        for (const auto& n:track.notes) {
            float x1=left+(n.start/float(composition::PPQ)-pan_)*beatWidth,x2=x1+std::max(S(2),n.length/float(composition::PPQ)*beatWidth);
            if (x2<left || x1>right) continue;float y=top+n.row*rowHeight;
            bool current=int(i)==selected_,chosen=selection_.count(n.id)!=0;ImVec2 a(std::max(left,x1),y+S(2)),b(std::min(right,x2),y+rowHeight-S(3));
            bool active=Active(status.transport.state) && n.start<=playbackTick && n.start+n.length>playbackTick;
            draw->AddRectFilled(a,b,Color(track,active || chosen ? 255:current ? 230:150),S(2));
            if (current || chosen) {draw->AddRect(a,b,chosen ? IM_COL32(255,236,179,255):Color(track),S(2),0,S(chosen ? 2.f:1.f));if (x2-x1>S(36)) draw->AddText(ImVec2(a.x+S(4),a.y),IM_COL32(17,27,32,255),notepalette::RowLabel(n.row));}
            if (chosen && n.id==note_) ui::MarkRect("composer_note",a,b);
            if (&n==&track.notes.front()) ui::MarkRect(i==0 ? "composer_first_note":"composer_other_note",a,b);
            if (hovered && io.MousePos.x>=a.x && io.MousePos.x<=b.x && io.MousePos.y>=a.y && io.MousePos.y<=b.y) {hitTrack=int(i);hit=n.id;edge=io.MousePos.x>=x2-S(7);}
        }
    }
    float playhead=left+(float(positionBeat_)-pan_)*beatWidth;
    if (playhead>=left && playhead<=right) {
        const ImU32 yellow=IM_COL32(255,213,100,255);
        draw->AddLine(ImVec2(playhead,origin.y+S(10)),ImVec2(playhead,bottom),yellow,S(2));
        draw->AddTriangleFilled(ImVec2(playhead-S(6),origin.y+S(2)),ImVec2(playhead+S(6),origin.y+S(2)),ImVec2(playhead,origin.y+S(10)),yellow);
    }
    draw->PopClipRect();
    if (recording_) {
        if (transportInput && !panMode && hovered && ImGui::IsMouseClicked(0) && io.MousePos.x<left && io.MousePos.y>=top && io.MousePos.y<bottom) recordMouseRow_=int((io.MousePos.y-top)/rowHeight);
        if (!ImGui::IsMouseDown(0)) recordMouseRow_=-1;
        RecordInput(status,transportInput && !panMode);
    }
    bool input=transportInput && !locked && !seeking_ && !panMode && !scrolling_;int row=int((io.MousePos.y-top)/rowHeight);
    if (hit && hovered) {
        const composition::Note* n=nullptr;for (const auto& item:project.tracks[hitTrack].notes) if (item.id==hit) n=&item;
        if (n) ImGui::SetTooltip("%s · %c / %s\n开始 %.2f 拍 · 时长 %.2f 拍 · 力度 %d\nCtrl 点选增减多选 · 拖动移动 · 单选拖右边缘改时长 · 右键删除\nCtrl+C 复制 · Ctrl+X 剪切 · Ctrl+V 粘贴到黄色指针",project.tracks[hitTrack].name.c_str(),notepalette::RowKey(n->row),notepalette::RowLabel(n->row),n->start/double(composition::PPQ),n->length/double(composition::PPQ),n->velocity);
        ImGui::SetMouseCursor(edge && !(selection_.size()>1 && selection_.count(hit)) ? ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_Hand);
    }
    if (input && hovered && ImGui::IsMouseClicked(1) && hit) {if (!selection_.count(hit)) {selection_.clear();selection_.insert(hit);}FocusSelection(hit);Remove();}
    if (input && hovered && ImGui::IsMouseClicked(0)) {
        if (row>=0 && row<21 && io.MousePos.y>=top) {
            if (io.MousePos.x<left) {
                Audition(status,row);
            }else if (hit) {
                bool chosen=selection_.count(hit)!=0;
                if (!io.KeyCtrl && !chosen) selection_.clear();selection_.insert(hit);FocusSelection(hit);BeginDrag(edge);
                deselectOnRelease_=io.KeyCtrl && chosen;
            }
            else if (!io.KeyCtrl && project.tracks[selected_].visible) {
                int start=int(std::floor((pan_+(io.MousePos.x-left)/beatWidth)*composition::PPQ/snapTicks))*snapTicks;
                int length=std::min(newLength_,project.beats*composition::PPQ-start);
                if (start>=0 && length>0) try {Remember();note_=project.AddNote(selected_,row,start,length,velocity_);newLength_=length;selection_.clear();selection_.insert(note_);inspectorRequest_=1;BeginDrag(true,true);error_.clear();Save();Audition(status,row);}catch (const std::exception& e) {error_=e.what();}
            }else if (!io.KeyCtrl) error_="请先打开此轨的显示开关，再添加音符。";
        }
    }
    if (dragging_) {
        auto n=Note();if (n && input && ImGui::IsMouseDragging(0,S(3))) {
            deselectOnRelease_=false;int delta=int(std::llround((io.MousePos.x-dragX_)/beatWidth*composition::PPQ/snapTicks))*snapTicks;
            if (resizing_) {
                auto moved=original_;moved.length=std::max(snapTicks,std::min(project.beats*composition::PPQ-moved.start,original_.length+delta));
                if (moved.length!=n->length && project.Place(selected_,moved)) {if (!dragRemembered_) {Remember();dragRemembered_=true;}*n=moved;newLength_=moved.length;error_.clear();}
            }else {
                delta=std::max(-dragFirstTick_,std::min(project.beats*composition::PPQ-dragEndTick_,delta));
                int rowDelta=std::max(-dragLowRow_,std::min(20-dragHighRow_,int(std::llround((io.MousePos.y-dragY_)/rowHeight))));
                if (delta!=dragTick_ || rowDelta!=dragRow_) {
                    bool placed;
                    if (!dragRemembered_) {auto moved=project;placed=moved.MoveNotes(selection_,delta-dragTick_,rowDelta-dragRow_);if (placed) {Remember();project=std::move(moved);dragRemembered_=true;}}
                    else placed=project.MoveNotes(selection_,delta-dragTick_,rowDelta-dragRow_);
                    if (placed) {dragTick_=delta;dragRow_=rowDelta;error_.clear();}else error_="移动后会与同轨的同音高音符重叠，请调整位置。";
                }
            }
        }
        if (!input || !ImGui::IsMouseDown(0)) {dragging_=false;if (deselectOnRelease_ && input) {selection_.erase(note_);FocusSelection();}if (dragRemembered_) Save();}
    }
    if (!recording_) KeyboardInput(status,input && !dragging_);
    if (!input) StopAudition();else for (int i=0;i<notepalette::Count;++i) if (auditionEnds_[i]>0 && game::NowMs()>=auditionEnds_[i]) {
        app::Command off;off.action=app::Action::ManualOff;off.slot=i;if (app::Send(off)) auditionEnds_[i]=0;
    }
    if (transportInput && hovered && io.MouseWheel!=0 && !dragging_ && !seeking_) {
        if (io.KeyCtrl) {float anchor=std::max(0.f,std::min(1.f,(io.MousePos.x-left)/(right-left))),at=pan_+anchor*span;zoom_=std::max(1.f,std::min(32.f,zoom_*float(std::pow(1.25,io.MouseWheel))));pan_=at-anchor*std::max(1.f,project.beats/zoom_);}
        else if (io.KeyShift) pan_-=io.MouseWheel*span*.1f;
    }
    if (panMode && hovered) {ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);ImGui::SetTooltip("按住空格 + 鼠标左键拖动，横向平移时间轴");}
    Scrollbar(transportInput && !dragging_ && !seeking_ && !panning_,width,keyWidth);
    ImGui::EndChild();
}
void View::Draw(const app::Status& status,HWND owner,float height,bool focused) {
    if (transportLoaded_ && !seeking_ && (Active(status.transport.state) || status.transport.state==player::State::Finished || status.transport.state==player::State::Error)) positionBeat_=project.BeatAt(status.transport.positionMs);
    positionBeat_=std::max(0.,std::min(double(project.beats),positionBeat_));
    bool locked=recording_ || Active(status.transport.state);ImGui::BeginChild("composer_workspace",ImVec2(0,height),ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(locked);if (ImGui::Button("新建")) ImGui::OpenPopup("new_composition");
    if (ImGui::BeginPopup("new_composition")) {ImGui::TextWrapped("清空当前创作并新建？可通过撤销恢复。");if (ImGui::Button("确认新建")) {Remember();project=composition::Project{};Select(0);ResetPlayback();pan_=0;Save();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    ImGui::SameLine();ImGui::BeginDisabled(undo_.empty());if (ImGui::Button("撤销")) Undo(false);ui::Mark("composer_undo");ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(redo_.empty());if (ImGui::Button("重做")) Undo(true);ImGui::EndDisabled();
    ImGui::SameLine();ImGui::SetNextItemWidth(S(85));int beats=project.beats;if (ImGui::InputInt("总拍数",&beats,0) && beats>=1 && beats<=composition::MaxBeats) {
        int end=0;for (const auto& t:project.tracks) for (const auto& n:t.notes) end=std::max(end,n.start+n.length);
        if (beats*composition::PPQ>=end) {Remember();project.beats=beats;for (auto it=project.beatTimes.begin();it!=project.beatTimes.end();) if (it->first>=beats) it=project.beatTimes.erase(it);else ++it;for (auto it=project.tempoChanges.begin();it!=project.tempoChanges.end();) if (it->first>=beats*composition::PPQ) it=project.tempoChanges.erase(it);else ++it;positionBeat_=std::min(positionBeat_,double(beats));Save();}else error_="总拍数不能小于现有音符的结束位置。";
    }
    ImGui::SameLine();ImGui::SetNextItemWidth(S(100));float ms=float(project.beatMs);if (ImGui::InputFloat("每拍 / 毫秒",&ms,0,0,"%.1f") && std::isfinite(ms) && ms>=10 && ms<=16000) {Remember();project.beatMs=ms;Save();}ui::Mark("composer_beat_ms");
    ImGui::SameLine();ImGui::SetNextItemWidth(S(85));float bpm=float(60000/project.beatMs);if (ImGui::InputFloat("BPM",&bpm,0,0,"%.2f") && std::isfinite(bpm) && bpm>=3.75f && bpm<=6000) {Remember();project.beatMs=60000/double(bpm);Save();}ui::Mark("composer_bpm");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("每拍毫秒 = 60000 / BPM · 范围 3.75–6000\n设置默认拍长，原有速度变化仍保留；“统一拍长”可统一全曲");
    ImGui::SameLine();if (ImGui::Button("导入 MIDI…")) Import(owner);ui::Mark("composer_import");ImGui::EndDisabled();ImGui::SameLine();
    if (ImGui::Button("导出 MIDI…")) Export(owner);ui::Mark("composer_export");
    ImGui::BeginDisabled(locked);const char* snaps[]={"1 拍","1/2 拍","1/4 拍","1/8 拍","1/16 拍"};ImGui::SetNextItemWidth(S(110));ImGui::Combo("网格",&snap_,snaps,5);
    ImGui::SameLine();ImGui::SetNextItemWidth(S(90));float newLength=newLength_/float(composition::PPQ);if (ImGui::InputFloat("新音时长 / 拍",&newLength,0,0,"%.3f") && std::isfinite(newLength) && newLength>0 && newLength<=composition::MaxBeats) newLength_=std::max(1,int(std::llround(newLength*composition::PPQ)));ImGui::SameLine();ImGui::SetNextItemWidth(S(65));ImGui::SliderInt("新音力度",&velocity_,1,127);ImGui::EndDisabled();
    ImGui::SameLine();if (ImGui::Button("全曲")) {zoom_=1;pan_=0;}ImGui::SameLine();ImGui::SetNextItemWidth(S(110));ImGui::SliderFloat("缩放",&zoom_,1,32,"%.1fx",ImGuiSliderFlags_Logarithmic);
    float span=std::max(1.f,project.beats/zoom_),maxPan=std::max(0.f,project.beats-span);pan_=std::max(0.f,std::min(maxPan,pan_));ImGui::SameLine();ImGui::SetNextItemWidth(S(110));ImGui::SliderFloat("起点 / 拍",&pan_,0,maxPan,"%.1f");
    ImGui::BeginDisabled(recording_);if (ImGui::Button(status.transport.state==player::State::Playing || status.transport.state==player::State::Countdown ? "暂停":"播放创作")) TogglePlayback(status);ui::Mark("composer_play");ImGui::EndDisabled();
    ImGui::SameLine();if (ImGui::Button("停止")) {app::Command c;c.action=app::Action::Stop;if (app::Send(c)) ResetPlayback();}ui::Mark("composer_stop");
    ImGui::SameLine();ImGui::BeginDisabled(Active(status.transport.state));if (ImGui::Button(recording_ ? "结束即时创作":"即时创作")) {if (recording_) StopRecording();else StartRecording();}ui::Mark("composer_record");ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(recording_);ImGui::Checkbox("接收游戏弹奏",&receiveGame_);ui::Mark("composer_receive_game");ImGui::EndDisabled();
    if (!project.tempoChanges.empty() || !project.beatTimes.empty()) {ImGui::SameLine();ImGui::BeginDisabled(locked);if (ImGui::Button("统一拍长")) {Remember();project.tempoChanges.clear();project.beatTimes.clear();Save();}ImGui::EndDisabled();}
    ImGui::SameLine();ImGui::TextDisabled("位置 %s",ui::Time(project.Time(positionBeat_),true).c_str());
    if (recording_) {ImGui::SameLine();ImGui::TextColored(ImVec4(1,.8f,.4f,1),receiveGame_ ? "录入中 · 游戏 + 插件键盘":"录入中 · 插件键盘");}
    else if (!locked) {ImGui::SameLine();ImGui::TextDisabled("三排键盘录入 · 空格 + 拖动平移");}
    ImGui::TextDisabled("%u 轨 · %u 音符 · %s · 点击添加 / 拖动移动 / 拖右边缘改时长 / 右键删除",unsigned(project.tracks.size()),unsigned(project.NoteCount()),ui::Time(project.Time(project.beats)).c_str());
    if (!error_.empty()) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"%s",error_.c_str());else if (!notice_.empty()) ImGui::TextWrapped("%s",notice_.c_str());
    if (!status.manualError.empty()) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"试听：%s",ui::Utf8(ui::Widen(status.manualError)).c_str());
    if (status.transport.state==player::State::Countdown) ImGui::TextColored(ImVec4(1,.8f,.45f,1),"倒计时 %.1f 秒 · 请切回游戏演奏界面",status.transport.countdownMs/1000.);
    else if (status.transport.state==player::State::Error) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"演奏失败：%s",ui::Utf8(ui::Widen(status.transport.error)).c_str());
    if (recording_ && !seeking_ && !panning_ && !scrolling_ && (positionBeat_<pan_ || positionBeat_>pan_+span*.9f)) pan_=std::max(0.f,std::min(maxPan,float(positionBeat_)-span*.15f));
    if (ImGui::BeginTable("composer_layout",2,ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("tracks",ImGuiTableColumnFlags_WidthFixed,S(245));ImGui::TableSetupColumn("roll",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();float body=std::max(S(220),ImGui::GetContentRegionAvail().y);ImGui::BeginChild("composer_sidebar",ImVec2(0,body));Sidebar(locked);ImGui::EndChild();
        ImGui::TableNextColumn();ImGui::BeginChild("composer_grid_holder",ImVec2(0,body));Grid(status,focused,locked);ImGui::EndChild();ImGui::EndTable();
    }
    const auto& io=ImGui::GetIO();
    bool editKeys=focused && !locked && !io.AppFocusLost && !io.WantTextInput && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) && !dragging_ && !seeking_ && !panning_ && !scrolling_ && !ImGui::IsKeyDown(ImGuiKey_Space);
    if (editKeys && io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
        if (ImGui::IsKeyPressed(ImGuiKey_C,false)) Copy(false);
        else if (ImGui::IsKeyPressed(ImGuiKey_X,false)) Copy(true);
        else if (ImGui::IsKeyPressed(ImGuiKey_V,false)) Paste();
        else if (ImGui::IsKeyPressed(ImGuiKey_Z,false)) Undo(io.KeyShift);
        else if (ImGui::IsKeyPressed(ImGuiKey_Y,false)) Undo(true);
    }
    if (editKeys && ImGui::IsKeyPressed(ImGuiKey_Delete,false)) Remove();
    ImGui::EndChild();
}
void View::Demo(bool record) {
    Release();ResetPlayback();
    project=composition::Project{};project.beats=16;int second=project.AddTrack();project.tracks[second].instrument=5;project.tracks[second].sustained=true;
    project.tracks[0].name="主旋律 / 风物之诗琴";project.tracks[1].name="和声 / 晚风圆号";
    for (int i=0;i<12;++i) project.AddNote(0,14-i%7,i*composition::PPQ,composition::PPQ/2);
    for (int i=0;i<4;++i) {project.AddNote(1,20,i*4*composition::PPQ,3*composition::PPQ);project.AddNote(1,16,i*4*composition::PPQ,3*composition::PPQ);}
    project.beatTimes[8]=750;Select(0);ResetPlayback();note_=project.tracks[0].notes[0].id;selection_.insert(note_);inspectorRequest_=1;zoom_=1;pan_=0;
    if (record) {positionBeat_=3.25;StartRecording();}
}
}
