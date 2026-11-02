#pragma once
#include "Player.h"

namespace timeline {
// Times stay in MIDI song time, including tempo changes, independent of playback speed.
struct Viewport {
    double duration=1,start=0,zoom=1;
    double Span() const;
    double Time(double fraction) const;
    double Fraction(double ms) const;
    void Clamp();
    void Zoom(double value,double anchor=.5);
    void Follow(double ms);
};
struct Interaction {
    int selected=-1,toggle=-1,notes=-1;
    bool seek=false;
    double position=0;
};
class View {
public:
    Viewport viewport;
    bool follow=true;
    void Open(const midi::Song& song);
    void Reveal(int track) { reveal_=track; }
    Interaction Draw(const midi::Song& song,const std::vector<player::TrackConfig>& configs,
        const std::vector<std::string>& names,const std::vector<int>& visible,int selected,
        const player::Snapshot& status,bool locked,bool canSeek);
private:
    struct Track {
        std::vector<size_t> order;
        std::vector<double> maxEnd;
        int low=60,high=60;
    };
    std::vector<Track> tracks_;
    int reveal_=-1;
    float laneHeight_=64;
    bool dragging_=false;
    double dragTime_=0;
};
}
