#include "Composition.h"
#include "NotePalette.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>
#include <stdexcept>

namespace composition {
namespace {
using Bytes=std::vector<uint8_t>;
void U32(Bytes& out,uint32_t n) {for (int s=24;s>=0;s-=8) out.push_back(uint8_t(n>>s));}
void F32(Bytes& out,float value) {uint32_t bits;std::memcpy(&bits,&value,4);U32(out,bits);}
void F64(Bytes& out,double value) {uint64_t bits;std::memcpy(&bits,&value,8);U32(out,uint32_t(bits>>32));U32(out,uint32_t(bits));}
void VLQ(Bytes& out,uint32_t n) {
    if (n>0x0fffffff) throw std::runtime_error("MIDI 时间超出范围。");
    uint8_t b[4];int count=0;b[count++]=uint8_t(n&127);
    while (n>>=7) b[count++]=uint8_t((n&127)|128);
    while (count) out.push_back(b[--count]);
}
void Meta(Bytes& out,int type,const Bytes& payload,uint32_t delta=0) {
    VLQ(out,delta);out.push_back(255);out.push_back(uint8_t(type));VLQ(out,uint32_t(payload.size()));out.insert(out.end(),payload.begin(),payload.end());
}
void Text(Bytes& out,int type,const std::string& text) {Meta(out,type,Bytes(text.begin(),text.end()));}
void Chunk(Bytes& out,const Bytes& track) {out.insert(out.end(),{'M','T','r','k'});U32(out,uint32_t(track.size()));out.insert(out.end(),track.begin(),track.end());}
uint32_t Tempo(double ms) {return uint32_t(std::llround(ms*1000));}
bool Duration(double ms) {return std::isfinite(ms) && ms>=.001 && ms<=16777.215;}
void String(Bytes& out,const std::string& text) {U32(out,uint32_t(text.size()));out.insert(out.end(),text.begin(),text.end());}
struct Reader {
    const Bytes& bytes;size_t pos=0;
    explicit Reader(const Bytes& value):bytes(value) {}
    uint32_t U32() {if (bytes.size()-pos<4) throw std::runtime_error("创作草稿不完整。");uint32_t n=0;for (int i=0;i<4;++i) n=(n<<8)|bytes[pos++];return n;}
    float F32() {auto bits=U32();float value;std::memcpy(&value,&bits,4);return value;}
    double F64() {uint64_t bits=uint64_t(U32())<<32;bits|=U32();double value;std::memcpy(&value,&bits,8);return value;}
    bool Bool() {auto value=U32();if (value>1) throw std::runtime_error("创作工程开关值无效。");return value!=0;}
    std::string String() {auto n=U32();if (n>255 || n>bytes.size()-pos) throw std::runtime_error("创作草稿音轨名无效。");std::string text(bytes.begin()+pos,bytes.begin()+pos+n);pos+=n;return text;}
};
void ValidateEditor(const Project& p,const EditorSettings& e) {
    if (e.selectedTrack<0 || e.selectedTrack>=int(p.tracks.size()) || e.snap<0 || e.snap>4 || e.newLength<1 || e.newLength>MaxBeats*PPQ ||
        e.velocity<1 || e.velocity>127 || e.inspectorTab<0 || e.inspectorTab>1 || !std::isfinite(e.zoom) || e.zoom<1 || e.zoom>32 ||
        !std::isfinite(e.pan) || e.pan<0 || e.pan>p.beats || !std::isfinite(e.positionBeat) || e.positionBeat<0 || e.positionBeat>p.beats ||
        !std::isfinite(e.sidebarWidth) || e.sidebarWidth<1 || e.sidebarWidth>10000 || !std::isfinite(e.trackScroll) || e.trackScroll<0 || e.trackScroll>1000000 ||
        e.selection.size()>p.NoteCount()) throw std::runtime_error("创作工程界面设置无效。");
    size_t found=0;bool active=!e.activeNote;
    for (size_t i=0;i<p.tracks.size();++i) for (const auto& n:p.tracks[i].notes) {
        if (e.selection.count(n.id)) {if (!p.tracks[i].visible) throw std::runtime_error("创作工程选择了隐藏音轨的音符。");++found;}
        if (n.id==e.activeNote && int(i)==e.selectedTrack && e.selection.count(n.id)) active=true;
    }
    if (found!=e.selection.size() || !active) throw std::runtime_error("创作工程音符选择无效。");
}
}
Project::Project() {AddTrack();}
int Project::AddTrack() {
    if (tracks.size()>=MaxTracks) throw std::runtime_error("最多支持 128 条创作音轨。");
    Track track;track.id=nextId++;track.name="音轨 "+std::to_string(tracks.size()+1);
    float hue=std::fmod(float(track.id-1)*.618033989f+.55f,1.f),s=.56f,v=.86f;
    float c=v*s,x=c*(1-std::abs(std::fmod(hue*6,2.f)-1)),m=v-c;float rgb[3]{};
    int sector=int(hue*6);if (sector==0) {rgb[0]=c;rgb[1]=x;}else if (sector==1) {rgb[0]=x;rgb[1]=c;}
    else if (sector==2) {rgb[1]=c;rgb[2]=x;}else if (sector==3) {rgb[1]=x;rgb[2]=c;}
    else if (sector==4) {rgb[0]=x;rgb[2]=c;}else {rgb[0]=c;rgb[2]=x;}
    for (int i=0;i<3;++i) track.color[i]=rgb[i]+m;
    tracks.push_back(track);return int(tracks.size()-1);
}
size_t Project::NoteCount() const {size_t n=0;for (const auto& track:tracks) n+=track.notes.size();return n;}
bool Project::Place(int track,const Note& note) const {
    if (track<0 || track>=int(tracks.size()) || note.row<0 || note.row>=21 || note.start<0 || note.length<=0 ||
        note.start>beats*PPQ-note.length || note.velocity<1 || note.velocity>127) return false;
    for (const auto& other:tracks[track].notes) if (other.id!=note.id && other.row==note.row &&
        note.start<other.start+other.length && other.start<note.start+note.length) return false;
    return true;
}
bool Project::MoveNotes(const std::set<unsigned>& ids,int tickDelta,int rowDelta) {
    if (ids.empty()) return false;
    struct Move {Note* target;Note value;};std::vector<Move> moves;
    for (auto& track:tracks) {
        if (std::none_of(track.notes.begin(),track.notes.end(),[&](const Note& n) {return ids.count(n.id)!=0;})) continue;
        auto placed=track.notes;
        for (size_t i=0;i<placed.size();++i) if (ids.count(placed[i].id)) {
            auto& n=placed[i];int64_t start=int64_t(n.start)+tickDelta,row=int64_t(n.row)+rowDelta;
            if (start<0 || start>beats*PPQ-n.length || row<0 || row>=notepalette::Count) return false;
            n.start=int(start);n.row=int(row);moves.push_back({&track.notes[i],n});
        }
        std::sort(placed.begin(),placed.end(),[](const Note& a,const Note& b) {return a.row!=b.row ? a.row<b.row:a.start<b.start;});
        for (size_t i=1;i<placed.size();++i) if (placed[i].row==placed[i-1].row && placed[i].start<placed[i-1].start+placed[i-1].length) return false;
    }
    if (moves.size()!=ids.size()) return false;
    for (const auto& move:moves) *move.target=move.value;return true;
}
unsigned Project::AddNote(int track,int row,int start,int length,int velocity) {
    if (NoteCount()>=200000) throw std::runtime_error("创作音符数量已达上限。");
    Note note;note.id=nextId;note.row=row;note.start=start;note.length=length;note.velocity=velocity;
    if (!Place(track,note)) throw std::runtime_error("同一音轨的同音高音符不能重叠，且音符必须在时间轴内。");
    ++nextId;tracks[track].notes.push_back(note);return note.id;
}
double Project::BaseTempo(int tick) const {auto it=tempoChanges.upper_bound(tick);return it==tempoChanges.begin() ? beatMs:(--it)->second;}
double Project::BeatDuration(int beat) const {return Time(std::min(beats,beat+1))-Time(beat);}
std::map<int,double> Project::Tempos() const {
    auto events=tempoChanges;events[0]=beatMs;
    for (const auto& b:beatTimes) {
        int start=b.first*PPQ,end=start+PPQ;auto it=events.upper_bound(start);
        while (it!=events.end() && it->first<end) it=events.erase(it);
        events[start]=b.second;if (end<beats*PPQ) events[end]=BaseTempo(end);
    }
    return events;
}
double Project::Time(double beat) const {
    double tick=std::max(0.,std::min(double(beats),beat))*PPQ,ms=0,previous=0,tempo=beatMs;
    for (const auto& e:Tempos()) {if (e.first>tick) break;ms+=(e.first-previous)*tempo/PPQ;previous=e.first;tempo=e.second;}return ms+(tick-previous)*tempo/PPQ;
}
double Project::BeatAt(double ms) const {
    ms=std::max(0.,ms);double previous=0,tempo=beatMs;
    for (const auto& e:Tempos()) {double span=(e.first-previous)*tempo/PPQ;if (ms<span) return (previous+ms/tempo*PPQ)/PPQ;ms-=span;previous=e.first;tempo=e.second;}return std::min(double(beats),(previous+ms/tempo*PPQ)/PPQ);
}
void Project::Validate() const {
    if (beats<1 || beats>MaxBeats || !Duration(beatMs) || tracks.empty() || tracks.size()>MaxTracks || NoteCount()>200000) throw std::runtime_error("创作项目参数无效。");
    for (const auto& b:beatTimes) if (b.first<0 || b.first>=beats || !Duration(b.second)) throw std::runtime_error("每拍时长无效。");
    if (tempoChanges.size()>65536) throw std::runtime_error("速度变化数量过多。");
    for (const auto& tempo:tempoChanges) if (tempo.first<=0 || tempo.first>=beats*PPQ || !Duration(tempo.second)) throw std::runtime_error("导入的速度变化无效。");
    std::set<unsigned> ids;unsigned maxId=0;
    for (size_t i=0;i<tracks.size();++i) {
        const auto& track=tracks[i];if (!track.id || !ids.insert(track.id).second || track.name.empty() || track.name.size()>255 ||
            track.instrument<1 || track.instrument>1000000 || track.middleDo<13 || track.middleDo>999977) throw std::runtime_error("创作音轨参数无效。");
        maxId=std::max(maxId,track.id);
        for (float c:track.color) if (!std::isfinite(c) || c<0 || c>1) throw std::runtime_error("音轨颜色无效。");
        std::vector<const Note*> ordered;
        for (const auto& note:track.notes) {
            if (!note.id || !ids.insert(note.id).second || note.row<0 || note.row>=21 || note.start<0 || note.length<=0 ||
                note.start>beats*PPQ-note.length || note.velocity<1 || note.velocity>127) throw std::runtime_error("创作音符无效。");
            maxId=std::max(maxId,note.id);
            ordered.push_back(&note);
        }
        std::sort(ordered.begin(),ordered.end(),[](const Note* a,const Note* b) {return a->row!=b->row ? a->row<b->row:a->start<b->start;});
        for (size_t n=1;n<ordered.size();++n) if (ordered[n]->row==ordered[n-1]->row && ordered[n]->start<ordered[n-1]->start+ordered[n-1]->length) throw std::runtime_error("同一轨的同音高音符不能重叠。");
    }
    if (nextId<=maxId) throw std::runtime_error("创作草稿标识无效。");
}
Bytes Project::Midi() const {
    Validate();Bytes out={'M','T','h','d',0,0,0,6,0,1,uint8_t((tracks.size()+1)>>8),uint8_t(tracks.size()+1),uint8_t(PPQ>>8),uint8_t(PPQ&255)};
    Bytes conductor;Text(conductor,3,"Tempo");Meta(conductor,0x58,{4,2,24,8});
    uint32_t previous=0,lastTempo=0;
    for (const auto& e:Tempos()) {
        auto us=Tempo(e.second);if (us==lastTempo) continue;
        uint32_t tick=uint32_t(e.first);Meta(conductor,0x51,{uint8_t(us>>16),uint8_t(us>>8),uint8_t(us)},tick-previous);previous=tick;lastTempo=us;
    }
    Meta(conductor,0x2f,{},uint32_t(beats*PPQ)-previous);Chunk(out,conductor);
    for (size_t i=0;i<tracks.size();++i) {
        const auto& track=tracks[i];Bytes data;Text(data,3,track.name);Text(data,4,"Game instrument "+std::to_string(track.instrument));
        // Non-commercial sequencer metadata preserves game configuration without confusing it with GM Program numbers.
        Bytes config={0x7d,'M','P','P',1};U32(config,uint32_t(track.instrument));U32(config,uint32_t(track.middleDo));config.push_back(uint8_t(track.sustained || track.instrument==11));Meta(data,0x7f,config);
        int channel=int(i%15);if (channel>=9) ++channel;Meta(data,0x21,{uint8_t(i/15)});
        struct Event {int tick,pitch,velocity;bool on;unsigned id;};std::vector<Event> events;
        for (const auto& n:track.notes) {events.push_back({n.start,notepalette::RowPitch(n.row),n.velocity,true,n.id});events.push_back({n.start+n.length,notepalette::RowPitch(n.row),0,false,n.id});}
        std::sort(events.begin(),events.end(),[](const Event& a,const Event& b) {if (a.tick!=b.tick) return a.tick<b.tick;if (a.on!=b.on) return !a.on;if (a.pitch!=b.pitch) return a.pitch<b.pitch;return a.id<b.id;});
        previous=0;for (const auto& e:events) {VLQ(data,uint32_t(e.tick)-previous);data.push_back(uint8_t((e.on ? 0x90:0x80)|channel));data.push_back(uint8_t(e.pitch));data.push_back(uint8_t(e.velocity));previous=uint32_t(e.tick);}
        Meta(data,0x2f,{},uint32_t(beats*PPQ)-previous);Chunk(out,data);
    }return out;
}
Bytes Project::Draft(const EditorSettings* editor) const {
    Validate();if (editor) ValidateEditor(*this,*editor);Bytes out={'M','P','C',uint8_t(editor ? 3:2)};
    auto duration=[&](double ms) {if (editor) F64(out,ms);else U32(out,Tempo(ms));};
    U32(out,uint32_t(beats));duration(beatMs);U32(out,nextId);U32(out,uint32_t(beatTimes.size()));
    for (const auto& beat:beatTimes) {U32(out,uint32_t(beat.first));duration(beat.second);}
    U32(out,uint32_t(tempoChanges.size()));for (const auto& tempo:tempoChanges) {U32(out,uint32_t(tempo.first));duration(tempo.second);}
    U32(out,uint32_t(tracks.size()));for (const auto& track:tracks) {
        U32(out,track.id);String(out,track.name);U32(out,uint32_t(track.instrument));U32(out,uint32_t(track.middleDo));U32(out,uint32_t(track.sustained));U32(out,uint32_t(track.visible));
        for (float c:track.color) {if (editor) F32(out,c);else U32(out,uint32_t(std::llround(c*65535)));}
        U32(out,uint32_t(track.notes.size()));for (const auto& note:track.notes) {U32(out,note.id);U32(out,uint32_t(note.row));U32(out,uint32_t(note.start));U32(out,uint32_t(note.length));U32(out,uint32_t(note.velocity));}
    }
    if (editor) {
        const auto& e=*editor;U32(out,uint32_t(e.selectedTrack));U32(out,uint32_t(e.snap));U32(out,uint32_t(e.newLength));U32(out,uint32_t(e.velocity));
        F32(out,e.zoom);F32(out,e.pan);F64(out,e.positionBeat);U32(out,uint32_t(e.receiveGame));U32(out,e.activeNote);
        U32(out,uint32_t(e.selection.size()));for (auto id:e.selection) U32(out,id);
        U32(out,uint32_t(e.inspectorTab));U32(out,uint32_t(e.moreSettings));F32(out,e.sidebarWidth);F32(out,e.trackScroll);
    }return out;
}
Project Project::ReadDraft(const Bytes& bytes,EditorSettings* editor) {
    if (bytes.size()>8*1024*1024) throw std::runtime_error("创作草稿过大。");
    Reader in{bytes};auto version=in.U32();if (version<0x4d504301 || version>0x4d504303) throw std::runtime_error("无法识别创作工程版本。");
    bool full=version==0x4d504303;auto duration=[&]() {return full ? in.F64():in.U32()/1000.;};
    Project p;p.tracks.clear();p.beats=int(in.U32());p.beatMs=duration();p.nextId=in.U32();auto beatCount=in.U32();if (beatCount>MaxBeats) throw std::runtime_error("创作草稿拍数无效。");
    for (uint32_t i=0;i<beatCount;++i) {int beat=int(in.U32());double ms=duration();if (!p.beatTimes.emplace(beat,ms).second) throw std::runtime_error("创作草稿拍数重复。");}
    if (version>=0x4d504302) {auto tempos=in.U32();if (tempos>65536) throw std::runtime_error("创作草稿速度变化过多。");for (uint32_t i=0;i<tempos;++i) {int tick=int(in.U32());double ms=duration();if (!p.tempoChanges.emplace(tick,ms).second) throw std::runtime_error("创作草稿速度变化重复。");}}
    auto count=in.U32();if (!count || count>MaxTracks) throw std::runtime_error("创作草稿音轨数量无效。");
    size_t noteCount=0;for (uint32_t i=0;i<count;++i) {
        Track track;track.id=in.U32();track.name=in.String();track.instrument=int(in.U32());track.middleDo=int(in.U32());track.sustained=in.Bool();track.visible=in.Bool();
        for (float& c:track.color) c=full ? in.F32():in.U32()/65535.f;auto n=in.U32();noteCount+=n;if (noteCount>200000) throw std::runtime_error("创作草稿音符过多。");
        for (uint32_t j=0;j<n;++j) {Note note;note.id=in.U32();note.row=int(in.U32());note.start=int(in.U32());note.length=int(in.U32());note.velocity=int(in.U32());track.notes.push_back(note);}p.tracks.push_back(std::move(track));
    }
    EditorSettings e;
    if (full) {
        e.selectedTrack=int(in.U32());e.snap=int(in.U32());e.newLength=int(in.U32());e.velocity=int(in.U32());e.zoom=in.F32();e.pan=in.F32();e.positionBeat=in.F64();
        e.receiveGame=in.Bool();e.activeNote=in.U32();auto selectedCount=in.U32();if (selectedCount>200000) throw std::runtime_error("创作工程选择数量无效。");
        for (uint32_t i=0;i<selectedCount;++i) if (!e.selection.insert(in.U32()).second) throw std::runtime_error("创作工程选择标识重复。");
        e.inspectorTab=int(in.U32());e.moreSettings=in.Bool();e.sidebarWidth=in.F32();e.trackScroll=in.F32();
    }
    if (in.pos!=bytes.size()) throw std::runtime_error("创作草稿包含多余数据。");p.Validate();ValidateEditor(p,e);if (editor) *editor=std::move(e);return p;
}
Project Project::Import(const midi::Song& song,ImportStats& stats) {
    stats=ImportStats{};Project p;p.tracks.clear();p.nextId=1;
    bool smpte=(song.division&0x8000)!=0;if (!song.division) throw std::runtime_error("MIDI 时间格式无效。");
    if (!smpte && !song.tempos.empty()) p.beatMs=song.tempos.front().microseconds/1000.;
    auto ticks=[&](const midi::Note& n,bool end) -> int {
        double value=smpte ? (end ? n.endMs:n.startMs)/p.beatMs*PPQ:double(end ? n.endTick:n.startTick)*PPQ/song.division;
        if (!std::isfinite(value) || value<0 || value>MaxBeats*PPQ) throw std::runtime_error("MIDI 长度超过创作模式的 4096 拍上限。");return int(std::llround(value));
    };
    int lastTick=0;
    if (smpte) {double end=song.durationMs/p.beatMs*PPQ;if (!std::isfinite(end) || end<0 || end>MaxBeats*PPQ) throw std::runtime_error("MIDI 长度超过 4096 拍。");lastTick=int(std::llround(end));}
    for (const auto& source:song.tracks) {
        if (!smpte) {double end=double(source.endTick)*PPQ/song.division;if (end>MaxBeats*PPQ) throw std::runtime_error("MIDI 长度超过 4096 拍。");lastTick=std::max(lastTick,int(std::llround(end)));}
        if (source.notes.empty()) continue;
        int index=p.AddTrack();auto& track=p.tracks[index];
        if (!source.name.empty()) {size_t length=std::min(size_t(255),source.name.size());if (length<source.name.size()) while (length && (uint8_t(source.name[length])&0xc0)==0x80) --length;track.name=source.name.substr(0,length);}
        if (source.gameInstrument) {track.instrument=source.gameInstrument;track.middleDo=source.gameMiddleDo;track.sustained=source.gameSustained;}
        std::vector<Note> notes;
        for (const auto& n:source.notes) {
            int pitch=n.pitch;if (pitch%12==1 || pitch%12==3 || pitch%12==6 || pitch%12==8 || pitch%12==10) ++pitch;
            int row=notepalette::Row(pitch);if (row<0) {++stats.skipped;continue;}
            if (pitch!=n.pitch) ++stats.converted;
            Note note;note.row=row;note.start=ticks(n,false);int end=ticks(n,true);note.length=std::max(1,end-note.start);note.velocity=std::max(1,std::min(127,n.velocity));
            if (note.start+note.length>MaxBeats*PPQ) throw std::runtime_error("MIDI 长度超过 4096 拍。");lastTick=std::max(lastTick,note.start+note.length);notes.push_back(note);
        }
        std::sort(notes.begin(),notes.end(),[](const Note& a,const Note& b) {return a.row!=b.row ? a.row<b.row:a.start<b.start;});
        for (const auto& note:notes) {
            if (!track.notes.empty()) {auto& previous=track.notes.back();if (previous.row==note.row && note.start<previous.start+previous.length) {previous.length=std::max(previous.start+previous.length,note.start+note.length)-previous.start;previous.velocity=std::max(previous.velocity,note.velocity);++stats.merged;continue;}}
            auto unique=note;unique.id=p.nextId++;track.notes.push_back(unique);if (++stats.imported>200000) throw std::runtime_error("MIDI 音符超过创作模式的 20 万个音符上限。");
        }
    }
    if (p.tracks.empty()) p.AddTrack();p.beats=std::max(1,(lastTick+PPQ-1)/PPQ);
    if (!smpte) for (const auto& tempo:song.tempos) {
        double tick=double(tempo.tick)*PPQ/song.division;if (tick>0 && tick<p.beats*PPQ) {int rounded=int(std::llround(tick));if (!rounded) p.beatMs=tempo.microseconds/1000.;else if (rounded<p.beats*PPQ) p.tempoChanges[rounded]=tempo.microseconds/1000.;}
    }
    p.tempoChanges.erase(0);p.Validate();return p;
}
}
