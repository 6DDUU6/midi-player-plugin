#include "Panel.h"
#include "App.h"
#include "InstrumentConfig.h"
#include "ImGuiHost.h"
#include "Timeline.h"
#include "Performance.h"
#include "Composer.h"
#include "../vendor/ImGui/imgui_impl_win32.h"
#include <commdlg.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <functional>
#include <iomanip>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include <stdexcept>
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND,UINT,WPARAM,LPARAM);
namespace panel {
namespace {
const wchar_t* windowClass=L"MidiPlayerPluginPanel";
const UINT Loaded=WM_APP+10,ShowPanel=WM_APP+11;
HWND window=nullptr;
std::atomic<HWND> published{nullptr};
bool previewMode=false,smokeMode=false,snapshotMode=false,loading=false,dirty=true,editDirty=false;
bool running=true,manualLogging=false,onlyNotes=true,seekActive=false,revealSelection=false,tableView=false,mappingColumnVisible=false;
bool manualView=false;
bool compositionView=false;
int selected=-1,editorTrack=-1,speed=100,delay=3,resultCode=0,automationPhase=0,automationFrames=0;
float seekValue=0;
std::wstring directory,ini,path;
std::string error,notice,lastLog;
uint64_t songHash=0;
std::shared_ptr<midi::Song> song;
std::vector<player::TrackConfig> configs;
player::TrackConfig draft;
char instrumentText[96]{};
instrumentui::Editor editor;
instrumentui::NotesViewer viewer;
ImGuiTextFilter trackFilter;
timeline::View timelineView;
performance::View performanceView;
composer::View composerView;
std::vector<std::string> trackNames;
struct Row {std::string name,channels,programs,range;};
std::vector<Row> rows;
struct LoadResult {std::shared_ptr<midi::Song> song;std::wstring path;uint64_t hash=0;std::string error;};
std::mutex loadMutex;
std::unique_ptr<LoadResult> pendingLoad;
std::thread loader;
float S(float value) {return ui::Scale(value);}
std::string TrackTitle(int i) {return "音轨 "+std::to_string(i+1)+" · "+rows[i].name;}
bool Active(player::State state) {return state==player::State::Playing || state==player::State::Countdown || state==player::State::Paused;}
std::wstring Section(size_t track) {
    std::wostringstream out;out<<L"song_"<<std::hex<<songHash<<L"_track_"<<std::dec<<track;return out.str();
}
void Write(const std::wstring& section,const wchar_t* key,int value) {
    auto text=std::to_wstring(value);
    if (!WritePrivateProfileStringW(section.c_str(),key,text.c_str(),ini.c_str())) throw std::runtime_error("无法写入 MidiPlayer.ini，请检查目录写入权限。");
}
std::map<int,int> ReadNoteMap(const std::wstring& section) {
    wchar_t buffer[2048]{};
    if (GetPrivateProfileStringW(section.c_str(),L"noteMap",L"",buffer,2048,ini.c_str())>=2047) throw std::runtime_error("自定义映射过长。");
    std::wstring text=buffer;std::map<int,int> mappings;
    if (!text.empty() && text.back()==L',') throw std::runtime_error("自定义音高映射格式无效。");
    std::wistringstream in(text);std::wstring field;
    while (std::getline(in,field,L',')) {
        std::wistringstream entry(field);int source,target;wchar_t colon;
        if (!(entry>>source>>colon>>target) || colon!=L':' || source<0 || source>127 || target<1 || target>1000000) throw std::runtime_error("自定义音高映射格式无效。");
        entry>>std::ws;if (!entry.eof() || !mappings.emplace(source,target).second) throw std::runtime_error("自定义音高映射重复或格式无效。");
    }return mappings;
}
bool Save() {
    try {
        if (song) for (size_t i=0;i<configs.size();++i) {
            auto section=Section(i);const auto& c=configs[i];
            Write(section,L"enabled",c.enabled);Write(section,L"instrument",c.instrument);Write(section,L"sustained",c.sustained);
            Write(section,L"middleDo",c.middleDo);Write(section,L"transpose",c.transpose);std::wstring mappings;
            for (const auto& m:c.noteKeys) {if (!mappings.empty()) mappings+=L",";mappings+=std::to_wstring(m.first)+L":"+std::to_wstring(m.second);}
            if (!WritePrivateProfileStringW(section.c_str(),L"noteMap",mappings.empty() ? nullptr:mappings.c_str(),ini.c_str())) throw std::runtime_error("无法保存音高映射。");
        }
        if (speed>=25 && speed<=400) Write(L"player",L"speed",speed);
        if (delay>=0 && delay<=30) Write(L"player",L"delay",delay);
        return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
bool Commit() {
    if (selected<0 || !editDirty) return true;
    if (!instrumentui::Validate(draft,instrumentText,error)) return false;
    configs[selected]=draft;dirty=true;instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),draft.instrument);
    if (!Save()) return false;editDirty=false;return true;
}
bool Select(int i) {
    if (i<0 || i>=int(configs.size()) || !Commit()) return false;
    selected=i;draft=configs[i];editDirty=false;instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),draft.instrument);return true;
}
bool Submit(app::Command command) {
    if (app::Send(command)) return true;error="命令队列已满，请等待游戏主线程处理。";return false;
}
void Stop() {app::Command c;c.action=app::Action::Stop;if (Submit(c)) composerView.ResetPlayback();}
void Pause() {app::Command c;c.action=app::Action::Pause;Submit(c);}
void LoadFile(const std::wstring& target) {
    if (loading || editor.open || !Commit()) return;
    if (loader.joinable()) loader.join();loading=true;error.clear();notice="正在读取 MIDI…";Stop();HWND destination=window;
    loader=std::thread([target,destination]() {
        std::unique_ptr<LoadResult> result(new LoadResult);result->path=target;
        try {
            std::ifstream file(target,std::ios::binary|std::ios::ate);
            if (!file) throw std::runtime_error("无法打开 MIDI 文件。");
            auto length=file.tellg();if (length<0 || length>64*1024*1024) throw std::runtime_error("MIDI 文件超过 64 MiB。");
            file.seekg(0);std::vector<uint8_t> bytes(static_cast<size_t>(length));
            if (!bytes.empty() && !file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) throw std::runtime_error("无法完整读取 MIDI 文件。");
            result->hash=14695981039346656037ull;for (auto b:bytes) {result->hash^=b;result->hash*=1099511628211ull;}
            result->song=std::make_shared<midi::Song>(midi::Parse(bytes));
        } catch (const std::exception& e) {result->error=e.what();}
        {std::lock_guard<std::mutex> lock(loadMutex);pendingLoad=std::move(result);}PostMessageW(destination,Loaded,0,0);
    });
}
void OpenFile() {
    if (!Commit()) return;wchar_t filename[32768]{};OPENFILENAMEW dialog{};dialog.lStructSize=sizeof(dialog);dialog.hwndOwner=window;
    dialog.lpstrFilter=L"MIDI 文件 (*.mid;*.midi)\0*.mid;*.midi\0所有文件\0*.*\0\0";
    dialog.lpstrFile=filename;dialog.nMaxFile=32768;dialog.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&dialog)) LoadFile(filename);
}
void LoadedFile() {
    std::unique_ptr<LoadResult> result;{std::lock_guard<std::mutex> lock(loadMutex);result=std::move(pendingLoad);}if (!result) return;loading=false;
    if (!result->error.empty()) {error=result->error;notice.clear();if (smokeMode || snapshotMode) {resultCode=2;running=false;}return;}
    song=result->song;path=result->path;songHash=result->hash;dirty=true;selected=-1;editDirty=false;
    viewer.open=false;configs.assign(song->tracks.size(),player::TrackConfig{});rows.clear();trackNames.clear();trackFilter.Clear();timelineView.Open(*song);
    for (size_t i=0;i<configs.size();++i) {
        auto& c=configs[i];auto section=Section(i);
        c.enabled=GetPrivateProfileIntW(section.c_str(),L"enabled",!song->tracks[i].notes.empty(),ini.c_str())!=0;
        const auto& imported=song->tracks[i];
        c.instrument=GetPrivateProfileIntW(section.c_str(),L"instrument",imported.gameInstrument ? imported.gameInstrument:1,ini.c_str());
        c.sustained=GetPrivateProfileIntW(section.c_str(),L"sustained",imported.gameInstrument ? imported.gameSustained:instrumentui::DefaultSustained(c.instrument),ini.c_str())!=0;
        c.sustained=player::UsesSustained(c.instrument,c.sustained);
        c.middleDo=GetPrivateProfileIntW(section.c_str(),L"middleDo",imported.gameInstrument ? imported.gameMiddleDo:player::DefaultMiddleDo,ini.c_str());c.transpose=int(GetPrivateProfileIntW(section.c_str(),L"transpose",0,ini.c_str()));
        try {c.noteKeys=ReadNoteMap(section);} catch (const std::exception& e) {error="音轨 "+std::to_string(i+1)+"："+e.what();}
        const auto& tr=song->tracks[i];Row row;row.name=tr.name.empty() ? (tr.notes.empty() ? "速度 / 控制 / 空音轨":"未命名音轨"):ui::Utf8(ui::Widen(tr.name));
        for (int ch=0;ch<16;++ch) if (tr.channels&(1<<ch)) {if (!row.channels.empty()) row.channels+=", ";row.channels+=std::to_string(ch+1);}
        for (int p:tr.programs) {if (!row.programs.empty()) row.programs+=", ";row.programs+=std::to_string(p+1);}
        if (!tr.notes.empty()) {int low=127,high=0;for (const auto& note:tr.notes) {low=std::min(low,note.pitch);high=std::max(high,note.pitch);}row.range=instrumentui::Pitch(low)+" – "+instrumentui::Pitch(high);}else row.range="—";
        trackNames.push_back(row.name);rows.push_back(std::move(row));
    }
    for (size_t i=0;i<song->tracks.size();++i) if (!song->tracks[i].notes.empty()) {Select(int(i));break;}
    if (selected<0 && !configs.empty()) Select(0);
    revealSelection=true;timelineView.Reveal(selected);
    notice=song->warnings.empty() ? "MIDI 已加载。":ui::Utf8(ui::Widen(song->warnings.front()));automationFrames=0;
}
bool PreparePlan() {
    if (!song || loading || editor.open || !Commit()) return false;
    if (speed<25 || speed>400) {error="速度应为 25%–400%。";return false;}
    if (!dirty && app::GetStatus().transport.state!=player::State::Empty) return true;
    try {
        app::Command load;load.action=app::Action::Load;load.plan=player::Build(*song,configs,speed/100.0);
        if (!Submit(load)) return false;dirty=false;return true;
    } catch (const std::exception& e) {error=e.what();return false;}
}
void Seek(double ms) {
    if (!PreparePlan()) return;app::Command command;command.action=app::Action::Seek;
    command.value=std::max(0.,std::min(song->durationMs,ms));Submit(command);
}
void StartPlayback() {
    if (!song || loading || editor.open || !Commit()) return;auto status=app::GetStatus();
    if (!previewMode && (!status.game.ready || !status.game.threadId)) {error="先进入游戏乐器界面并手动弹一个音，捕获游戏线程。";return;}
    bool shorts=false;for (size_t i=0;i<configs.size();++i) if (configs[i].enabled && player::UsesShort(configs[i].instrument,configs[i].sustained) && !song->tracks[i].notes.empty()) shorts=true;
    if (!previewMode && shorts && !status.game.shortReady) {error="启用的音轨包含短音乐器，请先在当前演奏界面手动弹一个短音。";return;}
    if (speed<25 || speed>400 || delay<0 || delay>30) {error="速度应为 25%–400%，倒计时应为 0–30 秒。";return;}
    try {
        if (!PreparePlan()) return;
        app::Command play;play.action=app::Action::Play;play.value=delay*1000;if (Submit(play)) {error.clear();Save();}
    } catch (const std::exception& e) {error=e.what();}
}
void TestNote() {
    if (editor.open || !Commit() || selected<0) return;auto status=app::GetStatus();const auto& c=configs[selected];
    if (!previewMode && (!status.game.ready || !status.game.threadId || (player::UsesShort(c.instrument,c.sustained) && !status.game.shortReady))) {error="先在游戏乐器界面手动弹一个音，完成对应的校准。";return;}
    if (delay<0 || delay>30) {error="倒计时应为 0–30 秒。";return;}
    app::Command test;test.action=app::Action::Test;test.instrument=c.instrument;test.key=c.middleDo;test.sustained=c.sustained;test.value=delay*1000.;if (Submit(test)) error.clear();dirty=true;
}
void OpenEditor() {
    if (!song || selected<0 || loading || editor.open || Active(app::GetStatus().transport.state) || !Commit()) return;
    editorTrack=selected;editor.Open(song->tracks[selected],TrackTitle(selected),configs[selected]);
}
void ViewNotes(int i) {
    if (!song || loading || editor.open || i<0 || i>=int(configs.size()) || !Select(i)) return;viewer.Open(song,size_t(i),TrackTitle(i),configs[i]);
}
void AllTracks(bool enabled) {
    if (!Commit() || !song) return;for (size_t i=0;i<configs.size();++i) configs[i].enabled=enabled && !song->tracks[i].notes.empty();
    if (selected>=0) {draft=configs[selected];instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),draft.instrument);}dirty=true;Save();
}
std::string StateText(const app::Status& status) {
    const auto& t=status.transport;
    switch (t.state) {
    case player::State::Countdown:return "倒计时 "+std::to_string(int(std::ceil(t.countdownMs/1000)))+" 秒 · 请切回游戏演奏界面";
    case player::State::Playing:return "正在演奏 · 已触发 "+std::to_string(t.playedNotes)+" 个音符 · 长音句柄 "+std::to_string(t.activeVoices);
    case player::State::Paused:return "已暂停 · 长音已停止；点击继续恢复演奏";
    case player::State::Finished:return "演奏完成 · 已触发 "+std::to_string(t.playedNotes)+" 个音符";
    case player::State::Error:return "演奏失败："+ui::Utf8(ui::Widen(t.error));
    default:return previewMode ? "界面预览模式 · 调度模拟运行，不产生游戏声音":ui::Utf8(ui::Widen(status.message));
    }
}
void DrawTracks(bool locked,float height) {
    ImGui::BeginChild("tracks_panel",ImVec2(0,height),ImGuiChildFlags_Borders);
    if (ImGui::RadioButton("时间轴",!tableView)) {tableView=false;timelineView.Reveal(selected);}ui::Mark("timeline_view");ImGui::SameLine();
    if (ImGui::RadioButton("音轨表",tableView)) {tableView=true;revealSelection=true;}ui::Mark("table_view");ImGui::SameLine();ImGui::TextDisabled("音轨工作区");ImGui::Separator();
    ImGui::SetNextItemWidth(S(180));if (ImGui::InputTextWithHint("##track_filter","搜索音轨…",trackFilter.InputBuf,sizeof(trackFilter.InputBuf))) trackFilter.Build();
    ImGui::SameLine();ImGui::Checkbox("仅含音符",&onlyNotes);ImGui::SameLine();ImGui::BeginDisabled(locked || !song);
    if (ImGui::SmallButton("全选音符轨")) AllTracks(true);ImGui::SameLine();if (ImGui::SmallButton("全部关闭")) AllTracks(false);ImGui::EndDisabled();
    if (!song) {ImGui::Spacing();ui::Muted("加载一个 MIDI 开始配置。所有音轨都会显示，包括速度、控制和空音轨。");ImGui::EndChild();return;}
    std::vector<int> visible;for (size_t i=0;i<rows.size();++i) if ((!onlyNotes || !song->tracks[i].notes.empty()) && trackFilter.PassFilter(rows[i].name.c_str())) visible.push_back(int(i));
    if (!tableView) {
        auto transport=app::GetStatus().transport;if (dirty) {transport.positionMs=0;transport.state=player::State::Stopped;}
        auto action=timelineView.Draw(*song,configs,trackNames,visible,selected,transport,locked,!loading && !editor.open);
        if (action.selected>=0) Select(action.selected);
        if (action.toggle>=0 && Commit()) {auto& c=configs[action.toggle];c.enabled=!c.enabled;if (selected==action.toggle) draft.enabled=c.enabled;dirty=true;Save();}
        if (action.notes>=0) ViewNotes(action.notes);
        if (action.seek) Seek(action.position);
        ImGui::EndChild();return;
    }
    auto flags=ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_Resizable|ImGuiTableFlags_Reorderable|ImGuiTableFlags_Hideable|ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX;
    if (ImGui::BeginTable("tracks",12,flags,ImVec2(0,std::max(S(70),ImGui::GetContentRegionAvail().y-S(30))))) {
        const char* headers[]={"启用","#","音轨名称","音符数","音域","游戏乐器","类型","映射","通道","Program","中音 Do","移调"};
        const float widths[]={36,30,170,55,100,135,94,64,65,78,85,65};
        for (int i=0;i<12;++i) ImGui::TableSetupColumn(headers[i],ImGuiTableColumnFlags_WidthFixed|(i>=8 ? ImGuiTableColumnFlags_DefaultHide:0),S(widths[i]));
        ImGui::TableSetupScrollFreeze(3,1);ImGui::TableHeadersRow();mappingColumnVisible=(ImGui::TableGetColumnFlags(7)&ImGuiTableColumnFlags_IsEnabled)!=0;
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(S(5),S(2)));ImGuiListClipper clipper;clipper.Begin(int(visible.size()));
        if (revealSelection) for (size_t r=0;r<visible.size();++r) if (visible[r]==selected) clipper.IncludeItemByIndex(int(r));
        while (clipper.Step()) for (int r=clipper.DisplayStart;r<clipper.DisplayEnd;++r) {
            int i=visible[r];auto& c=configs[i];ImGui::PushID(i);ImGui::TableNextRow();ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);bool enabled=c.enabled;if (ImGui::Checkbox("##enabled",&enabled)) {c.enabled=enabled;if (selected==i) draft.enabled=enabled;dirty=true;Save();}ImGui::EndDisabled();
            ImGui::TableNextColumn();ImGui::TextDisabled("%02d",i+1);ImGui::TableNextColumn();
            if (ImGui::Selectable(rows[i].name.c_str(),selected==i,ImGuiSelectableFlags_SpanAllColumns|ImGuiSelectableFlags_AllowOverlap|ImGuiSelectableFlags_AllowDoubleClick)) {Select(i);if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) ViewNotes(i);}
            if (revealSelection && selected==i) {ImGui::SetScrollHereY(.35f);revealSelection=false;}
            if (ImGui::IsItemHovered()) {ImGui::BeginTooltip();ImGui::TextUnformatted(rows[i].name.c_str());ImGui::TextDisabled("双击查看音符 · 右键表头显示更多列");ImGui::EndTooltip();}
            ImGui::TableNextColumn();ImGui::Text("%u",unsigned(song->tracks[i].notes.size()));ImGui::TableNextColumn();ImGui::TextUnformatted(rows[i].range.c_str());
            ImGui::TableNextColumn();ImGui::TextUnformatted(ui::Utf8(instrumentui::InstrumentLabel(c.instrument)).c_str());ImGui::TableNextColumn();
            if (player::UsesSustained(c.instrument,c.sustained)) ImGui::TextColored(ImVec4(.48f,.78f,1,1),"%s",instrumentui::NoteMode(c));else ImGui::TextDisabled("短音");
            ImGui::TableNextColumn();ImGui::Text("%u 条",unsigned(c.noteKeys.size()));
            ImGui::TableNextColumn();ImGui::TextUnformatted(rows[i].channels.empty() ? "—":rows[i].channels.c_str());ImGui::TableNextColumn();ImGui::TextUnformatted(rows[i].programs.empty() ? "—":rows[i].programs.c_str());
            ImGui::TableNextColumn();ImGui::Text("%d",c.middleDo);ImGui::TableNextColumn();ImGui::Text("%+d",c.transpose);ImGui::PopID();
        }ImGui::PopStyleVar();ImGui::EndTable();
    }
    ImGui::TextDisabled("显示 %u / %u 条 · 双击查看音符 · 右键表头管理列",unsigned(visible.size()),unsigned(rows.size()));ImGui::EndChild();
}
void DrawInspector(bool locked,float height) {
    ImGui::BeginChild("inspector",ImVec2(0,height),ImGuiChildFlags_Borders);
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(S(8),S(7)));ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,ImVec2(S(8),S(4)));ui::Heading("音轨配置");
    if (selected<0) {ui::Muted("在左侧选择音轨，配置游戏乐器与音高映射。");ImGui::PopStyleVar(2);ImGui::EndChild();return;}
    ImGui::TextWrapped("#%02d  %s",selected+1,rows[selected].name.c_str());
    ImGui::TextDisabled("%u 个音符 · %u 条自定义映射",unsigned(song->tracks[selected].notes.size()),unsigned(draft.noteKeys.size()));ImGui::Separator();ImGui::BeginDisabled(locked);
    if (ImGui::Checkbox("启用此音轨",&draft.enabled)) editDirty=true;ImGui::SameLine();if (instrumentui::SustainedInput(draft)) editDirty=true;
    ImGui::TextUnformatted("乐器键 · 可选名称 / 输入 ID");ImGui::SetNextItemWidth(-1);if (instrumentui::InstrumentInput(draft,instrumentText,sizeof(instrumentText))) editDirty=true;
    if (ImGui::BeginTable("pitch_settings",2)) {
        ImGui::TableNextColumn();ImGui::TextUnformatted("中音 Do 键");ImGui::SetNextItemWidth(-1);if (ImGui::InputInt("##middle_do",&draft.middleDo,0)) editDirty=true;
        ImGui::TableNextColumn();ImGui::TextUnformatted("移调 / 半音");ImGui::SetNextItemWidth(-1);if (ImGui::InputInt("##transpose",&draft.transpose,0)) editDirty=true;ImGui::EndTable();
    }
    if (ImGui::Button(editDirty ? "应用到此轨 *":"应用到此轨",ImVec2(-1,0)) && Commit()) {error.clear();notice="已保存当前音轨配置。";}
    if (ImGui::Button("乐器配置 / 音高映射…",ImVec2(-1,0))) OpenEditor();ui::Mark("open_editor");ImGui::EndDisabled();
    float half=(ImGui::GetContentRegionAvail().x-S(8))*.5f;
    if (ImGui::Button("查看音符…",ImVec2(half,0))) ViewNotes(selected);ui::Mark("notes_button");ImGui::SameLine();ImGui::BeginDisabled(locked);
    if (ImGui::Button("试听中音 Do",ImVec2(half,0))) TestNote();
    if (ImGui::Button("填入刚弹的中音 Do",ImVec2(-1,0))) {
        auto c=app::GetStatus().game;
        if (!c.lastNoteKey || !c.lastInstrument) error="请先在游戏里手动弹奏你定义的中音 Do。";
        else {draft.middleDo=c.lastNoteKey;draft.instrument=c.lastInstrument;draft.sustained=instrumentui::DefaultSustained(c.lastInstrument);instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),c.lastInstrument);editDirty=true;Commit();}
    }ImGui::EndDisabled();ui::Muted("乐器 5 / 9 / 10 默认长音，可手动修改。11 固定长音，音符开始时播放、按原时值松音。映射按音轨保存。");
    if (locked) ui::Muted("播放或暂停期间配置锁定，停止后可编辑。");ImGui::PopStyleVar(2);ImGui::EndChild();
}
void DrawTransport(const app::Status& status,bool locked) {
    ImGui::BeginChild("transport",ImVec2(0,S(154)),ImGuiChildFlags_Borders);const auto& t=status.transport;bool playing=t.state==player::State::Playing || t.state==player::State::Countdown;
    ImGui::BeginDisabled(!song || loading || playing || editor.open);ImGui::PushStyleColor(ImGuiCol_Button,ImVec4(.12f,.36f,.57f,1));
    if (ImGui::Button(t.state==player::State::Paused ? "继续演奏":"开始演奏",ImVec2(S(122),S(32)))) StartPlayback();ui::Mark("play");ImGui::PopStyleColor();ImGui::EndDisabled();
    ImGui::SameLine();ImGui::BeginDisabled(!playing);if (ImGui::Button("暂停",ImVec2(S(72),S(32)))) Pause();ui::Mark("pause");ImGui::EndDisabled();ImGui::SameLine();if (ImGui::Button("停止",ImVec2(S(72),S(32)))) Stop();ui::Mark("stop");
    ImGui::SameLine(0,S(22));ImGui::BeginDisabled(locked || loading);ImGui::SetNextItemWidth(S(100));if (ImGui::InputInt("速度 %",&speed,0)) dirty=true;
    if (ImGui::IsItemDeactivatedAfterEdit()) Save();ImGui::EndDisabled();ImGui::SameLine(0,S(20));ImGui::SetNextItemWidth(S(70));ImGui::InputInt("倒计时 / 秒",&delay,0);if (ImGui::IsItemDeactivatedAfterEdit()) Save();
    double duration=song ? song->durationMs:0,position=dirty ? 0:t.positionMs;
    if (!seekActive) seekValue=duration>0 ? float(position/duration):0;ImGui::BeginDisabled(!song || loading || editor.open);ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x-S(190));
    ImGui::SliderFloat("##seek",&seekValue,0,1,"");seekActive=ImGui::IsItemActive();if (ImGui::IsItemDeactivatedAfterEdit()) Seek(seekValue*duration);
    ImGui::EndDisabled();ImGui::SameLine();ImGui::Text("%s / %s",ui::Time(position,true).c_str(),ui::Time(duration).c_str());
    auto text=StateText(status);ImGui::TextColored(t.state==player::State::Error ? ImVec4(1,.43f,.4f,1):ImVec4(.64f,.79f,.93f,1),"%s",loading ? "正在读取 MIDI…":text.c_str());
    if (t.skippedNotes) ImGui::TextWrapped("略过 %u 个音符 · 最近：%s",unsigned(t.skippedNotes),ui::Utf8(ui::Widen(t.warning)).c_str());
    if (!error.empty()) {ImGui::PushStyleColor(ImGuiCol_Text,ImVec4(1,.48f,.44f,1));ImGui::TextWrapped("%s",error.c_str());ImGui::PopStyleColor();ImGui::SameLine();if (ImGui::SmallButton("清除提示")) error.clear();}
    else if (!notice.empty() && !playing) ImGui::TextDisabled("%s",notice.c_str());ImGui::EndChild();
}
void DrawLog(float height) {
    ImGui::BeginChild("manual_log",ImVec2(0,height),ImGuiChildFlags_Borders);ui::Heading("手动弹奏日志");const auto notes=app::ManualNotes();std::string text;
    for (const auto& note:notes) text+=game::FormatManualNote(note)+"\n";
    ImGui::BeginChild("log_lines",ImVec2(0,0));bool bottom=ImGui::GetScrollY()>=ImGui::GetScrollMaxY()-S(4);
    if (text.empty()) ui::Muted("等待游戏内手动弹奏… 仅记录手动输入，最多保留 200 条。");else ImGui::TextUnformatted(text.c_str());
    if (text!=lastLog && bottom) ImGui::SetScrollHereY(1);lastLog=text;ImGui::EndChild();ImGui::EndChild();
}
void Draw() {
    const auto status=app::GetStatus();bool locked=Active(status.transport.state) || loading || editor.open;auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(0,0));ImGui::SetNextWindowSize(display);
    ImGui::Begin("MIDI STUDIO###main",nullptr,ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoCollapse|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::AlignTextToFramePadding();ImGui::PushFont(nullptr,22);ImGui::TextUnformatted("MIDI STUDIO");ImGui::PopFont();ImGui::SameLine();ImGui::TextDisabled(previewMode ? "自动演奏 / 界面预览":"自动演奏 / 游戏插件");
    ImGui::SameLine(0,S(24));ImGui::BeginDisabled(editor.open || loading);
    if (ImGui::RadioButton("自动演奏 / 时间轴",!manualView && !compositionView) && (manualView || compositionView)) {performanceView.Release();composerView.Release();Stop();manualView=compositionView=false;dirty=true;}ui::Mark("mode_auto");
    ImGui::SameLine();if (ImGui::RadioButton("手动演奏 / 键盘",manualView) && !manualView && Commit()) {composerView.Release();Stop();manualView=true;compositionView=false;dirty=true;}ui::Mark("mode_manual");
    ImGui::SameLine();if (ImGui::RadioButton("创作模式 / 钢琴卷帘",compositionView) && !compositionView && Commit()) {performanceView.Release();Stop();manualView=false;compositionView=true;dirty=true;viewer.open=false;}ui::Mark("mode_composer");
    ImGui::EndDisabled();
    ImGui::SameLine(std::max(ImGui::GetItemRectMax().x-ImGui::GetWindowPos().x+S(20),ImGui::GetWindowWidth()-S(325)));
    if (ImGui::Checkbox("手动弹奏日志",&manualLogging)) {app::SetManualLogging(manualLogging);lastLog.clear();}ui::Mark("logging");ImGui::SameLine();if (ImGui::Button("使用说明")) ImGui::OpenPopup("help");
    if (ImGui::BeginPopup("help")) {ImGui::PushTextWrapPos(ImGui::GetCursorPosX()+S(470));
        ImGui::TextUnformatted("1. 在游戏演奏界面手动弹一个音，短音轨需手动弹一个短音。\n2. 加载 MIDI，勾选音轨，在右侧选择游戏乐器。\n3. 需要时校准中音 Do，或打开音高映射指定 noteKey。\n4. 点击开始，在倒计时内切回游戏。\n\n时间轴：点或拖动顶部刻度选播放位置；Ctrl+滚轮缩放，Shift+滚轮平移。黄色竖线表示当前曲内时间；跟随播放会自动翻到后续片段。双击音轨查看音符。\n音轨表：默认显示映射条数，右键表头管理其他列。\n\nCtrl+-  显示 / 隐藏\nCtrl+Alt+P  开始 / 暂停\nCtrl+Alt+S  停止");ImGui::PopTextWrapPos();ImGui::EndPopup();}
    ImGui::Separator();float logHeight=manualLogging ? S(150):0;
    if (compositionView) {
        float height=std::max(S(260),ImGui::GetContentRegionAvail().y-S(44)-(manualLogging ? logHeight+S(9):0));
        composerView.Draw(status,window,height,smokeMode || GetForegroundWindow()==window);
    }else if (manualView) {
        float height=std::max(S(260),ImGui::GetContentRegionAvail().y-S(44)-(manualLogging ? logHeight+S(9):0));
        performanceView.Draw(status,height,GetForegroundWindow()==window);
    } else {
    ImGui::BeginDisabled(locked);if (ImGui::Button("加载 MIDI…",ImVec2(S(112),0))) OpenFile();ImGui::SameLine();if (ImGui::Button("加载示例")) LoadFile(directory+L"..\\examples\\飞鼠plus.mid");ImGui::EndDisabled();ImGui::SameLine();
    size_t noteCount=0,enabledCount=0;
    if (song) {for (size_t i=0;i<song->tracks.size();++i) {noteCount+=song->tracks[i].notes.size();if (configs[i].enabled) ++enabledCount;}ImGui::TextDisabled("%u 条音轨  /  启用 %u 条  /  %u 个音符  /  %s",unsigned(rows.size()),unsigned(enabledCount),unsigned(noteCount),ui::Time(song->durationMs).c_str());}else ImGui::TextDisabled("尚未加载 MIDI");
    ImGui::TextDisabled("%s",path.empty() ? "选择 .mid / .midi 文件，或载入示例开始配置":ui::Utf8(path).c_str());
    float bodyHeight=std::max(S(150),ImGui::GetContentRegionAvail().y-S(154)-S(44)-(manualLogging ? logHeight+S(9):0)-S(9));
    if (ImGui::BeginTable("workspace",2,ImGuiTableFlags_Resizable)) {ImGui::TableSetupColumn("tracks",ImGuiTableColumnFlags_WidthStretch);ImGui::TableSetupColumn("settings",ImGuiTableColumnFlags_WidthFixed,S(320));
        ImGui::TableNextColumn();DrawTracks(locked,bodyHeight);ImGui::TableNextColumn();DrawInspector(locked,bodyHeight);ImGui::EndTable();}
    DrawTransport(status,locked);
    }if (manualLogging) DrawLog(logHeight);
    if (previewMode) ImGui::TextDisabled("PREVIEW · 调度模拟，不连接游戏");else ImGui::TextColored(status.game.ready && status.game.threadId ? ImVec4(.4f,.8f,.6f,1):ImVec4(.93f,.69f,.36f,1),"入口 %s · 线程 %lu · 短音 %s · 最近乐器 %d / noteKey %d",ui::Utf8(ui::Widen(status.game.status)).c_str(),status.game.threadId,status.game.shortReady ? "已校准":"未校准",status.game.lastInstrument,status.game.lastNoteKey);
    ImGui::SameLine();ImGui::TextDisabled("| Ctrl+- 显示 / 隐藏");player::TrackConfig accepted;
    if (editor.Draw(accepted) && editorTrack>=0 && editorTrack<int(configs.size())) {configs[editorTrack]=accepted;dirty=true;editDirty=false;Select(editorTrack);if (Save()) {notice="乐器设置与音高映射已保存。";error.clear();}}
    ImGui::End();viewer.Draw();
}
bool SmokeChecks() {
    bool ok=song && song->tracks.size()==14 && selected>=0 && std::abs(song->durationMs-91041.7759166667)<.001;
    timeline::Viewport view;view.duration=100000;view.Zoom(4,.25);
    ok=ok && std::abs(view.Time(.25)-25000)<.001 && std::abs(view.Fraction(25000)-.25)<.0001;
    view.Follow(90000);ok=ok && view.Fraction(90000)>=0 && view.Fraction(90000)<=1;
    view.start=200000;view.Clamp();ok=ok && view.start==75000 && view.Time(2)==100000;
    view.Zoom(1);ok=ok && view.start==0 && view.Span()==100000;
    size_t notes=0;for (const auto& track:song->tracks) notes+=track.notes.size();ok=ok && notes==371;const bool sustained[]={false,false,false,false,true,false,false,false,true,true,true};
    ok=ok && instrumentui::PresetCount()==11;
    for (int i=0;i<instrumentui::PresetCount();++i) {int id=instrumentui::PresetId(i),parsed=0;instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),id);ok=instrumentui::ReadInstrument(instrumentText,parsed) && parsed==id && instrumentui::DefaultSustained(id)==sustained[i] && ok;}
    int parsed=0;ok=ok && !instrumentui::ReadInstrument("10oops",parsed) && !instrumentui::ReadInstrument("",parsed) && instrumentui::ReadInstrument("12345",parsed) && parsed==12345;
    ok=ok && instrumentui::ReadInstrument("余音",parsed) && parsed==8 && instrumentui::ReadInstrument("沃雅妮莎",parsed) && parsed==11;
    player::TrackConfig longInstrument;longInstrument.instrument=11;longInstrument.sustained=false;std::string validationError;
    ok=ok && instrumentui::Validate(longInstrument,"11",validationError) && longInstrument.sustained && std::string(instrumentui::NoteMode(longInstrument))=="长音";
    Select(selected);draft.instrument=5;draft.middleDo=36;draft.transpose=-12;draft.sustained=true;instrumentui::SetInstrumentText(instrumentText,sizeof(instrumentText),5);editDirty=true;ok=Commit() && ok;
    draft.sustained=false;editDirty=true;ok=Commit() && ok;Select(selected);ok=ok && !draft.sustained && GetPrivateProfileIntW(Section(selected).c_str(),L"sustained",1,ini.c_str())==0;
    instrumentui::Editor e;e.Open(song->tracks[selected],TrackTitle(selected),configs[selected]);e.draft.noteKeys.clear();e.draft.instrument=12345;e.draft.middleDo=48;e.draft.sustained=true;instrumentui::SetInstrumentText(e.instrument,sizeof(e.instrument),12345);
    e.source=48;e.target=60;e.pending=true;ok=e.SetMapping() && ok;e.source=58;e.target=47;e.pending=true;player::TrackConfig accepted;ok=e.Accept(accepted) && ok;
    ok=ok && accepted.noteKeys==std::map<int,int>({{48,60},{58,47}}) && accepted.instrument==12345 && accepted.transpose==-12 && accepted.sustained;
    configs[selected]=accepted;editDirty=false;Select(selected);ok=Save() && ok;ok=ok && ReadNoteMap(Section(selected))==accepted.noteKeys;
    e.Open(song->tracks[selected],TrackTitle(selected),accepted);e.draft.noteKeys.clear();e.open=false;ok=ok && configs[selected].noteKeys==accepted.noteKeys;
    e.Open(song->tracks[selected],TrackTitle(selected),accepted);e.source=128;e.target=60;e.pending=true;ok=ok && !e.Accept(accepted);
    auto fixture=std::make_shared<midi::Song>();fixture->tracks.resize(2);midi::Note note;note.pitch=60;note.startMs=1000;note.endMs=1500;note.velocity=80;note.channel=2;fixture->tracks[0].notes.push_back(note);
    note.pitch=48;note.startMs=0;note.endMs=250;note.velocity=100;note.channel=1;fixture->tracks[0].notes.push_back(note);note.pitch=58;note.endMs=600;note.velocity=60;note.channel=0;fixture->tracks[0].notes.push_back(note);
    player::TrackConfig fixtureConfig;fixtureConfig.noteKeys[48]=60;instrumentui::NotesViewer v;v.Open(fixture,0,"fixture",fixtureConfig);
    const char* expected[3][10]={{"1","0:00.000","0:00.250","0.250","48","C 3","60","自定义","100","2"},{"2","0:00.000","0:00.600","0.600","58","A# / 降Si 3","47","默认","60","1"},{"3","0:01.000","0:01.500","0.500","60","C 4","48","默认","80","3"}};
    for (size_t row=0;row<3;++row) for (int col=0;col<10;++col) ok=ok && v.Cell(row,col)==expected[row][col];
    v.onlyCustom=true;v.Filter();ok=ok && v.filtered.size()==1;v.Open(fixture,1,"empty",fixtureConfig);ok=ok && v.filtered.empty();
    auto plan=player::Build(*song,configs,1);ok=ok && !plan->events.empty();app::SetManualLogging(true);app::SetManualLogging(false);return ok && app::ManualNotes().empty();
}
struct SmokeStep {
    const char* target;std::function<bool()> verify;float x,y,endX,endY,wheel;
    bool control;
    SmokeStep(const char* t,std::function<bool()> v,float a=.5f,float b=.5f,float c=-1,float d=-1,float w=0,bool ctrl=false):target(t),verify(v),x(a),y(b),endX(c),endY(d),wheel(w),control(ctrl) {}
};
std::vector<SmokeStep> smokeSteps;
size_t smokeStep=0;
double composerSeekPosition=0;
size_t composerRecordingBase=0;
std::vector<composition::Note> composerGroupBefore;
void SmokeReport(const std::string& message) {
    std::ofstream out(directory+L"MidiPlayer.ui-smoke.log",std::ios::app);out<<message<<"\n";
}
void PrepareSmoke() {
    smokeSteps={
        {"table_view",[] {return tableView && mappingColumnVisible;}},
        {"timeline_view",[] {return !tableView;}},
        {"timeline_selected_lane",[] {return !viewer.open;}},
        {"timeline_selected_lane",[] {return viewer.open;}},
        {"notes_close",[] {return !viewer.open;}},
        {"timeline_zoom",[] {return timelineView.viewport.zoom>1;}},
        {"timeline_pan",[] {return timelineView.viewport.start>0 && !timelineView.follow;}},
        {"timeline_fit",[] {return timelineView.viewport.zoom==1 && timelineView.viewport.start==0;}},
        {"timeline_follow",[] {return timelineView.follow;}},
        {"timeline_ruler",[] {auto s=app::GetStatus().transport;bool ok=s.state==player::State::Paused && std::abs(s.positionMs-song->durationMs*.75)<250;SmokeReport("Ruler drag: position="+std::to_string(s.positionMs)+" expected="+std::to_string(song->durationMs*.75)+" state="+std::to_string(int(s.state)));Stop();app::PreviewTick();return ok;}},
        {"open_editor",[] {return editor.open;}},
        {"instrument_presets",[] {return editor.open;}},
        {"preset_8",[] {return editor.draft.instrument==8 && !editor.draft.sustained;}},
        {"instrument_presets",[] {return editor.open;}},
        {"preset_11",[] {return editor.draft.instrument==11 && editor.draft.sustained;}},
        {"editor_save",[] {return !editor.open && configs[selected].instrument==11 && configs[selected].sustained && GetPrivateProfileIntW(Section(selected).c_str(),L"instrument",0,ini.c_str())==11;}},
        {"notes_button",[] {return viewer.open && viewer.config.instrument==11 && std::string(instrumentui::NoteMode(viewer.config))=="长音";}},
        {"notes_close",[] {return !viewer.open;}},
        {"open_editor",[] {return editor.open && editor.draft.instrument==11;}},
        {"instrument_presets",[] {return editor.open;}},
        {"preset_5",[] {return editor.draft.instrument==5 && editor.draft.sustained;}},
        {"editor_long",[] {bool ok=!editor.draft.sustained;editor.source=50;editor.target=64;editor.pending=true;return ok;}},
        {"editor_save",[] {return !editor.open && configs[selected].instrument==5 && !configs[selected].sustained && configs[selected].noteKeys.count(50) && configs[selected].noteKeys.at(50)==64;}},
        {"open_editor",[] {bool ok=editor.open;editor.draft.instrument=9;instrumentui::SetInstrumentText(editor.instrument,sizeof(editor.instrument),9);editor.draft.noteKeys.clear();return ok;}},
        {"editor_cancel",[] {return !editor.open && configs[selected].instrument==5 && configs[selected].noteKeys.count(50);}},
        {"notes_button",[] {return viewer.open && viewer.order.size()==song->tracks[selected].notes.size();}},
        {"notes_close",[] {return !viewer.open;}},
        {"logging",[] {return manualLogging;}},
        {"logging",[] {delay=0;return !manualLogging && app::ManualNotes().empty();}},
        {"play",[] {auto t=app::GetStatus().transport;return t.state==player::State::Playing && t.positionMs>0;}},
        {"pause",[] {return app::GetStatus().transport.state==player::State::Paused;}},
        {"timeline_ruler",[] {auto t=app::GetStatus().transport;return t.state==player::State::Paused && std::abs(t.positionMs-song->durationMs*.75)<250;}},
        {"play",[] {return app::GetStatus().transport.state==player::State::Playing && app::GetStatus().transport.positionMs>song->durationMs*.74;}},
        {"stop",[] {composerView.project=composition::Project{};return app::GetStatus().transport.state==player::State::Stopped;}},
        {"mode_composer",[] {return compositionView && !manualView;}},
        {"composer_ruler",[] {return app::GetStatus().transport.state==player::State::Stopped && std::abs(composerView.PositionBeat()-4)<.05;},.25f},
        {"composer_grid",[] {return composerView.project.NoteCount()==1;},.3f},
        {"composer_note",[] {const auto& n=composerView.project.tracks[0].notes[0];return n.row==8 && n.start>0;},.25f,.5f,1.25f,-1.2f},
        {"composer_note",[] {return composerView.project.tracks[0].notes[0].length>composition::PPQ && composerView.NewNoteLength()==composerView.project.tracks[0].notes[0].length;},.95f,.5f,1.95f,.5f},
        {"composer_grid",[] {const auto& notes=composerView.project.tracks[0].notes;return notes.size()==2 && notes[1].length==notes[0].length;},.65f},
        {"composer_delete_key",[] {return composerView.project.NoteCount()==1;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==2;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==1;}},
        {"composer_undo",[] {return composerView.project.tracks[0].notes[0].length==composition::PPQ;}},
        {"composer_undo",[] {return composerView.project.tracks[0].notes[0].row==9;}},
        {"composer_add_track",[] {return composerView.project.tracks.size()==2 && composerView.SelectedTrack()==1;}},
        {"instrument_presets",[] {return compositionView;}},
        {"preset_11",[] {return composerView.project.tracks[1].instrument==11 && composerView.project.tracks[1].sustained;}},
        {"composer_grid",[] {return composerView.project.NoteCount()==2 && app::GetStatus().manualVoices==1;},.45f},
        {"composer_piano",[] {return composerView.project.NoteCount()==2 && app::GetStatus().manualVoices==1;}},
        {"composer_solo",[] {const auto& p=composerView.project;auto exported=midi::Parse(p.Midi());return !p.tracks[0].visible && p.tracks[1].visible && exported.tracks[1].notes.size()==1 && exported.tracks[2].notes.size()==1 && exported.tracks[2].gameInstrument==11;}},
        {"composer_show_all",[] {return composerView.project.tracks[0].visible && composerView.project.tracks[1].visible;}},
        {"composer_delete_note",[] {return composerView.project.NoteCount()==1;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==2;}},
        {"composer_grid",[] {return composerView.Zoom()>2;},.5f,.5f,-1,-1,1},
        {"composer_ruler",[] {composerSeekPosition=composerView.project.Time(composerView.PositionBeat());return composerSeekPosition>1000 && app::GetStatus().transport.state==player::State::Stopped;},.5f},
        {"composer_play",[] {auto t=app::GetStatus().transport;return t.state==player::State::Playing && t.positionMs>=composerSeekPosition && t.positionMs<composerSeekPosition+250;}},
        {"composer_grid",[] {return composerView.Zoom()>3 && app::GetStatus().transport.state==player::State::Playing;},.5f,.5f,-1,-1,1},
        {"composer_ruler",[] {auto t=app::GetStatus().transport;return t.state==player::State::Playing && t.positionMs<composerSeekPosition-500;},.05f,.5f,.1f,.5f},
        {"composer_play",[] {return app::GetStatus().transport.state==player::State::Paused;}},
        {"composer_ruler",[] {auto t=app::GetStatus().transport;composerSeekPosition=t.positionMs;return t.state==player::State::Paused && t.positionMs>3000;},.6f,.5f,.7f,.5f},
        {"composer_play",[] {auto t=app::GetStatus().transport;return t.state==player::State::Playing && t.positionMs>=composerSeekPosition;}},
        {"composer_stop",[] {return app::GetStatus().transport.state==player::State::Stopped && composerView.PositionBeat()==0 && composerView.OpenMidi(directory+L"..\\examples\\飞鼠plus.mid") && composerView.project.tracks.size()==6 && composerView.project.NoteCount()>0;}},
        {"composer_undo",[] {return composerView.project.tracks.size()==2 && composerView.project.NoteCount()==2;}},
        {"composer_first_note",[] {return composerView.SelectionCount()==1;}},
        {"composer_other_note",[] {composerGroupBefore={composerView.project.tracks[0].notes[0],composerView.project.tracks[1].notes[0]};return composerView.SelectionCount()==2;},.5f,.5f,-1,-1,0,true},
        {"composer_delete_key",[] {return composerView.project.NoteCount()==0;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==2;}},
        {"composer_first_note",[] {return composerView.SelectionCount()==1 && composerView.NewNoteLength()==composerView.project.tracks[0].notes[0].length;}},
        {"composer_other_note",[] {return composerView.SelectionCount()==2;},.5f,.5f,-1,-1,0,true},
        {"composer_note",[] {const auto& p=composerView.project;int ticks=p.tracks[0].notes[0].start-composerGroupBefore[0].start,rowDelta=p.tracks[0].notes[0].row-composerGroupBefore[0].row;return ticks>0 && rowDelta<0 && p.tracks[1].notes[0].start-composerGroupBefore[1].start==ticks && p.tracks[1].notes[0].row-composerGroupBefore[1].row==rowDelta && p.tracks[0].notes[0].length==composerGroupBefore[0].length && p.tracks[1].notes[0].length==composerGroupBefore[1].length;},.25f,.5f,1.25f,-1.2f},
        {"composer_undo",[] {return composerView.project.tracks[0].notes[0].start==composerGroupBefore[0].start && composerView.project.tracks[1].notes[0].row==composerGroupBefore[1].row;}},
        {"composer_first_note",[] {return composerView.SelectionCount()==1;},.5f,.5f,-1,-1,0,true},
        {"composer_other_note",[] {return composerView.SelectionCount()==2;},.5f,.5f,-1,-1,0,true},
        {"composer_first_note",[] {return composerView.SelectionCount()==1;},.5f,.5f,-1,-1,0,true},
        {"composer_delete_note",[] {return composerView.project.NoteCount()==1;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==2;}},
        {"instrument_presets",[] {return compositionView;}},
        {"preset_11",[] {return composerView.project.tracks[composerView.SelectedTrack()].instrument==11;}},
        {"composer_ruler",[] {composerSeekPosition=composerView.project.Time(composerView.PositionBeat());return composerSeekPosition>0;},.1f},
        {"composer_record",[] {composerRecordingBase=composerView.project.NoteCount();return composerView.Recording();}},
        {"composer_record_keys",[] {const auto& p=composerView.project;size_t count=0;for (const auto& n:p.tracks[composerView.SelectedTrack()].notes) if ((n.row==6 || n.row==13) && n.length>1 && p.Time(n.start/double(composition::PPQ))>=composerSeekPosition) ++count;return composerView.Recording() && p.NoteCount()==composerRecordingBase+2 && count==2 && app::GetStatus().manualVoices==0;}},
        {"composer_piano",[] {return composerView.Recording() && composerView.project.NoteCount()==composerRecordingBase+3;},.5f,.5f,.5f,.5f},
        {"composer_ruler",[] {return composerView.Recording() && composerView.PositionBeat()>12;},.4f,.5f,.45f,.5f},
        {"composer_record",[] {return !composerView.Recording() && composerView.project.Time(composerView.PositionBeat())>composerSeekPosition && app::GetStatus().manualVoices==0;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==composerRecordingBase;}},
        {"composer_ruler",[] {return composerView.PositionBeat()<4;},.1f},
        {"composer_record",[] {return composerView.Recording();}},
        {"composer_record_held_keys",[] {bool ok=composerView.Recording() && app::GetStatus().manualVoices==2;composerView.Release();app::PreviewTick();ui::TestKey(ImGuiKey_Q,false);ui::TestKey(ImGuiKey_A,false);return ok && !composerView.Recording() && app::GetStatus().manualVoices==0;}},
        {"composer_undo",[] {return composerView.project.NoteCount()==composerRecordingBase;}},
        {"composer_ruler",[] {return composerView.PositionBeat()>31;},.9999f},
        {"composer_record",[] {return !composerView.Recording() && composerView.PositionBeat()==composerView.project.beats && app::GetStatus().manualVoices==0;}},
        {"mode_auto",[] {return !compositionView && !manualView && dirty;}}
    };
}
std::wstring Automation() {
    if ((!snapshotMode && !smokeMode) || !song || loading) return L"";++automationFrames;
    if (smokeMode && automationPhase==0 && automationFrames==4) {
        if (!SmokeChecks()) {SmokeReport("FAIL: configuration or note data checks");resultCode=3;running=false;return L"";}
        SmokeReport("PASS: presets, mappings, persistence, note times, empty tracks, filters");PrepareSmoke();automationPhase=1;automationFrames=0;return L"";
    }
    if (smokeMode) {
        if (!automationPhase) return L"";auto& step=smokeSteps[smokeStep];
        bool ruler=std::string(step.target)=="timeline_ruler",recordHeld=std::string(step.target)=="composer_record_held_keys",recordKeys=recordHeld || std::string(step.target)=="composer_record_keys",deleteKey=std::string(step.target)=="composer_delete_key";
        if (step.wheel && (automationFrames==4 || automationFrames==5)) SmokeReport("Wheel: ctrl="+std::to_string(ImGui::GetIO().KeyCtrl)+" delta="+std::to_string(ImGui::GetIO().MouseWheel)+" zoom="+std::to_string(composerView.Zoom()));
        if (automationFrames==3 && !(deleteKey ? (ui::TestMouse("composer_record",false) && ui::TestKey(ImGuiKey_Delete,true)):recordKeys ? (ui::TestKey(ImGuiKey_Q,true) && ui::TestKey(ImGuiKey_A,true)):step.wheel ? ui::TestWheel(step.target,true,step.wheel):ui::TestMouse(step.target,true,ruler ? .25f:step.x,step.y,step.control))) {SmokeReport(std::string("FAIL: missing widget ")+step.target);resultCode=4;running=false;}
        if (automationFrames==4 && !recordKeys && !deleteKey) {if (step.wheel) ui::TestWheel(step.target,true,0);else ui::TestMouse(step.target,step.endX>=0,ruler ? .75f:step.endX>=0 ? step.endX:step.x,step.endX>=0 ? step.endY:step.y,step.control);}
        if (automationFrames==6 && step.wheel) ui::TestWheel(step.target,false,0);
        if (automationFrames==5 && recordKeys && !recordHeld) {ui::TestKey(ImGuiKey_Q,false);ui::TestKey(ImGuiKey_A,false);}
        if (automationFrames==5 && deleteKey) ui::TestKey(ImGuiKey_Delete,false);
        if (automationFrames==5 && step.endX>=0) ui::TestMouse(step.target,false,step.endX,step.endY,step.control);
        if (automationFrames==8) {
            bool ok=step.verify();SmokeReport(std::string(ok ? "PASS: click ":"FAIL: click ")+step.target);
            if (!ok) {resultCode=5;running=false;}
            else if (++smokeStep==smokeSteps.size()) running=false;
            automationFrames=0;
        }return L"";
    }
    if (automationFrames!=5) return L"";const wchar_t* names[]={L"panel_preview.bmp",L"panel_manual_log_preview.bmp",L"instrument_config_preview.bmp",L"track_notes_preview.bmp",L"panel_compact_preview.bmp",L"panel_table_preview.bmp",L"panel_playing_preview.bmp",L"composer_preview.bmp",L"composer_compact_preview.bmp",L"composer_recording_compact_preview.bmp",L"composer_recording_preview.bmp"};return directory+names[automationPhase];
}
void AfterFrame() {
    if (!snapshotMode || !song || automationFrames!=5) return;++automationPhase;automationFrames=0;
    if (automationPhase==1) {manualLogging=true;app::SetManualLogging(true);}
    else if (automationPhase==2) {manualLogging=false;app::SetManualLogging(false);OpenEditor();}
    else if (automationPhase==3) {editor.open=false;editor.request=false;ViewNotes(selected);}
    else if (automationPhase==4) {viewer.open=false;RECT rect{0,0,1120,720};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);}
    else if (automationPhase==5) {RECT rect{0,0,1380,840};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);tableView=true;}
    else if (automationPhase==6) {tableView=false;delay=0;StartPlayback();app::PreviewTick();Seek(30000);timelineView.viewport.Zoom(3,0);timelineView.Reveal(selected);}
    else if (automationPhase==7) {Stop();app::PreviewTick();compositionView=true;composerView.Demo();}
    else if (automationPhase==8) {RECT rect{0,0,1080,720};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);}
    else if (automationPhase==9) composerView.Demo(true);
    else if (automationPhase==10) {RECT rect{0,0,1380,840};AdjustWindowRect(&rect,WS_OVERLAPPEDWINDOW,FALSE);SetWindowPos(window,nullptr,0,0,rect.right-rect.left,rect.bottom-rect.top,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);}
    else running=false;
}
bool HotkeysAllowed(HWND hwnd) {HWND front=GetForegroundWindow();wchar_t cls[128]{};GetClassNameW(front,cls,128);return front==hwnd || GetWindow(front,GW_OWNER)==hwnd || wcscmp(cls,L"UnityWndClass")==0;}
LRESULT CALLBACK WindowProc(HWND hwnd,UINT message,WPARAM wp,LPARAM lp) {
    if (message==WM_KILLFOCUS || (message==WM_ACTIVATE && LOWORD(wp)==WA_INACTIVE) || (message==WM_SHOWWINDOW && !wp)) {performanceView.Release();composerView.LoseFocus();}
    if (ImGui::GetCurrentContext() && ImGui_ImplWin32_WndProcHandler(hwnd,message,wp,lp)) return TRUE;
    switch (message) {
    case WM_SIZE:if (wp!=SIZE_MINIMIZED) ui::Resize(LOWORD(lp),HIWORD(lp));else composerView.LoseFocus();return 0;
    case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={LONG(S(1080)),LONG(S(720))};return 0;
    case WM_DPICHANGED:{auto r=reinterpret_cast<RECT*>(lp);SetWindowPos(hwnd,nullptr,r->left,r->top,r->right-r->left,r->bottom-r->top,SWP_NOZORDER|SWP_NOACTIVATE);return 0;}
    case WM_ERASEBKGND:return 1;case WM_SYSCOMMAND:if ((wp&0xfff0)==SC_KEYMENU) return 0;break;
    case Loaded:LoadedFile();return 0;case ShowPanel:ShowWindow(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);return 0;
    case WM_CLOSE:performanceView.Release();composerView.Release();if (!Commit()) {ShowWindow(hwnd,SW_RESTORE);return 0;}Save();if (previewMode) {Stop();app::PreviewTick();running=false;}else ShowWindow(hwnd,SW_HIDE);return 0;
    case WM_DESTROY:published.store(nullptr);running=false;PostQuitMessage(0);return 0;
    case WM_HOTKEY:
        if (!HotkeysAllowed(hwnd)) return 0;
        if (wp==1) {if (IsWindowVisible(hwnd)) {performanceView.Release();composerView.LoseFocus();ShowWindow(hwnd,SW_HIDE);}else {ShowWindow(hwnd,SW_RESTORE);SetForegroundWindow(hwnd);}}
        else if (wp==3) {performanceView.Release(true);composerView.Release();Stop();}else if (wp==2 && !editor.open && !manualView) {if (compositionView) composerView.TogglePlayback(app::GetStatus());else {auto state=app::GetStatus().transport.state;if (state==player::State::Playing || state==player::State::Countdown) Pause();else StartPlayback();}}return 0;
    }return DefWindowProcW(hwnd,message,wp,lp);
}
}
int Run(HINSTANCE module,bool preview,bool smoke,bool snapshot) {
    previewMode=preview;smokeMode=smoke;snapshotMode=snapshot;wchar_t filename[32768]{};GetModuleFileNameW(module,filename,32768);directory=filename;directory=directory.substr(0,directory.find_last_of(L"/\\")+1);
    ini=directory+(smoke ? L"MidiPlayer.smoke.ini":snapshot ? L"MidiPlayer.preview.ini":L"MidiPlayer.ini");speed=GetPrivateProfileIntW(L"player",L"speed",100,ini.c_str());delay=GetPrivateProfileIntW(L"player",L"delay",3,ini.c_str());
    performanceView.Load(ini);
    // The smoke sequence assumes a fresh composer; normal launches restore the saved editor state.
    if (!smoke) composerView.Load(ini+L".composition");
    if (smoke) {std::ofstream out(directory+L"MidiPlayer.ui-smoke.log");}
    if (preview) ImGui_ImplWin32_EnableDpiAwareness();WNDCLASSEXW wc{};wc.cbSize=sizeof(wc);wc.lpfnWndProc=WindowProc;wc.hInstance=module;wc.lpszClassName=windowClass;wc.hCursor=LoadCursor(nullptr,IDC_ARROW);wc.hIcon=LoadIcon(nullptr,IDI_APPLICATION);RegisterClassExW(&wc);
    RECT work{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);int width=std::min(1380,int(work.right-work.left-48)),height=std::min(880,int(work.bottom-work.top-48));
    window=CreateWindowExW(0,windowClass,preview ? L"MIDI 自动演奏 · ImGui 界面预览：小明石开发":L"MIDI 自动演奏 · ImGui 游戏插件：小明石开发",WS_OVERLAPPEDWINDOW,work.left+24,work.top+24,width,height,nullptr,nullptr,module,nullptr);
    if (!window) return 10;if (!ui::Initialize(window)) {MessageBoxW(window,L"无法创建 DirectX 11 界面，或找不到中文字体。",L"MIDI 自动演奏",MB_ICONERROR);DestroyWindow(window);return 11;}
    published.store(window);ui::EnableTestInput(smoke);RegisterHotKey(window,1,MOD_CONTROL|MOD_NOREPEAT,VK_OEM_MINUS);RegisterHotKey(window,2,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'P');RegisterHotKey(window,3,MOD_CONTROL|MOD_ALT|MOD_NOREPEAT,'S');
    if (preview && !smoke && !snapshot) ShowWindow(window,SW_SHOW);if (preview) LoadFile(directory+L"..\\examples\\飞鼠plus.mid");DWORD began=GetTickCount();MSG msg{};
    while (running) {
        while (PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)) {if (msg.message==WM_QUIT) {running=false;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if (!running) break;app::PreviewTick();if (compositionView) composerView.Tick();bool automation=smoke || snapshot;
        if (automation && GetTickCount()-began>30000) {resultCode=6;break;}
        if (automation || (IsWindowVisible(window) && !IsIconic(window))) {if (!ui::NewFrame()) {resultCode=12;break;}Draw();auto snapshotPath=Automation();if (!ui::Render(snapshotPath)) {resultCode=13;break;}AfterFrame();}
        else MsgWaitForMultipleObjects(0,nullptr,FALSE,25,QS_ALLINPUT);
    }
    performanceView.Release(true);composerView.Release();app::PreviewTick();app::SetManualLogging(false);published.store(nullptr);for (int i=1;i<=3;++i) UnregisterHotKey(window,i);if (loader.joinable()) loader.join();pendingLoad.reset();ui::Shutdown();if (IsWindow(window)) DestroyWindow(window);UnregisterClassW(windowClass,module);return resultCode;
}
void Show() {auto hwnd=published.load();if (hwnd) PostMessageW(hwnd,ShowPanel,0,0);}
}
