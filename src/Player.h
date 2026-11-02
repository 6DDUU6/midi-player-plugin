#pragma once
#include "Midi.h"
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace player {
constexpr int DefaultMiddleDo = 48;
struct TrackConfig {
    bool enabled = true, sustained = false;
    int instrument = 1, middleDo = DefaultMiddleDo, transpose = 0;
    std::map<int,int> noteKeys; // Original MIDI pitch -> final game noteKey.
};
struct Event {
    double ms=0; uint32_t token=0;
    int instrument=0, noteKey=0, track=0, midiPitch=-1;
    bool sustained=false, on=false;
};
struct Span { Event start; double endMs; Span(Event e,double end):start(e),endMs(end) {} };
struct Plan {
    std::vector<Event> events; std::vector<Span> spans;
    double durationMs=0, speed=1;
};
std::shared_ptr<const Plan> Build(const midi::Song& song,const std::vector<TrackConfig>& configs,double speed);
int MapNoteKey(const TrackConfig& config,int midiPitch);
bool UsesSustained(int instrument,bool sustained);
bool UsesShort(int instrument,bool sustained);
enum class StartResult { Started, Rejected, Error };
class Backend {
public:
    virtual ~Backend() {}
    virtual bool Short(int instrument,int key,std::string& error)=0;
    virtual StartResult Start(int instrument,int key,uint32_t& handle,std::string& error)=0;
    virtual bool Stop(uint32_t handle,std::string& error)=0;
};
enum class State { Empty, Stopped, Countdown, Playing, Paused, Finished, Error };
struct Snapshot {
    State state=State::Empty; double positionMs=0,durationMs=0,countdownMs=0;
    size_t activeVoices=0,playedNotes=0,skippedNotes=0; std::string error,warning;
};
class Transport {
public:
    explicit Transport(Backend& b):backend_(b) {}
    void Load(std::shared_ptr<const Plan> plan);
    void Play(double nowMs,double countdownMs);
    void Pause(double nowMs);
    void Stop();
    void Seek(double positionMs,double nowMs);
    void Tick(double nowMs);
    Snapshot Status() const;
private:
    struct Voice { uint32_t token,handle; };
    Backend& backend_; std::shared_ptr<const Plan> plan_;
    std::vector<Voice> voices_;
    State state_=State::Empty;
    double position_=0,anchor_=0,startAt_=0,lastTick_=0;
    size_t cursor_=0,played_=0,skipped_=0; bool restore_=false;
    std::string error_,warning_;
    bool Cleanup();
    bool Emit(const Event& e);
    void Fail(const std::string& error);
    void SetCursor();
};
}
