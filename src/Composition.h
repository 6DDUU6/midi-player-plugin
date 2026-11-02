#pragma once
#include "Midi.h"
#include <map>

namespace composition {
constexpr int PPQ=960,MaxTracks=128,MaxBeats=4096;
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
struct ImportStats {size_t imported=0,converted=0,skipped=0,merged=0;};
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
    size_t NoteCount() const;
    double BeatDuration(int beat) const;
    double BaseTempo(int tick) const;
    std::map<int,double> Tempos() const;
    double Time(double beat) const;
    double BeatAt(double ms) const;
    void Validate() const;
    std::vector<uint8_t> Midi() const;
    std::vector<uint8_t> Draft() const;
    static Project ReadDraft(const std::vector<uint8_t>& bytes);
    static Project Import(const midi::Song& song,ImportStats& stats);
};
}
