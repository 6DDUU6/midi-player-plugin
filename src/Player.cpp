#include "Player.h"
#include <algorithm>
#include <cmath>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace player {
namespace {
std::string NoteContext(const Event& e) {
    std::ostringstream out;
    if (e.track>0) out<<"音轨 "<<e.track<<" · ";
    out<<std::fixed<<std::setprecision(3)<<e.ms/1000<<" 秒 · 乐器id="<<e.instrument<<" noteKey="<<e.noteKey;
    if (e.midiPitch>=0) out<<" MIDI="<<e.midiPitch;
    return out.str();
}
}
bool UsesSustained(int instrument,bool sustained) {return instrument==11 || sustained;}
bool UsesShort(int instrument,bool sustained) {return !UsesSustained(instrument,sustained);}
int MapNoteKey(const TrackConfig& c,int midiPitch) {
    if (midiPitch<0 || midiPitch>127) throw std::runtime_error("MIDI pitch must be 0 to 127");
    auto custom=c.noteKeys.find(midiPitch);
    int64_t key;
    if (custom!=c.noteKeys.end()) key=custom->second;
    else {
        // MIDI has no accidental spelling: round black keys up after transposition.
        int pitch=midiPitch+c.transpose;
        int pitchClass=(pitch%12+12)%12;
        if (pitchClass==1 || pitchClass==3 || pitchClass==6 || pitchClass==8 || pitchClass==10) ++pitch;
        // MIDI middle C is pitch 60. Game keys retain their semitone offsets.
        key=int64_t(c.middleDo)+pitch-60;
    }
    if (key<=0 || key>1000000) throw std::runtime_error("Mapped noteKey must be 1 to 1000000");
    return int(key);
}
std::shared_ptr<const Plan> Build(const midi::Song& song,const std::vector<TrackConfig>& configs,double speed) {
    if (configs.size()!=song.tracks.size()) throw std::runtime_error("Track configuration count mismatch");
    if (!std::isfinite(speed) || speed<0.25 || speed>4) throw std::runtime_error("Speed must be 25% to 400%");
    auto p=std::make_shared<Plan>(); p->durationMs=song.durationMs; p->speed=speed; uint32_t token=1;
    for (size_t ti=0;ti<configs.size();++ti) {
        const auto& c=configs[ti]; if (!c.enabled) continue;
        const bool sustained=UsesSustained(c.instrument,c.sustained);
        if (c.instrument<=0 || c.instrument>1000000 || c.middleDo<=0 || c.middleDo>1000000 || c.transpose<-127 || c.transpose>127)
            throw std::runtime_error("Invalid instrument key, middle Do key, or transpose on track "+std::to_string(ti+1));
        for (const auto& mapping:c.noteKeys) if (mapping.first<0 || mapping.first>127 || mapping.second<1 || mapping.second>1000000)
            throw std::runtime_error("Invalid custom note mapping on track "+std::to_string(ti+1));
        for (const auto& n:song.tracks[ti].notes) {
            if (sustained && n.endMs<=n.startMs) continue;
            int key;
            try {key=MapNoteKey(c,n.pitch);} catch (const std::exception& e) {throw std::runtime_error(std::string(e.what())+" on track "+std::to_string(ti+1));}
            Event e; e.ms=n.startMs; e.token=token++; e.instrument=c.instrument; e.noteKey=key; e.sustained=sustained; e.on=true;
            e.track=int(ti+1); e.midiPitch=n.pitch;
            p->events.push_back(e);
            if (sustained) { p->spans.push_back({e,n.endMs}); e.on=false; e.ms=n.endMs; p->events.push_back(e); }
        }
    }
    std::stable_sort(p->events.begin(),p->events.end(),[](const Event& a,const Event& b){
        if (a.ms!=b.ms) return a.ms<b.ms;
        if (a.on!=b.on) return !a.on;
        return a.token<b.token;
    });
    if (p->events.empty()) throw std::runtime_error("No notes in enabled tracks");
    return p;
}
void Transport::Fail(const std::string& error) { error_=error; state_=State::Error; }
bool Transport::Cleanup() {
    std::string e;
    for (size_t i=0;i<voices_.size();) {
        if (backend_.Stop(voices_[i].handle,e)) voices_.erase(voices_.begin()+i);
        else { ++i; error_=e.empty() ? "Game refused to stop an active note" : e; }
    }
    return voices_.empty();
}
void Transport::SetCursor() {
    if (!plan_) { cursor_=0; return; }
    cursor_=size_t(std::lower_bound(plan_->events.begin(),plan_->events.end(),position_,[](const Event& e,double p){return e.ms<p;})-plan_->events.begin());
}
void Transport::Load(std::shared_ptr<const Plan> p) {
    if (!Cleanup()) { Fail(error_); return; }
    plan_=std::move(p); position_=0; cursor_=0; played_=0; skipped_=0; restore_=false; error_.clear(); warning_.clear(); state_=plan_ ? State::Stopped : State::Empty;
}
void Transport::Play(double now,double countdown) {
    if (!plan_ || state_==State::Playing || state_==State::Countdown) return;
    if (!Cleanup()) { Fail(error_); return; }
    if (state_==State::Finished || position_>=plan_->durationMs) {position_=0;played_=0;skipped_=0;warning_.clear();}
    error_.clear(); SetCursor(); restore_=true; startAt_=now+std::max(0.0,countdown);
    state_=State::Countdown;
}
void Transport::Pause(double now) {
    if (state_!=State::Playing && state_!=State::Countdown) return;
    if (state_==State::Playing) position_=std::min(plan_->durationMs,std::max(position_,(now-anchor_)*plan_->speed));
    state_=State::Paused; restore_=false;
    if (!Cleanup()) Fail(error_);
}
void Transport::Stop() {
    state_=plan_ ? State::Stopped : State::Empty; position_=0; cursor_=0; restore_=false;
    skipped_=0; warning_.clear();
    if (!Cleanup()) Fail(error_); else error_.clear();
}
void Transport::Seek(double pos,double now) {
    if (!plan_) return;
    bool playing=state_==State::Playing || state_==State::Countdown;
    if (!Cleanup()) { Fail(error_); return; }
    position_=std::max(0.0,std::min(plan_->durationMs,pos)); SetCursor(); restore_=playing;
    anchor_=now-position_/plan_->speed; startAt_=now; state_=playing ? State::Playing : State::Paused;
}
bool Transport::Emit(const Event& e) {
    std::string error;
    if (!e.on) {
        auto it=std::find_if(voices_.begin(),voices_.end(),[&](const Voice& v){return v.token==e.token;});
        if (it==voices_.end()) return true;
        if (!backend_.Stop(it->handle,error)) { Fail(NoteContext(e)+" · "+error); return false; }
        voices_.erase(it); return true;
    }
    if (e.sustained && voices_.size()>=256) { Fail("More than 256 simultaneous sustained notes"); return false; }
    if (UsesShort(e.instrument,e.sustained) && !backend_.Short(e.instrument,e.noteKey,error)) { Fail(NoteContext(e)+" · "+error); return false; }
    if (e.sustained) {
        uint32_t handle=0;
        auto result=backend_.Start(e.instrument,e.noteKey,handle,error);
        if (result==StartResult::Rejected && !handle) {
            ++skipped_; warning_=NoteContext(e)+" · "+(error.empty() ? "游戏未创建长音":error);
            return true; // No owned handle: its later note-off must not stop another note.
        }
        if (result!=StartResult::Started || !handle) { Fail(NoteContext(e)+" · "+(error.empty() ? "Game returned an empty play handle":error)); return false; }
        voices_.push_back({e.token,handle});
    }
    ++played_; return true;
}
void Transport::Tick(double now) {
    lastTick_=now;
    if (state_!=State::Playing && state_!=State::Countdown) { if (!voices_.empty()) Cleanup(); return; }
    if (state_==State::Countdown) {
        if (now<startAt_) return;
        anchor_=now-position_/plan_->speed; state_=State::Playing;
    }
    position_=std::min(plan_->durationMs,std::max(position_,(now-anchor_)*plan_->speed));
    size_t count=0;
    if (restore_) {
        restore_=false;
        for (const auto& span:plan_->spans) if (span.start.ms<position_ && span.endMs>position_) {
            if (++count>256 || !Emit(span.start)) { if (state_!=State::Error) Fail("Too many notes at seek position"); Cleanup(); return; }
        }
    }
    if (cursor_<plan_->events.size() && position_-plan_->events[cursor_].ms>1000*plan_->speed) {
        Fail("Game update stalled over one second; stopped to avoid a burst of late notes"); Cleanup(); return;
    }
    while (cursor_<plan_->events.size() && plan_->events[cursor_].ms<=position_) {
        if (++count>256) { Fail("More than 256 note events due in one frame; reduce enabled tracks"); Cleanup(); return; }
        if (!Emit(plan_->events[cursor_++])) { Cleanup(); return; }
    }
    if (cursor_==plan_->events.size() && position_>=plan_->durationMs) {
        state_=State::Finished; if (!Cleanup()) Fail(error_);
    }
}
Snapshot Transport::Status() const {
    Snapshot s; s.state=state_; s.positionMs=position_; s.durationMs=plan_ ? plan_->durationMs : 0;
    s.countdownMs=state_==State::Countdown ? std::max(0.0,startAt_-lastTick_) : 0;
    s.activeVoices=voices_.size(); s.playedNotes=played_; s.skippedNotes=skipped_; s.error=error_; s.warning=warning_; return s;
}
}
