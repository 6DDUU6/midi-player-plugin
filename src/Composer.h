#pragma once
#include "Composition.h"
#include "App.h"
#include <array>

namespace composer {
class View {
public:
    composition::Project project;
    void Load(const std::wstring& path);
    void Draw(const app::Status& status,HWND owner,float height,bool focused);
    void Release();
    void LoseFocus();
    void Tick();
    void Demo(bool record=false);
    void TogglePlayback(const app::Status& status);
    void ResetPlayback();
    int SelectedTrack() const {return selected_;}
    float Zoom() const {return zoom_;}
    double PositionBeat() const {return positionBeat_;}
    size_t SelectionCount() const {return selection_.size();}
    bool Recording() const {return recording_;}
    int NewNoteLength() const {return newLength_;}
    bool OpenMidi(const std::wstring& path);
    bool SaveProject(const std::wstring& path);
    bool OpenProject(const std::wstring& path);
private:
    int selected_=0,snap_=2,newLength_=composition::PPQ,velocity_=100;
    int inspectorRequest_=0;
    int inspectorTab_=0,layoutRevision_=0,restoreScrollFrames_=0;
    bool moreSettings_=false;
    float sidebarWidth_=245,drawnSidebarWidth_=0,trackScroll_=0;
    std::array<double,21> auditionEnds_{};
    unsigned note_=0;
    std::set<unsigned> selection_;
    float zoom_=2,pan_=0;
    char name_[256]{},instrument_[96]{};
    bool dragging_=false,resizing_=false;
    bool dragRemembered_=false,deselectOnRelease_=false;
    int dragFirstTick_=0,dragEndTick_=0,dragLowRow_=0,dragHighRow_=0,dragTick_=0,dragRow_=0;
    bool seeking_=false,transportLoaded_=false;
    bool panning_=false,scrolling_=false;
    float panMouseX_=0,panAnchor_=0,scrollOffset_=0;
    double positionBeat_=0;
    struct RecordVoice {unsigned note;int track;RecordVoice(unsigned n=0,int t=0):note(n),track(t) {}};
    std::array<RecordVoice,21> recordVoices_{};
    std::map<uint32_t,RecordVoice> gameVoices_;
    std::array<bool,21> recordHeld_{};
    bool recording_=false,recordRemembered_=false;
    bool receiveGame_=true;
    int recordMouseRow_=-1;
    size_t recordedNotes_=0;
    size_t skippedGameNotes_=0;
    double recordClock_=0,recordStartMs_=0;
    composition::Note original_;
    float dragX_=0,dragY_=0;
    std::wstring draftPath_;
    std::string error_,notice_;
    struct History {
        composition::Project project;composition::EditorSettings editor;bool restoreEditor;
        History(const composition::Project& p,composition::EditorSettings e,bool restore):project(p),editor(std::move(e)),restoreEditor(restore) {}
    };
    std::vector<History> undo_,redo_;
    struct ClipboardTrack {int offset;composition::Track track;};
    std::vector<ClipboardTrack> clipboard_;
    void Select(int track,bool clear=true);
    void FocusSelection(unsigned preferred=0);
    void PruneSelection();
    void BeginDrag(bool resize,bool remembered=false);
    composition::Note* Note();
    composition::EditorSettings Settings() const;
    void Restore(composition::Project project,const composition::EditorSettings& editor);
    void Remember(bool restoreEditor=false);
    void Save();
    void Undo(bool redo);
    void Remove();
    void Copy(bool cut);
    void Paste();
    void Export(HWND owner);
    void Import(HWND owner);
    void SaveProjectDialog(HWND owner);
    void ImportProject(HWND owner);
    void Play(const app::Status& status);
    void Seek(double beat,const app::Status& status);
    void StartRecording();
    void StopRecording();
    void UpdateRecording();
    void ReleaseRecordingVoices();
    void RecordInput(const app::Status& status,bool input);
    void KeyboardInput(const app::Status& status,bool input);
    void RecordGameInput();
    unsigned AddRecordedNote(int row,int start,int length);
    void ExtendRecordedNote(const RecordVoice& voice,int endTick);
    void Audition(const app::Status& status,int row,bool chord=false);
    void StopAudition();
    void Sidebar(bool locked);
    void Grid(const app::Status& status,bool focused,bool locked);
    void Scrollbar(bool input,float width,float keyWidth);
};
}
