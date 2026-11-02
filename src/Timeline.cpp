#include "Timeline.h"
#include "ImGuiHost.h"
#include "InstrumentConfig.h"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace timeline {
double Viewport::Span() const {return std::max(1.,duration)/std::max(1.,zoom);}
double Viewport::Time(double fraction) const {return std::max(0.,std::min(duration,start+fraction*Span()));}
double Viewport::Fraction(double ms) const {return (ms-start)/Span();}
void Viewport::Clamp() {duration=std::max(1.,duration);zoom=std::max(1.,std::min(64.,zoom));start=std::max(0.,std::min(duration-Span(),start));}
void Viewport::Zoom(double value,double anchor) {double time=start+anchor*Span();zoom=value;Clamp();start=time-anchor*Span();Clamp();}
void Viewport::Follow(double ms) {if (ms<start || ms>start+Span()*.9) {start=ms-Span()*.15;Clamp();}}
namespace {
float S(float x) {return ui::Scale(x);}
ImU32 TrackColor(int track,bool enabled,int alpha=255) {
    const ImU32 colors[]={IM_COL32(97,186,224,255),IM_COL32(136,198,127,255),IM_COL32(211,171,100,255),IM_COL32(172,148,222,255),IM_COL32(219,135,153,255),IM_COL32(100,198,183,255)};
    ImU32 c=enabled ? colors[track%6]:IM_COL32(100,110,126,255);return (c&0x00ffffff)|(ImU32(alpha)<<24);
}
double GridStep(double span,float width) {
    double target=span*double(S(85))/std::max(1.f,width)/1000.;
    double unit=std::pow(10.,std::floor(std::log10(std::max(.001,target))));
    return (target/unit<=1 ? 1:target/unit<=2 ? 2:target/unit<=5 ? 5:10)*unit*1000.;
}
void Grid(ImDrawList* draw,float left,float right,float top,float bottom,const Viewport& view,double step,bool labels) {
    draw->PushClipRect(ImVec2(left,top),ImVec2(right,bottom),true);
    for (double ms=std::ceil(view.start/step)*step;ms<=view.start+view.Span()+.01;ms+=step) {
        float x=left+float(view.Fraction(ms))*(right-left);
        draw->AddLine(ImVec2(x,top),ImVec2(x,bottom),IM_COL32(55,65,80,140));
        if (labels) {auto text=ui::Time(ms,step<1000);draw->AddText(ImVec2(x+S(5),top+S(4)),IM_COL32(158,171,191,255),text.c_str());}
    }draw->PopClipRect();
}
void Playhead(ImDrawList* draw,float left,float right,float top,float bottom,const Viewport& view,double position,bool head) {
    double fraction=view.Fraction(position);if (fraction<0 || fraction>1) return;
    float x=left+float(fraction)*(right-left);const ImU32 color=IM_COL32(255,202,94,255);
    draw->AddLine(ImVec2(x,top),ImVec2(x,bottom),color,S(2));
    if (head) draw->AddTriangleFilled(ImVec2(x-S(6),top),ImVec2(x+S(6),top),ImVec2(x,top+S(8)),color);
}
}
void View::Open(const midi::Song& song) {
    viewport=Viewport{};viewport.duration=std::max(1.,song.durationMs);follow=true;dragging_=false;reveal_=-1;
    tracks_.assign(song.tracks.size(),Track{});
    for (size_t i=0;i<song.tracks.size();++i) {
        auto& track=tracks_[i];const auto& notes=song.tracks[i].notes;
        track.order.resize(notes.size());std::iota(track.order.begin(),track.order.end(),size_t(0));
        std::stable_sort(track.order.begin(),track.order.end(),[&](size_t a,size_t b) {return notes[a].startMs<notes[b].startMs;});
        double end=0;track.low=127;track.high=0;
        for (size_t n:track.order) {end=std::max(end,notes[n].endMs);track.maxEnd.push_back(end);track.low=std::min(track.low,notes[n].pitch);track.high=std::max(track.high,notes[n].pitch);}
    }
}
Interaction View::Draw(const midi::Song& song,const std::vector<player::TrackConfig>& configs,
    const std::vector<std::string>& names,const std::vector<int>& visible,int selected,
    const player::Snapshot& status,bool locked,bool canSeek) {
    Interaction result;viewport.Clamp();const auto& io=ImGui::GetIO();
    bool playing=status.state==player::State::Playing;
    double position=dragging_ ? dragTime_:status.positionMs;
    if (ImGui::Button("全曲")) {viewport.zoom=1;viewport.start=0;}ui::Mark("timeline_fit");ImGui::SameLine();
    ImGui::SetNextItemWidth(S(125));float zoom=float(viewport.zoom);
    if (ImGui::SliderFloat("##timeline_zoom",&zoom,1,64,"%.1fx",ImGuiSliderFlags_Logarithmic)) viewport.Zoom(zoom);
    ui::Mark("timeline_zoom");ImGui::SameLine();ImGui::Checkbox("跟随播放",&follow);ui::Mark("timeline_follow");
    ImGui::SameLine();ImGui::SetNextItemWidth(S(80));ImGui::SliderFloat("轨高",&laneHeight_,52,100,"%.0f");
    if (playing && follow && !dragging_) viewport.Follow(position);

    const float labelWidth=S(196),rulerHeight=S(30),laneHeight=S(laneHeight_);
    float width=std::max(S(250),ImGui::GetContentRegionAvail().x-ImGui::GetStyle().ScrollbarSize);
    float gridWidth=std::max(S(45),width-labelWidth);
    ImVec2 ruler=ImGui::GetCursorScreenPos();auto draw=ImGui::GetWindowDrawList();
    float left=ruler.x+labelWidth,right=left+gridWidth;
    double step=GridStep(viewport.Span(),gridWidth);
    draw->AddRectFilled(ruler,ImVec2(right,ruler.y+rulerHeight),IM_COL32(28,35,46,255));
    draw->AddText(ImVec2(ruler.x+S(8),ruler.y+S(5)),IM_COL32(161,175,195,255),"音轨 / 游戏乐器");
    Grid(draw,left,right,ruler.y,ruler.y+rulerHeight,viewport,step,true);
    ImGui::SetCursorScreenPos(ImVec2(left,ruler.y));ImGui::BeginDisabled(!canSeek);
    ImGui::InvisibleButton("timeline_ruler",ImVec2(gridWidth,rulerHeight));ui::Mark("timeline_ruler");
    if (canSeek && ImGui::IsItemActivated()) {dragging_=true;dragTime_=viewport.Time((io.MousePos.x-left)/gridWidth);}
    if (dragging_) {
        dragTime_=viewport.Time((io.MousePos.x-left)/gridWidth);position=dragTime_;
        if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {result.seek=true;result.position=dragTime_;dragging_=false;}
    }
    if (ImGui::IsItemHovered() || dragging_) {ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);ImGui::SetTooltip("跳转到 %s",ui::Time(viewport.Time((io.MousePos.x-left)/gridWidth),true).c_str());}
    ImGui::EndDisabled();Playhead(draw,left,right,ruler.y,ruler.y+rulerHeight,viewport,position,true);
    ImGui::SetCursorScreenPos(ImVec2(ruler.x,ruler.y+rulerHeight));
    float remaining=ImGui::GetContentRegionAvail().y-S(66);
    // This child owns only vertical scrolling; labels and notes always share exactly the same rows.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,ImVec2(0,0));ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,ImVec2(0,0));
    ImGui::BeginChild("timeline_lanes",ImVec2(0,std::max(S(55),remaining)),ImGuiChildFlags_Borders,ImGuiWindowFlags_AlwaysVerticalScrollbar|((io.KeyCtrl || io.KeyShift) ? ImGuiWindowFlags_NoScrollWithMouse:0));
    draw=ImGui::GetWindowDrawList();
    ImGuiListClipper clipper;clipper.Begin(int(visible.size()),laneHeight);
    if (reveal_>=0) for (size_t n=0;n<visible.size();++n) if (visible[n]==reveal_) clipper.IncludeItemByIndex(int(n));
    while (clipper.Step()) for (int row=clipper.DisplayStart;row<clipper.DisplayEnd;++row) {
        int i=visible[row];const auto& config=configs[i];const auto& track=tracks_[i];const auto& notes=song.tracks[i].notes;
        ImGui::PushID(i);ImVec2 origin=ImGui::GetCursorScreenPos();float top=origin.y,bottom=top+laneHeight;
        left=ruler.x+labelWidth;right=left+gridWidth;
        draw->AddRectFilled(origin,ImVec2(right,bottom),selected==i ? IM_COL32(31,49,65,255):(row%2 ? IM_COL32(23,29,39,255):IM_COL32(26,33,43,255)));
        draw->AddRectFilled(ImVec2(origin.x,top),ImVec2(origin.x+S(3),bottom),TrackColor(i,config.enabled));
        draw->AddLine(ImVec2(origin.x,bottom-S(1)),ImVec2(right,bottom-S(1)),IM_COL32(48,58,71,255));
        Grid(draw,left,right,top,bottom,viewport,step,false);
        draw->PushClipRect(ImVec2(left+1,top+S(5)),ImVec2(right,bottom-S(5)),true);
        const midi::Note* hovered=nullptr;
        size_t first=size_t(std::lower_bound(track.maxEnd.begin(),track.maxEnd.end(),viewport.start)-track.maxEnd.begin());
        for (size_t n=first;n<track.order.size();++n) {
            const auto& note=notes[track.order[n]];if (note.startMs>viewport.start+viewport.Span()) break;if (note.endMs<viewport.start) continue;
            float x1=left+float(viewport.Fraction(note.startMs))*gridWidth,x2=left+float(viewport.Fraction(note.endMs))*gridWidth;
            x1=std::max(left,x1);x2=std::min(right,std::max(x1+S(2),x2));
            float range=float(std::max(12,track.high-track.low+4));
            float y=top+S(10)+float(track.high+2-note.pitch)/range*(laneHeight-S(24));
            bool active=playing && config.enabled && note.startMs<=status.positionMs && note.endMs>status.positionMs;
            draw->AddRectFilled(ImVec2(x1,y),ImVec2(x2,y+S(4)),active ? IM_COL32(255,223,147,255):TrackColor(i,config.enabled,config.enabled ? 220:110),S(1));
            if (io.MousePos.x>=x1 && io.MousePos.x<=std::max(x1+S(4),x2) && io.MousePos.y>=y-S(3) && io.MousePos.y<=y+S(7)) hovered=&note;
        }draw->PopClipRect();
        if (notes.empty()) draw->AddText(ImVec2(left+S(12),top+S(27)),IM_COL32(89,101,120,255),"速度 / 控制 / 空音轨");
        Playhead(draw,left,right,top,bottom,viewport,position,false);
        ImGui::SetCursorScreenPos(ImVec2(origin.x+S(10),top+S(12)));bool enabled=config.enabled;ImGui::BeginDisabled(locked);
        if (ImGui::Checkbox("##lane_enabled",&enabled)) result.toggle=i;ImGui::EndDisabled();
        ImGui::SetCursorScreenPos(ImVec2(origin.x+S(38),top));
        if (ImGui::InvisibleButton("lane_label",ImVec2(labelWidth-S(38),laneHeight))) result.selected=i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {result.selected=i;result.notes=i;}
        if (i==selected) ui::Mark("timeline_selected_lane");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s · %s\n%u 个音符 · %u 条映射\n双击查看音符",names[i].c_str(),ui::Utf8(instrumentui::InstrumentLabel(config.instrument)).c_str(),instrumentui::NoteMode(config),unsigned(notes.size()),unsigned(config.noteKeys.size()));
        draw->PushClipRect(ImVec2(origin.x+S(38),top),ImVec2(left-S(5),bottom),true);
        std::string title=std::to_string(i+1)+"  "+names[i];
        draw->AddText(ImVec2(origin.x+S(39),top+S(3)),IM_COL32(216,226,238,255),title.c_str());
        auto instrument=ui::Utf8(instrumentui::InstrumentLabel(config.instrument));
        draw->AddText(ImVec2(origin.x+S(39),top+S(22)),TrackColor(i,config.enabled),instrument.c_str());
        auto detail=std::string(instrumentui::NoteMode(config))+" · 映射 "+std::to_string(config.noteKeys.size());
        draw->AddText(ImGui::GetFont(),ImGui::GetFontSize()*.82f,ImVec2(origin.x+S(39),top+S(42)),IM_COL32(129,146,168,255),detail.c_str());draw->PopClipRect();
        ImGui::SetCursorScreenPos(ImVec2(left,top));
        if (ImGui::InvisibleButton("lane_notes",ImVec2(gridWidth,laneHeight))) result.selected=i;
        if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(0)) {result.selected=i;result.notes=i;}
        if (hovered && ImGui::IsItemHovered()) {
            std::string key;try {key=std::to_string(player::MapNoteKey(config,hovered->pitch));}catch (const std::exception&) {key="超出有效范围";}
            ImGui::SetTooltip("%s · MIDI %d → noteKey %s\n%s — %s · 力度 %d",instrumentui::Pitch(hovered->pitch).c_str(),hovered->pitch,key.c_str(),ui::Time(hovered->startMs,true).c_str(),ui::Time(hovered->endMs,true).c_str(),hovered->velocity);
        }
        if (reveal_==i) {ImGui::SetScrollHereY(.3f);reveal_=-1;}
        ImGui::SetCursorScreenPos(ImVec2(origin.x,bottom));ImGui::PopID();
    }
    bool wheel=ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows) && io.MouseWheel!=0;
    if (wheel && io.KeyCtrl) viewport.Zoom(viewport.zoom*std::pow(1.25,io.MouseWheel),std::max(0.,std::min(1.,double((io.MousePos.x-left)/gridWidth))));
    else if (wheel && io.KeyShift) {viewport.start-=io.MouseWheel*viewport.Span()*.1;viewport.Clamp();follow=false;}
    ImGui::EndChild();ImGui::PopStyleVar(2);
    float pan=float(viewport.start/1000.),maxPan=float((viewport.duration-viewport.Span())/1000.);
    ImGui::BeginDisabled(maxPan<=0);ImGui::SetNextItemWidth(-1);
    if (ImGui::SliderFloat("##timeline_pan",&pan,0,std::max(.001f,maxPan),"视窗起点 %.2f 秒")) {viewport.start=pan*1000.;viewport.Clamp();follow=false;}
    ui::Mark("timeline_pan");ImGui::EndDisabled();
    ImGui::TextDisabled("点 / 拖动上方刻度跳转 · Ctrl+滚轮缩放 · Shift+滚轮平移");
    return result;
}
}
