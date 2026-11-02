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
void View::Select(int track) {
    if (track<0 || track>=int(project.tracks.size())) return;selected_=track;note_=0;inspectorRequest_=0;
    snprintf(name_,sizeof(name_),"%s",project.tracks[track].name.c_str());instrumentui::SetInstrumentText(instrument_,sizeof(instrument_),project.tracks[track].instrument);
}
composition::Note* View::Note() {if (selected_<0 || selected_>=int(project.tracks.size())) return nullptr;for (auto& n:project.tracks[selected_].notes) if (n.id==note_) return &n;return nullptr;}
void View::Load(const std::wstring& path) {
    draftPath_=path;
    if (GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) try {
        std::ifstream in(path,std::ios::binary|std::ios::ate);auto size=in.tellg();if (!in || size<0 || size>8*1024*1024) throw std::runtime_error("无法读取创作草稿。");
        in.seekg(0);std::vector<uint8_t> bytes(static_cast<size_t>(size));if (!in.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("创作草稿读取不完整。");project=composition::Project::ReadDraft(bytes);
    }catch (const std::exception& e) {error_=e.what();}
    Select(0);
}
void View::Remember() {if (undo_.size()>=40) undo_.erase(undo_.begin());undo_.push_back(project);redo_.clear();}
void View::Save() {if (draftPath_.empty()) return;try {Write(draftPath_,project.Draft());}catch (const std::exception& e) {error_=e.what();}}
void View::Undo(bool redo) {
    auto& from=redo ? redo_:undo_;auto& to=redo ? undo_:redo_;if (from.empty()) return;
    to.push_back(project);project=std::move(from.back());from.pop_back();Select(std::min(selected_,int(project.tracks.size())-1));dragging_=false;error_.clear();Save();
}
void View::Remove() {
    if (!Note()) return;Remember();auto& notes=project.tracks[selected_].notes;notes.erase(std::remove_if(notes.begin(),notes.end(),[&](const composition::Note& n) {return n.id==note_;}),notes.end());note_=0;error_.clear();Save();
}
void View::StopAudition() {
    if (audition_>=0) {app::Command command;command.action=app::Action::ManualRelease;app::Send(command);audition_=-1;}
}
void View::Release() {
    StopAudition();
    if (dragging_) {dragging_=false;Save();}
}
void View::Audition(const app::Status& status,int row) {
    const auto& track=project.tracks[selected_];
    bool ready=status.preview || (status.game.ready && status.game.threadId && (!player::UsesShort(track.instrument,track.sustained) || status.game.shortReady));
    if (!ready) {error_="音符已保留；试听前请先在游戏短音乐器界面弹一个音完成校准。";return;}
    StopAudition();app::Command command;command.action=app::Action::ManualOn;command.slot=0;command.instrument=track.instrument;
    command.sustained=track.sustained;command.key=track.middleDo+notepalette::RowPitch(row)-60;
    if (!app::Send(command)) {error_="试听命令队列已满。";return;}
    audition_=row;auditionEnd_=game::NowMs()+250;
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
        Release();Remember();project=std::move(imported);Select(0);beat_=0;pan_=0;zoom_=1;error_.clear();Save();
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
        if (!app::Send(load)) throw std::runtime_error("命令队列已满。");app::Command play;play.action=app::Action::Play;play.value=status.preview ? 0:3000;
        if (!app::Send(play)) throw std::runtime_error("命令队列已满。");error_.clear();Release();
    }catch (const std::exception& e) {error_=e.what();}
}
void View::TogglePlayback(const app::Status& status) {
    app::Command command;
    if (status.transport.state==player::State::Playing || status.transport.state==player::State::Countdown) {command.action=app::Action::Pause;app::Send(command);}
    else if (status.transport.state==player::State::Paused) {command.action=app::Action::Play;app::Send(command);}
    else Play(status);
}
void View::Sidebar(bool locked) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(S(8),S(5)));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(S(6),S(3)));
    ui::Heading("创作音轨");ImGui::BeginDisabled(locked);
    if (ImGui::Button("+ 添加音轨",ImVec2(-1,0))) {try {Remember();Select(project.AddTrack());error_.clear();Save();}catch (const std::exception& e) {error_=e.what();}}ui::Mark("composer_add_track");
    if (ImGui::SmallButton("显示全部")) {Remember();for (auto& t:project.tracks) t.visible=true;Save();}ui::Mark("composer_show_all");ImGui::SameLine();
    if (ImGui::SmallButton("只显示此轨")) {Remember();for (size_t i=0;i<project.tracks.size();++i) project.tracks[i].visible=int(i)==selected_;Save();}ui::Mark("composer_solo");
    ImGui::EndDisabled();
    ImGui::BeginChild("composer_track_list",ImVec2(0,std::max(S(100),std::min(S(130),ImGui::GetContentRegionAvail().y*.24f))),ImGuiChildFlags_Borders);
    for (size_t i=0;i<project.tracks.size();++i) {
        auto& t=project.tracks[i];ImGui::PushID(int(t.id));bool visible=t.visible;ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("##visible",&visible)) {Remember();t.visible=visible;Save();}ImGui::EndDisabled();ImGui::SameLine();
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
    ImGui::Separator();auto note=Note();if (note) {
        ImGui::TextColored(ImVec4(.7f,.84f,1,1),"选中：%s %d · %c",notepalette::RowLabel(note->row),7-note->row%7,notepalette::RowKey(note->row));
        float start=note->start/float(composition::PPQ),length=note->length/float(composition::PPQ);int velocity=note->velocity;
        ImGui::SetNextItemWidth(S(135));bool change=ImGui::InputFloat("开始 / 拍",&start,.25f,1,"%.2f");
        ImGui::SetNextItemWidth(S(135));change=ImGui::InputFloat("时长 / 拍",&length,.25f,1,"%.2f") || change;
        ImGui::SetNextItemWidth(S(85));change=ImGui::SliderInt("力度",&velocity,1,127) || change;
        if (change && std::isfinite(start) && std::isfinite(length) && start>=0 && length>0 && start+length<=project.beats) {
            auto n=*note;n.start=int(std::llround(start*composition::PPQ));n.length=int(std::llround(length*composition::PPQ));n.velocity=velocity;
            if (project.Place(selected_,n)) {Remember();*note=n;error_.clear();Save();}else error_="同一轨的同音高音符不能重叠。";
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
void View::Grid(const app::Status& status,bool focused,bool locked) {
    const auto& io=ImGui::GetIO();float span=project.beats/zoom_;span=std::max(1.f,span);pan_=std::max(0.f,std::min(float(project.beats)-span,pan_));
    ImGui::BeginChild("composer_roll",ImVec2(0,0),ImGuiChildFlags_Borders,ImGuiWindowFlags_AlwaysVerticalScrollbar|((io.KeyCtrl || io.KeyShift) ? ImGuiWindowFlags_NoScrollWithMouse:0));
    auto origin=ImGui::GetCursorScreenPos();float width=ImGui::GetContentRegionAvail().x,keyWidth=S(100),rulerHeight=S(30);
    float rowHeight=std::max(S(14),std::min(S(24),(ImGui::GetContentRegionAvail().y-rulerHeight)/21));
    float left=origin.x+keyWidth,right=origin.x+width,top=origin.y+rulerHeight,bottom=top+21*rowHeight;
    float beatWidth=std::max(1.f,(right-left)/span);auto draw=ImGui::GetWindowDrawList();
    ImGui::InvisibleButton("composer_canvas",ImVec2(width,rulerHeight+21*rowHeight),ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight);ui::Mark("composer_grid");
    bool hovered=ImGui::IsItemHovered();draw->PushClipRect(origin,ImVec2(right,bottom),true);
    draw->AddRectFilled(origin,ImVec2(right,bottom),IM_COL32(20,26,33,255));
    for (int row=0;row<21;++row) {
        float y=top+row*rowHeight;draw->AddRectFilled(ImVec2(origin.x,y),ImVec2(right,y+rowHeight),(row/7)%2 ? IM_COL32(26,34,43,255):IM_COL32(23,29,38,255));
        draw->AddRectFilled(ImVec2(origin.x,y),ImVec2(left,y+rowHeight),row==audition_ ? IM_COL32(53,104,113,255):IM_COL32(34,44,54,255));
        if (row==13) ui::MarkRect("composer_piano",ImVec2(origin.x,y),ImVec2(left,y+rowHeight));
        char label[64];sprintf_s(label,"%c  %s %d",notepalette::RowKey(row),notepalette::RowLabel(row),7-row%7);
        draw->AddText(ImGui::GetFont(),std::min(ImGui::GetFontSize(),rowHeight-S(2)),ImVec2(origin.x+S(6),y+S(1)),IM_COL32(180,199,213,255),label);draw->AddLine(ImVec2(origin.x,y+rowHeight),ImVec2(right,y+rowHeight),IM_COL32(44,53,63,255));
    }
    int snapTicks=composition::PPQ/(1<<snap_);float gridStep=1.f/(1<<snap_);
    for (float b=std::ceil(pan_/gridStep)*gridStep;b<=pan_+span+.001f;b+=gridStep) {
        float x=left+(b-pan_)*beatWidth;bool whole=std::abs(b-std::round(b))<.001f,bar=whole && int(std::round(b))%4==0;
        draw->AddLine(ImVec2(x,whole ? origin.y:top),ImVec2(x,bottom),bar ? IM_COL32(95,113,130,200):whole ? IM_COL32(66,82,98,200):IM_COL32(43,53,64,100));
        if (whole && beatWidth>=S(26)) {char label[32];sprintf_s(label,"%d.%d",int(b)/4+1,int(b)%4+1);draw->AddText(ImVec2(x+S(3),origin.y+S(5)),IM_COL32(172,188,206,255),label);}
    }
    if (beat_>=pan_ && beat_<pan_+span) {float x=left+(beat_-pan_)*beatWidth;draw->AddRectFilled(ImVec2(x,origin.y),ImVec2(x+beatWidth,top),IM_COL32(84,149,169,45));}
    int hitTrack=-1;unsigned hit=0;bool edge=false;
    double playbackTick=project.BeatAt(status.transport.positionMs)*composition::PPQ;
    for (int pass=0;pass<2;++pass) for (size_t i=0;i<project.tracks.size();++i) {
        const auto& track=project.tracks[i];if (!track.visible || ((int(i)==selected_)!=(pass==1))) continue;
        for (const auto& n:track.notes) {
            float x1=left+(n.start/float(composition::PPQ)-pan_)*beatWidth,x2=x1+n.length/float(composition::PPQ)*beatWidth;
            if (x2<left || x1>right) continue;float y=top+n.row*rowHeight;
            bool current=int(i)==selected_,chosen=current && n.id==note_;ImVec2 a(std::max(left,x1),y+S(2)),b(std::min(right,x2),y+rowHeight-S(3));
            bool active=Active(status.transport.state) && n.start<=playbackTick && n.start+n.length>playbackTick;
            draw->AddRectFilled(a,b,Color(track,active ? 255:current ? 230:150),S(2));
            if (current) {draw->AddRect(a,b,chosen ? IM_COL32(255,236,179,255):Color(track),S(2),0,S(chosen ? 2.f:1.f));if (x2-x1>S(36)) draw->AddText(ImVec2(a.x+S(4),a.y),IM_COL32(17,27,32,255),notepalette::RowLabel(n.row));}
            if (chosen) ui::MarkRect("composer_note",a,b);
            if (hovered && io.MousePos.x>=a.x && io.MousePos.x<=b.x && io.MousePos.y>=a.y && io.MousePos.y<=b.y) {hitTrack=int(i);hit=n.id;edge=io.MousePos.x>=x2-S(7);}
        }
    }
    if (Active(status.transport.state)) {float x=left+(float(project.BeatAt(status.transport.positionMs))-pan_)*beatWidth;if (x>=left && x<=right) draw->AddLine(ImVec2(x,origin.y),ImVec2(x,bottom),IM_COL32(255,213,100,255),S(2));}
    draw->PopClipRect();
    bool input=focused && !locked && !io.AppFocusLost && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);int row=int((io.MousePos.y-top)/rowHeight);
    if (hit && hovered) {
        const composition::Note* n=nullptr;for (const auto& item:project.tracks[hitTrack].notes) if (item.id==hit) n=&item;
        if (n) ImGui::SetTooltip("%s · %c / %s %d\n开始 %.2f 拍 · 时长 %.2f 拍 · 力度 %d\n拖动移动 · 拖右边缘改时长 · 右键删除",project.tracks[hitTrack].name.c_str(),notepalette::RowKey(n->row),notepalette::RowLabel(n->row),7-n->row%7,n->start/double(composition::PPQ),n->length/double(composition::PPQ),n->velocity);
        ImGui::SetMouseCursor(edge ? ImGuiMouseCursor_ResizeEW:ImGuiMouseCursor_Hand);
    }
    if (input && hovered && ImGui::IsMouseClicked(1) && hit) {Select(hitTrack);note_=hit;Remove();}
    if (input && hovered && ImGui::IsMouseClicked(0)) {
        if (io.MousePos.y<top && io.MousePos.x>=left) beat_=std::max(0,std::min(project.beats-1,int(pan_+(io.MousePos.x-left)/beatWidth)));
        else if (row>=0 && row<21 && io.MousePos.y>=top) {
            if (io.MousePos.x<left) {
                Audition(status,row);
            }else if (hit) {Select(hitTrack);note_=hit;inspectorRequest_=1;Remember();original_=*Note();dragging_=true;resizing_=edge;dragX_=io.MousePos.x;dragY_=io.MousePos.y;}
            else if (project.tracks[selected_].visible) {
                int start=int(std::floor((pan_+(io.MousePos.x-left)/beatWidth)*composition::PPQ/snapTicks))*snapTicks;
                int length=std::min(length_*composition::PPQ/4,project.beats*composition::PPQ-start);
                if (start>=0 && length>0) try {Remember();note_=project.AddNote(selected_,row,start,length,velocity_);inspectorRequest_=1;original_=*Note();dragging_=true;resizing_=true;dragX_=io.MousePos.x;dragY_=io.MousePos.y;error_.clear();Save();Audition(status,row);}catch (const std::exception& e) {error_=e.what();}
            }else error_="请先打开此轨的显示开关，再添加音符。";
        }
    }
    if (dragging_) {
        auto n=Note();if (n && input && ImGui::IsMouseDragging(0,S(3))) {
            auto moved=original_;int delta=int(std::llround((io.MousePos.x-dragX_)/beatWidth*composition::PPQ/snapTicks))*snapTicks;
            if (resizing_) moved.length=std::max(snapTicks,std::min(project.beats*composition::PPQ-moved.start,original_.length+delta));
            else {moved.start=std::max(0,std::min(project.beats*composition::PPQ-moved.length,original_.start+delta));moved.row=std::max(0,std::min(20,original_.row+int(std::llround((io.MousePos.y-dragY_)/rowHeight))));}
            if (project.Place(selected_,moved)) {*n=moved;error_.clear();}
        }
        if (!input || !ImGui::IsMouseDown(0)) {dragging_=false;Save();}
    }
    if (audition_>=0 && (!input || game::NowMs()>=auditionEnd_)) StopAudition();
    if (input && !io.WantTextInput && !dragging_ && hovered && ImGui::IsKeyPressed(ImGuiKey_Delete,false)) Remove();
    if (focused && !io.AppFocusLost && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) && hovered && io.MouseWheel!=0 && !dragging_) {
        if (io.KeyCtrl) {float anchor=std::max(0.f,std::min(1.f,(io.MousePos.x-left)/(right-left))),at=pan_+anchor*span;zoom_=std::max(1.f,std::min(32.f,zoom_*float(std::pow(1.25,io.MouseWheel))));pan_=at-anchor*std::max(1.f,project.beats/zoom_);}
        else if (io.KeyShift) pan_-=io.MouseWheel*span*.1f;
    }
    ImGui::EndChild();
}
void View::Draw(const app::Status& status,HWND owner,float height,bool focused) {
    bool locked=Active(status.transport.state);ImGui::BeginChild("composer_workspace",ImVec2(0,height),ImGuiChildFlags_Borders);
    ImGui::BeginDisabled(locked);if (ImGui::Button("新建")) ImGui::OpenPopup("new_composition");
    if (ImGui::BeginPopup("new_composition")) {ImGui::TextWrapped("清空当前创作并新建？可通过撤销恢复。");if (ImGui::Button("确认新建")) {Remember();project=composition::Project{};Select(0);pan_=0;beat_=0;Save();ImGui::CloseCurrentPopup();}ImGui::EndPopup();}
    ImGui::SameLine();ImGui::BeginDisabled(undo_.empty());if (ImGui::Button("撤销")) Undo(false);ui::Mark("composer_undo");ImGui::EndDisabled();ImGui::SameLine();ImGui::BeginDisabled(redo_.empty());if (ImGui::Button("重做")) Undo(true);ImGui::EndDisabled();
    ImGui::SameLine();ImGui::SetNextItemWidth(S(85));int beats=project.beats;if (ImGui::InputInt("总拍数",&beats,0) && beats>=1 && beats<=composition::MaxBeats) {
        int end=0;for (const auto& t:project.tracks) for (const auto& n:t.notes) end=std::max(end,n.start+n.length);
        if (beats*composition::PPQ>=end) {Remember();project.beats=beats;for (auto it=project.beatTimes.begin();it!=project.beatTimes.end();) if (it->first>=beats) it=project.beatTimes.erase(it);else ++it;for (auto it=project.tempoChanges.begin();it!=project.tempoChanges.end();) if (it->first>=beats*composition::PPQ) it=project.tempoChanges.erase(it);else ++it;beat_=std::min(beat_,beats-1);Save();}else error_="总拍数不能小于现有音符的结束位置。";
    }
    ImGui::SameLine();ImGui::SetNextItemWidth(S(100));float ms=float(project.beatMs);if (ImGui::InputFloat("每拍 / 毫秒",&ms,0,0,"%.1f") && std::isfinite(ms) && ms>=10 && ms<=16000) {Remember();project.beatMs=ms;Save();}
    ImGui::SameLine();ImGui::TextDisabled("%.1f BPM",60000/project.beatMs);ImGui::SameLine();if (ImGui::Button("导入 MIDI…")) Import(owner);ui::Mark("composer_import");ImGui::EndDisabled();ImGui::SameLine();
    if (ImGui::Button("导出 MIDI…")) Export(owner);ui::Mark("composer_export");
    ImGui::BeginDisabled(locked);const char* snaps[]={"1 拍","1/2 拍","1/4 拍","1/8 拍","1/16 拍"};ImGui::SetNextItemWidth(S(110));ImGui::Combo("网格",&snap_,snaps,5);
    ImGui::SameLine();ImGui::SetNextItemWidth(S(80));ImGui::SliderInt("新音时长 / 1/4 拍",&length_,1,32);ImGui::SameLine();ImGui::SetNextItemWidth(S(65));ImGui::SliderInt("新音力度",&velocity_,1,127);ImGui::EndDisabled();
    ImGui::SameLine();if (ImGui::Button("全曲")) {zoom_=1;pan_=0;}ImGui::SameLine();ImGui::SetNextItemWidth(S(110));ImGui::SliderFloat("缩放",&zoom_,1,32,"%.1fx",ImGuiSliderFlags_Logarithmic);
    float span=std::max(1.f,project.beats/zoom_),maxPan=std::max(0.f,project.beats-span);pan_=std::max(0.f,std::min(maxPan,pan_));ImGui::SameLine();ImGui::SetNextItemWidth(S(110));ImGui::SliderFloat("起点 / 拍",&pan_,0,maxPan,"%.1f");
    ImGui::BeginDisabled(locked);ImGui::SetNextItemWidth(S(60));int beat=beat_+1;if (ImGui::InputInt("单独设置第几拍",&beat,0)) beat_=std::max(0,std::min(project.beats-1,beat-1));
    ImGui::SameLine();ImGui::SetNextItemWidth(S(90));float duration=float(project.BeatDuration(beat_));if (ImGui::InputFloat("此拍 / 毫秒",&duration,0,0,"%.1f") && std::isfinite(duration) && duration>=10 && duration<=16000) {Remember();project.beatTimes[beat_]=duration;Save();}
    ImGui::SameLine();if (ImGui::Button("此拍恢复默认")) {Remember();project.beatTimes.erase(beat_);Save();}ImGui::EndDisabled();
    ImGui::SameLine();if (ImGui::Button(status.transport.state==player::State::Playing || status.transport.state==player::State::Countdown ? "暂停":"播放创作")) TogglePlayback(status);ui::Mark("composer_play");
    ImGui::SameLine();if (ImGui::Button("停止")) {app::Command c;c.action=app::Action::Stop;app::Send(c);}ui::Mark("composer_stop");
    if (!project.tempoChanges.empty() || !project.beatTimes.empty()) {ImGui::SameLine();ImGui::BeginDisabled(locked);if (ImGui::Button("统一拍长")) {Remember();project.tempoChanges.clear();project.beatTimes.clear();Save();}ImGui::EndDisabled();}
    ImGui::TextDisabled("%u 轨 · %u 音符 · %s · 点击添加 / 拖动移动 / 拖右边缘改时长 / 右键删除",unsigned(project.tracks.size()),unsigned(project.NoteCount()),ui::Time(project.Time(project.beats)).c_str());
    if (!error_.empty()) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"%s",error_.c_str());else if (!notice_.empty()) ImGui::TextWrapped("%s",notice_.c_str());
    if (!status.manualError.empty()) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"试听：%s",ui::Utf8(ui::Widen(status.manualError)).c_str());
    if (status.transport.state==player::State::Countdown) ImGui::TextColored(ImVec4(1,.8f,.45f,1),"倒计时 %.1f 秒 · 请切回游戏演奏界面",status.transport.countdownMs/1000.);
    else if (status.transport.state==player::State::Error) ImGui::TextColored(ImVec4(1,.5f,.45f,1),"演奏失败：%s",ui::Utf8(ui::Widen(status.transport.error)).c_str());
    if (ImGui::BeginTable("composer_layout",2,ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("tracks",ImGuiTableColumnFlags_WidthFixed,S(245));ImGui::TableSetupColumn("roll",ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextColumn();float body=std::max(S(220),ImGui::GetContentRegionAvail().y);ImGui::BeginChild("composer_sidebar",ImVec2(0,body));Sidebar(locked);ImGui::EndChild();
        ImGui::TableNextColumn();ImGui::BeginChild("composer_grid_holder",ImVec2(0,body));Grid(status,focused,locked);ImGui::EndChild();ImGui::EndTable();
    }
    if (focused && !locked && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId) && ImGui::GetIO().KeyCtrl && !dragging_) {
        if (ImGui::IsKeyPressed(ImGuiKey_Z,false)) Undo(ImGui::GetIO().KeyShift);else if (ImGui::IsKeyPressed(ImGuiKey_Y,false)) Undo(true);
    }
    ImGui::EndChild();
}
void View::Demo() {
    project=composition::Project{};project.beats=16;int second=project.AddTrack();project.tracks[second].instrument=5;project.tracks[second].sustained=true;
    project.tracks[0].name="主旋律 / 风物之诗琴";project.tracks[1].name="和声 / 晚风圆号";
    for (int i=0;i<12;++i) project.AddNote(0,14-i%7,i*composition::PPQ,composition::PPQ/2);
    for (int i=0;i<4;++i) {project.AddNote(1,20,i*4*composition::PPQ,3*composition::PPQ);project.AddNote(1,16,i*4*composition::PPQ,3*composition::PPQ);}
    project.beatTimes[8]=750;Select(0);note_=project.tracks[0].notes[0].id;inspectorRequest_=1;zoom_=1;pan_=0;
}
}
