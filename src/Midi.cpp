#include "Midi.h"
#include <algorithm>
#include <array>
#include <deque>
#include <limits>
#include <map>
#include <stdexcept>
#include <utility>

namespace midi {
namespace {
struct Reader {
    const std::vector<uint8_t>& b; size_t p, end;
    void need(size_t n) { if (n > end - p) throw std::runtime_error("MIDI data is truncated"); }
    uint8_t u8() { need(1); return b[p++]; }
    uint16_t u16() { uint16_t v=u8(); return uint16_t((v<<8)|u8()); }
    uint32_t u32() { uint32_t v=u16(); return (v<<16)|u16(); }
    uint32_t vlq() {
        uint32_t v=0;
        for (int i=0;i<4;++i) { auto c=u8(); v=(v<<7)|(c&127); if (!(c&128)) return v; }
        throw std::runtime_error("MIDI VLQ exceeds four bytes");
    }
    bool tag(const char* s) { need(4); bool ok=std::equal(b.begin()+p,b.begin()+p+4,s); p+=4; return ok; }
};
struct Raw { uint64_t tick; size_t order, track; int status, a, b,port; };
struct Tempo {
    uint64_t tick; size_t order; uint32_t us; double atMs;
    Tempo(uint64_t t,size_t o,uint32_t u,double at):tick(t),order(o),us(u),atMs(at) {}
};
struct Ref { size_t track,note; };
}
Song Parse(const std::vector<uint8_t>& bytes) {
    if (bytes.size() > 64*1024*1024) throw std::runtime_error("MIDI exceeds 64 MiB");
    Reader r{bytes,0,bytes.size()};
    if (!r.tag("MThd")) throw std::runtime_error("Missing MThd header");
    auto hlen=r.u32(); if (hlen<6) throw std::runtime_error("Invalid MIDI header length");
    r.need(hlen); const auto hend=r.p+hlen;
    Song s; s.format=r.u16(); auto count=r.u16(); s.division=r.u16(); r.p=hend;
    if (s.format>1) throw std::runtime_error("Format 2 contains independent songs; use format 0 or 1");
    if (!count || count>1024 || (s.format==0 && count!=1)) throw std::runtime_error("Invalid MIDI track count");
    if (!s.division) throw std::runtime_error("MIDI time division is zero");
    double smpteMs=0;
    if (s.division&0x8000) {
        int fps=-int(int8_t(s.division>>8)), tpf=s.division&255;
        if (!tpf || (fps!=24 && fps!=25 && fps!=29 && fps!=30)) throw std::runtime_error("Invalid SMPTE division");
        smpteMs=1000.0/((fps==29 ? 30000.0/1001.0 : double(fps))*tpf);
    }
    s.tracks.resize(count);
    std::vector<Raw> events; std::vector<Tempo> tempos{{0,0,500000,0}};
    size_t order=1, eventCount=0;
    uint64_t lastTick=0;
    for (size_t ti=0;ti<count;++ti) {
        if (!r.tag("MTrk")) throw std::runtime_error("Missing MTrk chunk");
        auto length=r.u32(); r.need(length);
        Reader t{bytes,r.p,r.p+length}; r.p+=length;
        uint64_t tick=0; uint8_t running=0; bool ended=false;int port=0;
        while (t.p<t.end) {
            if (++eventCount>2000000) throw std::runtime_error("MIDI has too many events");
            auto delta=t.vlq();
            if (tick>std::numeric_limits<uint64_t>::max()-delta) throw std::runtime_error("MIDI tick overflow");
            tick+=delta; size_t eo=order++; auto status=t.u8();
            if (status<128) {
                if (!running) throw std::runtime_error("Running status without a channel message");
                --t.p; status=running;
            }
            if (status==255) {
                running=0; auto type=t.u8(); auto n=t.vlq(); t.need(n);
                if (type==3 && s.tracks[ti].name.empty())
                    s.tracks[ti].name.assign(reinterpret_cast<const char*>(bytes.data()+t.p),n);
                if (type==0x21 && n==1 && bytes[t.p]<128) port=bytes[t.p];
                if (type==0x7f && n==14 && bytes[t.p]==0x7d && bytes[t.p+1]=='M' && bytes[t.p+2]=='P' && bytes[t.p+3]=='P' && bytes[t.p+4]==1) {
                    auto number=[&](size_t at) {uint32_t v=0;for (int i=0;i<4;++i) v=(v<<8)|bytes[at+i];return v;};
                    auto instrument=number(t.p+5),middle=number(t.p+9);
                    if (instrument>=1 && instrument<=1000000 && middle>=13 && middle<=999977 && bytes[t.p+13]<=1) {
                        s.tracks[ti].gameInstrument=int(instrument);s.tracks[ti].gameMiddleDo=int(middle);s.tracks[ti].gameSustained=bytes[t.p+13]!=0;
                    }
                }
                if (type==81) {
                    if (n!=3) throw std::runtime_error("Invalid tempo event length");
                    uint32_t us=(uint32_t(bytes[t.p])<<16)|(uint32_t(bytes[t.p+1])<<8)|bytes[t.p+2];
                    if (!us) throw std::runtime_error("MIDI tempo is zero");
                    tempos.push_back({tick,eo,us,0});
                }
                t.p+=n;
                if (type==47) { if (n) throw std::runtime_error("Invalid end-of-track event"); ended=true; break; }
            } else if (status==240 || status==247) {
                running=0; auto n=t.vlq(); t.need(n); t.p+=n;
            } else if (status>=128 && status<240) {
                running=status; int op=status&240, a=t.u8(), b=0;
                if (op!=192 && op!=208) b=t.u8();
                if (a>127 || b>127) throw std::runtime_error("Invalid MIDI channel data byte");
                s.tracks[ti].channels|=uint16_t(1u<<(status&15));
                if (op==192 && std::find(s.tracks[ti].programs.begin(),s.tracks[ti].programs.end(),a)==s.tracks[ti].programs.end())
                    s.tracks[ti].programs.push_back(a);
                events.push_back({tick,eo,ti,status,a,b,port});
            } else throw std::runtime_error("Unsupported status in MIDI track");
        }
        if (!ended) s.warnings.push_back("Track "+std::to_string(ti+1)+" has no end-of-track marker");
        s.tracks[ti].endTick=tick; lastTick=std::max(lastTick,tick);
    }
    std::stable_sort(events.begin(),events.end(),[](const Raw& a,const Raw& b){ return a.tick!=b.tick ? a.tick<b.tick : a.order<b.order; });
    struct Port {std::array<std::array<std::deque<Ref>,128>,16> active;std::array<std::vector<Ref>,16> held;std::array<bool,16> sustain{};};
    std::map<int,Port> ports;
    auto finish=[&](Ref ref,uint64_t at) { auto& n=s.tracks[ref.track].notes[ref.note]; n.endTick=std::max(at,n.startTick); };
    auto off=[&](Port& port,int ch,int pitch,uint64_t at) {
        auto& q=port.active[ch][pitch]; if (q.empty()) return;
        auto ref=q.front(); q.pop_front(); if (port.sustain[ch]) port.held[ch].push_back(ref); else finish(ref,at);
    };
    for (const auto& e:events) {
        auto& port=ports[e.port];
        int ch=e.status&15, op=e.status&240;
        if (op==144 && e.b) {
            auto& ns=s.tracks[e.track].notes;
            Note n; n.pitch=e.a; n.velocity=e.b; n.channel=ch; n.startTick=n.endTick=e.tick;
            ns.push_back(n); port.active[ch][e.a].push_back({e.track,ns.size()-1});
        } else if (op==128 || (op==144 && !e.b)) off(port,ch,e.a,e.tick);
        else if (op==176 && e.a==64) {
            port.sustain[ch]=e.b>=64;
            if (!port.sustain[ch]) { for (auto ref:port.held[ch]) finish(ref,e.tick); port.held[ch].clear(); }
        } else if (op==176 && (e.a==120 || e.a==123)) {
            for (int p=0;p<128;++p) while (!port.active[ch][p].empty()) off(port,ch,p,e.tick);
            if (e.a==120) { for (auto ref:port.held[ch]) finish(ref,e.tick); port.held[ch].clear(); }
        }
    }
    size_t dangling=0;
    for (auto& pair:ports) for (int ch=0;ch<16;++ch) {
        for (auto& q:pair.second.active[ch]) for (auto ref:q) { finish(ref,lastTick); ++dangling; }
        for (auto ref:pair.second.held[ch]) { finish(ref,lastTick); ++dangling; }
    }
    if (dangling) s.warnings.push_back(std::to_string(dangling)+" unclosed notes ended at song end");
    std::stable_sort(tempos.begin(),tempos.end(),[](const Tempo& a,const Tempo& b){ return a.tick!=b.tick ? a.tick<b.tick : a.order<b.order; });
    std::vector<Tempo> map;
    for (auto tempo:tempos) { if (!map.empty() && map.back().tick==tempo.tick) map.back()=tempo; else map.push_back(tempo); }
    for (const auto& tempo:map) s.tempos.push_back({tempo.tick,tempo.us});
    for (size_t i=1;i<map.size();++i)
        map[i].atMs=map[i-1].atMs+double(map[i].tick-map[i-1].tick)*map[i-1].us/(s.division*1000.0);
    auto ms=[&](uint64_t tick) {
        if (smpteMs) return double(tick)*smpteMs;
        auto it=std::upper_bound(map.begin(),map.end(),tick,[](uint64_t t,const Tempo& p){return t<p.tick;}); --it;
        return it->atMs+double(tick-it->tick)*it->us/(s.division*1000.0);
    };
    s.durationMs=ms(lastTick);
    for (auto& tr:s.tracks) {
        for (auto& n:tr.notes) { n.startMs=ms(n.startTick); n.endMs=ms(n.endTick); }
        std::stable_sort(tr.notes.begin(),tr.notes.end(),[](const Note& a,const Note& b){return a.startTick<b.startTick;});
    }
    if (s.durationMs>24*60*60*1000.0) throw std::runtime_error("MIDI duration exceeds 24 hours");
    return s;
}
}
