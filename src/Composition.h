#pragma once
#include "Midi.h"
#include <map>
#include <set>

namespace composition {
constexpr int PPQ=960,MaxTracks=128,MaxBeats=4096;
constexpr float MaxZoom=100.f;
constexpr int MinNoteLength=PPQ/16;
bool IsNoteLength(int ticks);
// Round to the nearest 1/16 beat that fits; ties round up. Zero means no duration fits.
int QuantizeLength(double ticks,int available=MaxBeats*PPQ);
struct Note {
    unsigned id=0;
    int row=14,start=0,length=PPQ,velocity=100;
};
struct Track {
    unsigned id=0;
    std::string name;
    int instrument=1,middleDo=48;
    bool sustained=false,visible=true;
    float color[3]{.35f,.7f,1.f};
    std::vector<Note> notes;
};
struct ImportStats {size_t imported=0,converted=0,skipped=0,merged=0,quantized=0;};
struct LengthStats {size_t adjusted=0,merged=0;};
struct EditorSettings {
    int selectedTrack=0,snap=2,newLength=PPQ,velocity=100,inspectorTab=0;
    float zoom=2,pan=0,sidebarWidth=245,trackScroll=0;
    double positionBeat=0;
    bool receiveGame=true,moreSettings=false;
    unsigned activeNote=0;
    std::set<unsigned> selection;
};
struct Project {
    int beats=32;
    double beatMs=500;
    std::map<int,double> beatTimes;
    std::map<int,double> tempoChanges;
    unsigned nextId=1;
    std::vector<Track> tracks;
    Project();
    int AddTrack();
    unsigned AddNote(int track,int row,int start,int length,int velocity=100);
    bool Place(int track,const Note& note) const;
    bool MoveNotes(const std::set<unsigned>& ids,int tickDelta,int rowDelta);
    size_t NoteCount() const;
    double BeatDuration(int beat) const;
    double BaseTempo(int tick) const;
    std::map<int,double> Tempos() const;
    double Time(double beat) const;
    double BeatAt(double ms) const;
    void Validate() const;
    LengthStats QuantizeLengths(EditorSettings* editor=nullptr);
    std::vector<uint8_t> Midi() const;
    // An editor snapshot writes V3 with exact values; omitting it retains the V2 data-only format.
    std::vector<uint8_t> Draft(const EditorSettings* editor=nullptr) const;
    static Project ReadDraft(const std::vector<uint8_t>& bytes,EditorSettings* editor=nullptr);
    static Project Import(const midi::Song& song,ImportStats& stats);
};
}
