#include "Performance.h"
#include "InstrumentConfig.h"
#include "ImGuiHost.h"
#include <algorithm>
#include <cstdio>
namespace performance {
namespace {
const char* keys[3]={"QWERTYU","ASDFGHJ","ZXCVBNM"};
const char* degrees[7]={"do","re","mi","fa","so","la","ti"};
const char* octaves[3]={"高音 / +1 八度","中音 / 基准八度","低音 / -1 八度"};
const int offsets[7]={0,2,4,5,7,9,11};
const ImGuiKey keyboardKeys[21]={ImGuiKey_Q,ImGuiKey_W,ImGuiKey_E,ImGuiKey_R,ImGuiKey_T,ImGuiKey_Y,ImGuiKey_U,
    ImGuiKey_A,ImGuiKey_S,ImGuiKey_D,ImGuiKey_F,ImGuiKey_G,ImGuiKey_H,ImGuiKey_J,
    ImGuiKey_Z,ImGuiKey_X,ImGuiKey_C,ImGuiKey_V,ImGuiKey_B,ImGuiKey_N,ImGuiKey_M};
bool Active(player::State state) {return state==player::State::Playing || state==player::State::Countdown || state==player::State::Paused;}
void CenterText(ImDrawList* draw,const ImVec2& center,const char* text,ImU32 color) {auto size=ImGui::CalcTextSize(text);draw->AddText(ImVec2(center.x-size.x*.5f,center.y-size.y*.5f),color,text);}
}
void View::Load(const std::wstring& path) {
    ini=path;config.instrument=GetPrivateProfileIntW(L"performance",L"instrument",1,ini.c_str());
    if (config.instrument<1 || config.instrument>1000000) config.instrument=1;
    config.sustained=player::UsesSustained(config.instrument,GetPrivateProfileIntW(L"performance",L"sustained",instrumentui::DefaultSustained(config.instrument),ini.c_str())!=0);
    config.middleDo=int(GetPrivateProfileIntW(L"performance",L"middleDo",player::DefaultMiddleDo,ini.c_str()));
    config.transpose=int(GetPrivateProfileIntW(L"performance",L"transpose",0,ini.c_str()));
    if (config.middleDo<1 || config.middleDo>1000000) config.middleDo=player::DefaultMiddleDo;
    if (config.transpose< -127 || config.transpose>127) config.transpose=0;
    instrumentui::SetInstrumentText(instrument,sizeof(instrument),config.instrument);
}
int View::Key(int slot) const {
    try {return player::MapNoteKey(config,60+(1-slot/7)*12+offsets[slot%7]);} catch (...) {return 0;}
}
bool View::Save() {
    const wchar_t* names[]={L"instrument",L"sustained",L"middleDo",L"transpose"};
    const int values[]={config.instrument,int(config.sustained),config.middleDo,config.transpose};
    for (int i=0;i<4;++i) {auto value=std::to_wstring(values[i]);if (!WritePrivateProfileStringW(L"performance",names[i],value.c_str(),ini.c_str())) {error="无法保存手动演奏设置，请检查配置目录写入权限。";return false;}}return true;
}
void View::Release(bool force) {
    bool any=false;for (bool down:held) any=any || down;
    if (any || force) {app::Command command;command.action=app::Action::ManualRelease;app::Send(command);}
    held.fill(false);keyboard.fill(false);mouseSlot=-1;
}
void View::Draw(const app::Status& status,float height,bool focused) {
    const auto scale=[](float v) {return ui::Scale(v);};
    const bool playing=Active(status.transport.state);bool changed=false;
    ImGui::BeginChild("performance_workspace",ImVec2(0,height),ImGuiChildFlags_Borders);
    if (ImGui::BeginTable("performance_layout",2,ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("instrument",ImGuiTableColumnFlags_WidthFixed,scale(264));
        ImGui::TableSetupColumn("keyboard",ImGuiTableColumnFlags_WidthStretch);ImGui::TableNextColumn();
        ui::Heading("手动演奏");ui::Muted("选择乐器后，按键或按住鼠标弹奏。");
        ImGui::Separator();ImGui::BeginDisabled(playing);
        ImGui::TextUnformatted("游戏乐器 / 可直接输入 ID");ImGui::SetNextItemWidth(-scale(8));
        changed=instrumentui::InstrumentInput(config,instrument,sizeof(instrument)) || changed;
        changed=instrumentui::SustainedInput(config) || changed;
        ImGui::SetNextItemWidth(scale(90));changed=ImGui::InputInt("中音 Do 键",&config.middleDo,0) || changed;
        ImGui::SetNextItemWidth(scale(90));changed=ImGui::InputInt("移调 / 半音",&config.transpose,0) || changed;
        if (ImGui::Button("填入刚弹的中音 Do")) {
            if (!status.game.lastInstrument || !status.game.lastNoteKey) error="先在游戏里手动弹一下中音 Do，再点击填入。";
            else {config.instrument=status.game.lastInstrument;config.middleDo=status.game.lastNoteKey;config.sustained=instrumentui::DefaultSustained(config.instrument);instrumentui::SetInstrumentText(instrument,sizeof(instrument),config.instrument);changed=true;}
        }ImGui::EndDisabled();
        ImGui::Separator();if (ImGui::Checkbox("启用键盘弹奏",&keyboardEnabled)) Release();
        if (ImGui::Button("全部松音 / Esc",ImVec2(-scale(8),scale(34)))) Release(true);
        ImGui::TextDisabled("%s · 长音 %u 个",instrumentui::NoteMode(config),unsigned(status.manualVoices));
        ImGui::PushTextWrapPos(0);
        ui::Muted("上排 Q W E R T Y U\n中排 A S D F G H J\n下排 Z X C V B N M");
        ui::Muted("每排依次为 do re mi fa so la ti。\n长音按住持续，松开停止；支持和弦。\n短音每次按下触发一次。");
        if (config.instrument==11) ui::Muted("沃雅妮莎：按下立即播放长音；按住持续，松键停止。");
        ui::Muted("键盘仅在此面板获得焦点时生效。编辑输入框时暂停接收音符；切页、隐藏或失焦会全部松音。");
        ImGui::PopTextWrapPos();
        auto validated=config;std::string validation;const bool valid=instrumentui::Validate(validated,instrument,validation);
        if (changed) {Release();if (valid) {config=validated;error.clear();Save();}}
        const bool ready=status.preview || (status.game.ready && status.game.threadId && (!player::UsesShort(config.instrument,config.sustained) || status.game.shortReady));
        const bool available=valid && ready && !playing;
        ImGui::TableNextColumn();ui::Heading("演奏键盘 / 三个八度");
        if (playing) ImGui::TextDisabled("自动演奏仍在运行，请先停止。");
        else if (!ready) ImGui::TextWrapped("请先进入游戏乐器界面并手动弹一个音；使用短音乐器时先弹一个短音完成校准。");
        else ImGui::TextDisabled(status.preview ? "预览模式：显示按键状态，不产生游戏声音。":"已就绪 · 点击面板后使用键盘弹奏，可同时按多个键。");
        if (!valid) ImGui::TextColored(ImVec4(1,.48f,.44f,1),"%s",validation.c_str());
        if (!error.empty()) ImGui::TextWrapped("%s",error.c_str());
        if (!status.manualError.empty()) ImGui::TextColored(ImVec4(1,.48f,.44f,1),"%s",status.manualError.c_str());
        auto& io=ImGui::GetIO();
        const bool inputAllowed=available && focused && !io.AppFocusLost && !ImGui::IsPopupOpen(nullptr,ImGuiPopupFlags_AnyPopupId);
        const bool keyboardAllowed=inputAllowed && keyboardEnabled && !io.WantTextInput && !io.KeyCtrl && !io.KeyAlt && !io.KeySuper;
        bool cancel=inputAllowed && !io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Escape,false);
        if (!inputAllowed || cancel) Release(cancel);
        if (!io.MouseDown[0]) mouseSlot=-1;
        for (int i=0;i<21;++i) {
            if (!keyboardAllowed || cancel || !ImGui::IsKeyDown(keyboardKeys[i])) keyboard[i]=false;
            else if (ImGui::IsKeyPressed(keyboardKeys[i],false)) keyboard[i]=true;
        }
        ImGui::BeginChild("performance_pads",ImVec2(0,0));
        float gap=scale(10),width=std::max(scale(52),(ImGui::GetContentRegionAvail().x-gap*6)/7);
        float padHeight=std::max(scale(82),std::min(scale(122),(ImGui::GetContentRegionAvail().y-scale(96))/3));
        for (int row=0;row<3;++row) {
            ImGui::TextDisabled("%s",octaves[row]);
            for (int col=0;col<7;++col) {
                int slot=row*7+col,key=Key(slot);if (col) ImGui::SameLine(0,gap);
                ImGui::PushID(slot);ImGui::BeginDisabled(!inputAllowed || !key || cancel);
                ImGui::InvisibleButton("pad",ImVec2(width,padHeight));
                if (ImGui::IsItemActivated() && ImGui::IsMouseDown(0)) mouseSlot=slot;
                bool hovered=ImGui::IsItemHovered(),down=keyboard[slot] || mouseSlot==slot;
                auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();auto draw=ImGui::GetWindowDrawList();
                const bool enabled=inputAllowed && key;
                ImU32 fill=!enabled ? IM_COL32(31,36,44,255):(down ? IM_COL32(44,126,123,255):(hovered ? IM_COL32(42,65,74,255):IM_COL32(34,45,57,255)));
                draw->AddRectFilled(a,b,fill,scale(8));draw->AddRect(a,b,down ? IM_COL32(121,237,215,255):IM_COL32(69,87,103,255),scale(8),0,scale(down ? 2.f:1.f));
                char badge[2]={keys[row][col],0};
                CenterText(draw,ImVec2((a.x+b.x)*.5f,a.y+padHeight*.24f),badge,enabled ? IM_COL32(240,245,247,255):IM_COL32(110,119,128,255));
                CenterText(draw,ImVec2((a.x+b.x)*.5f,a.y+padHeight*.52f),degrees[col],down ? IM_COL32(255,255,255,255):IM_COL32(119,204,194,255));
                char label[32];if (key) sprintf_s(label,"%d",key);else sprintf_s(label,"--");
                CenterText(draw,ImVec2((a.x+b.x)*.5f,a.y+padHeight*.79f),label,IM_COL32(158,178,191,255));
                if (hovered) ImGui::SetTooltip("%c · %s\n%s\nnoteKey %d\n按住长音，松开停止",keys[row][col],degrees[col],octaves[row],key);
                ImGui::EndDisabled();ImGui::PopID();
            }ImGui::Dummy(ImVec2(0,scale(5)));
        }ImGui::EndChild();ImGui::EndTable();
        for (int i=0;i<21;++i) {
            bool down=inputAllowed && !cancel && Key(i) && (keyboard[i] || mouseSlot==i);
            if (down==held[i]) continue;app::Command command;command.action=down ? app::Action::ManualOn:app::Action::ManualOff;
            command.slot=i;command.instrument=config.instrument;command.key=Key(i);command.sustained=config.sustained;
            if (!app::Send(command)) {error="命令队列已满，已松开全部手动音符。";Release(true);break;}held[i]=down;
        }
    }ImGui::EndChild();
}
}
