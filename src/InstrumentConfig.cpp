#include "InstrumentConfig.h"
#include "ImGuiHost.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <numeric>
#include <sstream>
namespace instrumentui {
namespace {
struct Instrument {int id;const wchar_t* name;};
const Instrument presets[]={{1,L"风物之诗琴"},{2,L"镜花之琴"},{3,L"绮筵之鼓"},{4,L"老旧的琴诗"},{5,L"晚风圆号"},
    {6,L"悠可琴"},{7,L"聚聚鼓"},{8,L"余音"},{9,L"跃律琴"},{10,L"谐律键琴"},{11,L"沃雅妮莎"}};
const char* editorName="乐器配置 · MIDI 音高转换：小明石开发###instrument_editor";
const char* viewerName="音轨音符 · 时间与音高：小明石开发###notes_viewer";
}
int PresetCount() {return int(sizeof(presets)/sizeof(presets[0]));}
int PresetId(int index) {return index>=0 && index<PresetCount() ? presets[index].id:0;}
std::wstring InstrumentLabel(int id) {
    for (const auto& entry:presets) if (id==entry.id) return std::to_wstring(id)+L"："+entry.name;
    return std::to_wstring(id)+L"：自定义";
}
bool DefaultSustained(int id) {return id==5 || id==9 || id==10 || id==11;}
const char* NoteMode(const player::TrackConfig& config) {return player::UsesSustained(config.instrument,config.sustained) ? "长音":"短音";}
bool SustainedInput(player::TrackConfig& config) {
    if (config.instrument==11) {ImGui::TextDisabled("长音（固定）");return false;}
    return ImGui::Checkbox("长音乐器",&config.sustained);
}
void SetInstrumentText(char* text,size_t size,int id) {
    auto label=ui::Utf8(InstrumentLabel(id));snprintf(text,size,"%s",label.c_str());
}
bool ReadInstrument(const char* text,int& id) {
    auto value=ui::Widen(text);auto first=value.find_first_not_of(L" \t\r\n");if (first==std::wstring::npos) return false;
    value=value.substr(first,value.find_last_not_of(L" \t\r\n")-first+1);
    for (const auto& entry:presets) if (value==entry.name || value==InstrumentLabel(entry.id)) {id=entry.id;return true;}
    wchar_t* end=nullptr;long number=wcstol(value.c_str(),&end,10);
    if (end!=value.c_str() && !*end && number>=1 && number<=1000000) {id=int(number);return true;}
    if (number>=1 && number<=1000000 && value==InstrumentLabel(int(number))) {id=int(number);return true;}
    return false;
}
bool InstrumentInput(player::TrackConfig& config,char* text,size_t size) {
    bool changed=false;float width=ImGui::CalcItemWidth();
    ImGui::SetNextItemWidth(std::max(ui::Scale(60),width-ImGui::GetFrameHeight()-ui::Scale(4)));
    if (ImGui::InputTextWithHint("##instrument","输入乐器数字 ID",text,size)) {
        int id;if (ReadInstrument(text,id) && config.instrument!=id) {config.instrument=id;config.sustained=DefaultSustained(id);}
        changed=true;
    }
    ImGui::SameLine(0,ui::Scale(4));
    bool showPresets=ImGui::BeginCombo("##presets","",ImGuiComboFlags_NoPreview|ImGuiComboFlags_HeightLarge);ui::Mark("instrument_presets");
    if (showPresets) {
        for (const auto& preset:presets) {auto label=ui::Utf8(InstrumentLabel(preset.id));
            if (ImGui::Selectable(label.c_str(),config.instrument==preset.id)) {config.instrument=preset.id;config.sustained=DefaultSustained(preset.id);SetInstrumentText(text,size,preset.id);changed=true;}
            ui::Mark(("preset_"+std::to_string(preset.id)).c_str());
        }
        ImGui::Separator();if (ImGui::Selectable("x：自定义数字")) {text[0]=0;changed=true;}ImGui::EndCombo();
    }
    return changed;
}
bool Validate(player::TrackConfig& config,const char* instrument,std::string& error) {
    int id;if (!ReadInstrument(instrument,id)) {error="请选择预设乐器，或输入 1–1000000 的乐器 ID。";return false;}
    if (config.middleDo<1 || config.middleDo>1000000 || config.transpose< -127 || config.transpose>127) {
        error="中音 Do 键应为 1–1000000；移调应为 −127–127 半音。";return false;
    }
    config.instrument=id;if (id==11) config.sustained=true;error.clear();return true;
}
std::string Pitch(int pitch) {
    if (pitch<0 || pitch>127) return "—";
    const char* names[]={"C","C# / 降Re","D","D# / 降Mi","E","F","F# / 降Sol","G","G# / 降La","A","A# / 降Si","B"};
    return std::string(names[pitch%12])+" "+std::to_string(pitch/12-1);
}
std::string MappedKey(const player::TrackConfig& config,int pitch) {
    try {return std::to_string(player::MapNoteKey(config,pitch));} catch (...) {return "超出范围";}
}
void Editor::Open(const midi::Track& track,const std::string& trackTitle,const player::TrackConfig& config) {
    draft=config;title=trackTitle;SetInstrumentText(instrument,sizeof(instrument),config.instrument);
    std::fill(counts,counts+128,0);for (const auto& note:track.notes) if (note.pitch>=0 && note.pitch<128) ++counts[note.pitch];
    error.clear();onlyUsed=false;open=request=true;SelectPitch(48);
}
void Editor::SelectPitch(int pitch) {
    source=std::max(0,std::min(127,pitch));pending=false;reveal=true;
    try {target=player::MapNoteKey(draft,source);} catch (...) {target=1;}
}
bool Editor::SetMapping() {
    if (source<0 || source>127 || target<1 || target>1000000) {error="MIDI 音高应为 0–127；目标 noteKey 应为 1–1000000。";return false;}
    draft.noteKeys[source]=target;pending=false;error.clear();return true;
}
bool Editor::Accept(player::TrackConfig& result) {
    if (!Validate(draft,instrument,error) || (pending && !SetMapping())) return false;
    result=draft;open=false;return true;
}
bool Editor::Draw(player::TrackConfig& result) {
    if (request) {ImGui::OpenPopup(editorName);request=false;}
    auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(ui::Scale(980),display.x-ui::Scale(40)),std::min(ui::Scale(750),display.y-ui::Scale(40))),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Scale(780),ui::Scale(540)),display);
    bool saved=false;
    if (ImGui::BeginPopupModal(editorName,&open,ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(title.c_str());ImGui::Separator();
        ImGui::TextUnformatted("游戏乐器 / 可直接输入 ID");ImGui::SetNextItemWidth(ui::Scale(290));
        InstrumentInput(draft,instrument,sizeof(instrument));ImGui::SameLine();SustainedInput(draft);ui::Mark("editor_long");
        if (draft.instrument==11) ui::Muted("沃雅妮莎：音符开始时立即播放长音，按 MIDI 原定结束时间松音。");
        ImGui::SetNextItemWidth(ui::Scale(120));ImGui::InputInt("中音 Do 键",&draft.middleDo,0);ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Scale(100));ImGui::InputInt("移调（半音）",&draft.transpose,0);
        ui::Muted("自定义映射按原始 MIDI 音高匹配，直接使用目标 noteKey；其余音沿用中音 Do、移调与降号转自然音规则。");
        ImGui::Checkbox("仅显示此轨使用的音高",&onlyUsed);ImGui::SameLine();ImGui::TextDisabled("自定义 %u 条",unsigned(draft.noteKeys.size()));
        std::vector<int> pitches;for (int i=0;i<128;++i) if (!onlyUsed || counts[i]) pitches.push_back(i);
        float tableHeight=std::max(ui::Scale(100),ImGui::GetContentRegionAvail().y-ui::Scale(182));
        const auto flags=ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX;
        if (ImGui::BeginTable("pitch_mapping",6,flags,ImVec2(0,tableHeight))) {
            const char* headers[]={"MIDI 音高","音名 / 八度","此轨音符数","默认 noteKey","自定义 noteKey","实际 noteKey"};
            for (auto header:headers) ImGui::TableSetupColumn(header,ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();
            auto defaults=draft;defaults.noteKeys.clear();ImGuiListClipper clipper;clipper.Begin(int(pitches.size()));
            if (reveal) for (size_t r=0;r<pitches.size();++r) if (pitches[r]==source) clipper.IncludeItemByIndex(int(r));
            while (clipper.Step()) for (int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
                int p=pitches[row];ImGui::PushID(p);ImGui::TableNextRow();ImGui::TableNextColumn();
                if (ImGui::Selectable(std::to_string(p).c_str(),p==source,ImGuiSelectableFlags_SpanAllColumns)) SelectPitch(p);
                if (reveal && p==source) {ImGui::SetScrollHereY(.5f);reveal=false;}
                ImGui::TableNextColumn();ImGui::TextUnformatted(Pitch(p).c_str());ImGui::TableNextColumn();ImGui::Text("%d",counts[p]);
                ImGui::TableNextColumn();ImGui::TextUnformatted(MappedKey(defaults,p).c_str());ImGui::TableNextColumn();
                auto custom=draft.noteKeys.find(p);
                if (custom!=draft.noteKeys.end()) ImGui::TextColored(ImVec4(.43f,.75f,1,1),"%d",custom->second);else ImGui::TextDisabled("—");
                ImGui::TableNextColumn();ImGui::TextUnformatted(MappedKey(draft,p).c_str());ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::SetNextItemWidth(ui::Scale(76));
        if (ImGui::InputInt("MIDI 音高",&source,0)) pending=true;ImGui::SameLine();ImGui::TextUnformatted("→");ImGui::SameLine();
        ImGui::SetNextItemWidth(ui::Scale(100));if (ImGui::InputInt("noteKey",&target,0)) pending=true;
        ImGui::SameLine();if (ImGui::Button("添加 / 更新")) SetMapping();
        if (ImGui::Button("恢复此音默认")) {draft.noteKeys.erase(source);SelectPitch(source);error.clear();}
        ImGui::SameLine();if (ImGui::Button("清空全部映射")) {draft.noteKeys.clear();SelectPitch(source);error.clear();}
        ImGui::SameLine();ImGui::TextDisabled("例如：48 → 60；未提交的输入会随保存写入。");
        if (!error.empty()) ImGui::TextColored(ImVec4(1,.42f,.40f,1),"%s",error.c_str());
        ImGui::Separator();
        if (ImGui::Button("保存到此轨",ImVec2(ui::Scale(142),0)) && Accept(result)) {saved=true;ImGui::CloseCurrentPopup();}
        ui::Mark("editor_save");
        ImGui::SameLine();if (ImGui::Button("取消",ImVec2(ui::Scale(84),0)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {open=false;ImGui::CloseCurrentPopup();}
        ui::Mark("editor_cancel");
        ImGui::EndPopup();
    }
    return saved;
}
void NotesViewer::Open(std::shared_ptr<const midi::Song> source,size_t index,const std::string& trackTitle,const player::TrackConfig& settings) {
    song=std::move(source);track=index;config=settings;title=trackTitle;pitch=-1;onlyCustom=false;open=focus=true;
    order.resize(song->tracks[track].notes.size());std::iota(order.begin(),order.end(),size_t(0));
    const auto& notes=song->tracks[track].notes;
    std::stable_sort(order.begin(),order.end(),[&](size_t a,size_t b) {return notes[a].startMs<notes[b].startMs;});Filter();
}
void NotesViewer::Filter() {
    filtered.clear();if (!song) return;const auto& notes=song->tracks[track].notes;
    for (auto index:order) if ((pitch<0 || notes[index].pitch==pitch) && (!onlyCustom || config.noteKeys.count(notes[index].pitch))) filtered.push_back(index);
}
std::string NotesViewer::Cell(size_t row,int column) const {
    if (!song || row>=filtered.size()) return {};const auto& note=song->tracks[track].notes[filtered[row]];
    switch (column) {
    case 0:return std::to_string(row+1);case 1:return ui::Time(note.startMs,true);case 2:return ui::Time(note.endMs,true);
    case 3:{std::ostringstream out;out<<std::fixed<<std::setprecision(3)<<(note.endMs-note.startMs)/1000;return out.str();}
    case 4:return std::to_string(note.pitch);case 5:return Pitch(note.pitch);case 6:return MappedKey(config,note.pitch);
    case 7:return config.noteKeys.count(note.pitch) ? "自定义":"默认";
    case 8:return std::to_string(note.velocity);case 9:return std::to_string(note.channel+1);
    }return {};
}
void NotesViewer::Draw() {
    if (!open) return;auto display=ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowSize(ImVec2(std::min(ui::Scale(1100),display.x-ui::Scale(40)),std::min(ui::Scale(660),display.y-ui::Scale(40))),ImGuiCond_Appearing);
    ImGui::SetNextWindowPos(ImVec2(display.x*.5f,display.y*.5f),ImGuiCond_Appearing,ImVec2(.5f,.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(ui::Scale(700),ui::Scale(360)),display);
    if (focus) {ImGui::SetNextWindowFocus();focus=false;}
    if (ImGui::Begin(viewerName,&open,ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(title.c_str());
        ImGui::TextDisabled("%u 个音符 · %s · %s · 中音 Do %d · 移调 %+d",unsigned(order.size()),ui::Utf8(InstrumentLabel(config.instrument)).c_str(),NoteMode(config),config.middleDo,config.transpose);
        ImGui::SetNextItemWidth(ui::Scale(80));bool changed=ImGui::InputInt("筛选 MIDI 音高（−1 为全部）",&pitch,0);ImGui::SameLine();
        changed=ImGui::Checkbox("仅自定义映射",&onlyCustom) || changed;
        if (changed) Filter();
        float height=std::max(ui::Scale(90),ImGui::GetContentRegionAvail().y-ui::Scale(65));
        if (ImGui::BeginTable("track_notes",10,ImGuiTableFlags_RowBg|ImGuiTableFlags_BordersInnerH|ImGuiTableFlags_Resizable|ImGuiTableFlags_ScrollY|ImGuiTableFlags_ScrollX,ImVec2(0,height))) {
            const char* headers[]={"序号","开始时间","结束时间","时长 / 秒","MIDI 音高","音名 / 八度","noteKey","映射","力度","通道"};
            const float widths[]={52,100,100,90,90,120,90,76,62,62};
            for (int i=0;i<10;++i) ImGui::TableSetupColumn(headers[i],ImGuiTableColumnFlags_WidthFixed,ui::Scale(widths[i]));
            ImGui::TableSetupScrollFreeze(0,1);ImGui::TableHeadersRow();ImGuiListClipper clipper;clipper.Begin(int(filtered.size()));
            while (clipper.Step()) for (int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
                ImGui::TableNextRow();for (int column=0;column<10;++column) {ImGui::TableNextColumn();ImGui::TextUnformatted(Cell(row,column).c_str());}
            }ImGui::EndTable();
        }
        ui::Muted(order.empty() ? "此音轨没有音符，可能是速度、控制或空音轨。":"按曲内开始时间排列，已计入速度变化与延音踏板；同一时间的多音分别列出。");
        if (ImGui::Button("关闭",ImVec2(ui::Scale(90),0))) open=false;
        ui::Mark("notes_close");
        ImGui::SameLine();ImGui::TextDisabled("显示 %u / %u",unsigned(filtered.size()),unsigned(order.size()));
    }ImGui::End();
}
}
