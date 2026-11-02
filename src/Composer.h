#pragma once
#include "Composition.h"
#include "App.h"

namespace composer {
class View {
public:
    composition::Project project;
    void Load(const std::wstring& path);
    void Draw(const app::Status& status,HWND owner,float height,bool focused);
    void Release();
    void Demo();
    void TogglePlayback(const app::Status& status);
    int SelectedTrack() const {return selected_;}
    float Zoom() const {return zoom_;}
    bool OpenMidi(const std::wstring& path);
private:
    int selected_=0,snap_=2,length_=4,velocity_=100,beat_=0,audition_=-1;
    int inspectorRequest_=0;
    double auditionEnd_=0;
    unsigned note_=0;
    float zoom_=2,pan_=0;
    char name_[256]{},instrument_[96]{};
    bool dragging_=false,resizing_=false;
    composition::Note original_;
    float dragX_=0,dragY_=0;
    std::wstring draftPath_;
    std::string error_,notice_;
    std::vector<composition::Project> undo_,redo_;
    void Select(int track);
    composition::Note* Note();
    void Remember();
    void Save();
    void Undo(bool redo);
    void Remove();
    void Export(HWND owner);
    void Import(HWND owner);
    void Play(const app::Status& status);
    void Audition(const app::Status& status,int row);
    void StopAudition();
    void Sidebar(bool locked);
    void Grid(const app::Status& status,bool focused,bool locked);
};
}
