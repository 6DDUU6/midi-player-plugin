#include "Locator.h"
#include "../vendor/MinHook/src/hde/hde64.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace locator {
const std::map<std::string,std::string>& Patterns() {
    static const std::map<std::string,std::string> p={
        {"emitShort","C7 ?? 30 05 00 00 00 48 8B ?? ?? 48 85 C9"},
        {"emitStart","C7 ?? 30 06 00 00 00 48 8B ?? ?? 48 85 C9"},
        {"emitStop","C7 ?? 30 07 00 00 00 48 8B ?? ?? 48 85 C9"},
        {"shortPlay","44 89 CD 44 89 C3 48 89 D7 49 89 CE 44 0F B6 BC 24 ?? ?? ?? ?? 80 3D ?? ?? ?? ?? 00 0F 85 ?? ?? ?? ?? 49 8B 46 10 48 85 C0"},
        {"component","BA 00 C0 00 00 E8 ?? ?? ?? ?? 48 85 C0"},
        {"mainThread","48 83 EC 28 E8 ?? ?? ?? ?? 48 85 C0 74 0C 48 8B 08 48 3B 0D ?? ?? ?? ?? 74 05 48 83 C4 28 C3 48 89 C1 48 83 C4 28 E9"}
    }; return p;
}
std::vector<int> Pattern(const std::string& text) {
    std::istringstream in(text); std::string token; std::vector<int> result;
    while (in>>token) {
        if (token=="?" || token=="??") result.push_back(-1);
        else {
            if (token.size()!=2 || token.find_first_not_of("0123456789abcdefABCDEF")!=std::string::npos) throw std::runtime_error("Invalid byte signature");
            result.push_back(std::stoi(token,nullptr,16));
        }
    }
    if (result.empty()) throw std::runtime_error("Empty byte signature");
    return result;
}
std::vector<uint32_t> Scan(const uint8_t* bytes,size_t size,uint32_t rva,const std::vector<int>& pattern) {
    std::vector<uint32_t> hits;
    if (size<pattern.size()) return hits;
    size_t anchor=0;
    while (anchor<pattern.size() && pattern[anchor]<0) ++anchor;
    if (anchor==pattern.size()) throw std::runtime_error("All-wildcard signature");
    size_t offset=0,limit=size-pattern.size();
    while (offset<=limit) {
        auto found=static_cast<const uint8_t*>(std::memchr(bytes+offset+anchor,pattern[anchor],limit-offset+1));
        if (!found) break;
        offset=size_t(found-bytes)-anchor; bool ok=true;
        for (size_t i=0;i<pattern.size();++i) if (pattern[i]>=0 && bytes[offset+i]!=pattern[i]) {ok=false;break;}
        if (ok) { hits.push_back(rva+uint32_t(offset)); if (hits.size()>4096) throw std::runtime_error("Too many signature matches"); }
        ++offset;
    }
    return hits;
}
Body Decode(const uint8_t* bytes,size_t size,uint32_t start) {
    if (!size || size>65536) throw std::runtime_error("Invalid function extent");
    Body b; b.start=start; b.end=start+uint32_t(size); bool prologue=true;
    for (size_t pc=0;pc<size;) {
        uint8_t scratch[16]{}; std::memcpy(scratch,bytes+pc,std::min(size-pc,sizeof(scratch)));
        hde64s h{}; hde64_disasm(scratch,&h);
        if ((h.flags&F_ERROR) || !h.len || pc+h.len>size) throw std::runtime_error("Cannot decode complete function at RVA "+std::to_string(start+uint32_t(pc)));
        uint32_t at=start+uint32_t(pc); b.instructions.insert(at);
        if (h.opcode==0xE8 || h.opcode==0xE9) {
            uint32_t target=at+h.len+int32_t(h.imm.imm32);
            if (target<b.start || target>=b.end) b.refs.push_back({at,target,h.opcode==0xE9});
        }
        int reg=h.modrm_reg+(h.rex_r ? 8:0), rm=h.modrm_rm+(h.rex_b ? 8:0);
        int base=rm;
        if (h.flags&F_SIB) base=h.sib_base+(h.rex_b ? 8:0);
        if (h.modrm_mod==0 && h.modrm_rm==5) base=16; // RIP
        int disp=(h.flags&F_DISP8) ? int(int8_t(h.disp.disp8)) : ((h.flags&F_DISP32) ? int(int32_t(h.disp.disp32)):0);
        bool mov=h.opcode==0x89 || h.opcode==0x8B;
        if (prologue && mov && h.modrm_mod==3) {
            int source=h.opcode==0x89 ? reg:rm, destination=h.opcode==0x89 ? rm:reg;
            bool argument=source==1 || source==2 || source==8 || source==9;
            bool nonvolatile=destination==3 || destination==5 || destination==6 || destination==7 || destination>=12;
            if (argument && nonvolatile) b.savedArgs.insert(source);
        }
        if ((h.opcode>=0x70 && h.opcode<=0x7F) || h.opcode==0xE8 || h.opcode==0xE9 || h.opcode==0xEB || h.opcode==0xFF || h.opcode==0xC3 || h.opcode==0xC2 || (h.opcode==0x0F && h.opcode2>=0x80 && h.opcode2<=0x8F)) prologue=false;
        if (h.opcode==0x8B && h.modrm_mod!=3 && h.rex_w && reg==1) b.loads[at]=disp;
        if (h.modrm_mod!=3 && !h.rex_w && !h.p_66) {
            if (h.opcode==0x89) b.stores.push_back({at,base,disp,false,uint32_t(reg)});
            if (h.opcode==0xC7 && h.modrm_reg==0) b.stores.push_back({at,base,disp,true,h.imm.imm32});
        }
        if (((h.opcode==0x81 && h.modrm_reg==1) || h.opcode==0x0D) && h.imm.imm32==0x80000000) b.highBit=true;
        if (h.opcode==0xFF && h.modrm_reg==2 && h.modrm_mod!=3) b.indirectOffsets.push_back(disp);
        pc+=h.len;
    }
    return b;
}
bool ContainsReference(const uint8_t* bytes,size_t size,uint32_t start,const std::vector<uint32_t>& targets) {
    for (size_t i=0;i+5<=size;++i) if (bytes[i]==0xE8 || bytes[i]==0xE9) {
        int32_t disp=0;std::memcpy(&disp,bytes+i+1,4);
        auto target=start+uint32_t(i)+5+disp;
        if (std::find(targets.begin(),targets.end(),target)!=targets.end()) return true;
    }
    return false;
}
namespace {
template<class T> T only(const std::vector<T>& values,const std::string& name) {
    if (values.size()!=1) throw std::runtime_error(name+": expected one candidate, got "+std::to_string(values.size()));
    return values[0];
}
bool refs(const Body& b,uint32_t target) { for (auto r:b.refs) if (r.target==target) return true; return false; }
}
Result Resolve(const Hits& hits,const Reader& reader) {
    auto read=[&](uint32_t rva) {return reader(rva,{});};
    std::map<std::string,Body> bodies; std::map<std::string,uint32_t> anchors;
    for (const auto& name:{"emitShort","emitStart","emitStop","shortPlay"}) {
        anchors[name]=only(hits.at(name),name); auto b=read(anchors[name]);
        if (!b.instructions.count(anchors[name])) throw std::runtime_error(std::string(name)+": signature is not instruction-aligned");
        bodies[name]=std::move(b);
    }
    std::vector<uint32_t> appends; std::vector<int> offsets;
    std::vector<std::set<uint32_t>> tails; std::set<uint32_t> setup;
    for (const auto& item:std::map<std::string,int>{{"emitShort",2},{"emitStart",3},{"emitStop",1}}) {
        const auto& b=bodies.at(item.first); auto anchor=anchors.at(item.first);
        auto first=std::find_if(b.refs.begin(),b.refs.end(),[&](Ref r){return r.at>anchor && !r.tail;});
        if (first==b.refs.end()) throw std::runtime_error("Missing integer list append");
        appends.push_back(first->target); int count=0; uint32_t last=0;
        for (auto r:b.refs) if (r.target==first->target && !r.tail) {++count;last=r.at;}
        if (count!=item.second || !b.loads.count(anchor+7)) throw std::runtime_error("Message list shape mismatch");
        offsets.push_back(b.loads.at(anchor+7)); std::set<uint32_t> tail;
        for (auto r:b.refs) { if (r.at>last) tail.insert(r.target); if (r.at<anchor) setup.insert(r.target); }
        tails.push_back(std::move(tail));
    }
    if (!std::all_of(appends.begin(),appends.end(),[&](uint32_t x){return x==appends[0];}) || !std::all_of(offsets.begin(),offsets.end(),[&](int x){return x==offsets[0];})) throw std::runtime_error("Messages do not share the same integer list");
    std::vector<uint32_t> senders;
    for (auto target:tails[0]) if (!setup.count(target) && tails[1].count(target) && tails[2].count(target)) senders.push_back(target);
    Result out; out.sender=only(senders,"common sender"); out.shortPlay=bodies.at("shortPlay").start; out.emitShort=bodies.at("emitShort").start;
    std::map<uint32_t,Body> candidates;
    for (auto hit:hits.at("component")) {
        auto b=reader(hit,{out.emitShort,bodies.at("emitStart").start,bodies.at("emitStop").start});
        if (!refs(b,out.emitShort) && !refs(b,bodies.at("emitStart").start) && !refs(b,bodies.at("emitStop").start)) continue;
        if (!b.instructions.count(hit)) throw std::runtime_error("Unaligned component signature");
        candidates[b.start]=std::move(b);
    }
    std::vector<uint32_t> plays,stops;
    for (const auto& c:candidates) {
        const auto& b=c.second;
        if (b.highBit && refs(b,bodies.at("emitStart").start)) plays.push_back(b.start);
        if (b.highBit && refs(b,bodies.at("emitStop").start)) stops.push_back(b.start);
        if (refs(b,out.emitShort) && refs(b,out.shortPlay)) out.shortUi.push_back(b.start);
    }
    out.gamePlay=only(plays,"sustained play"); out.gameStop=only(stops,"sustained stop");
    if (out.shortUi.empty()) throw std::runtime_error("Missing short-note UI callback");
    bool old=offsets[0]==0x28 && read(out.gamePlay).savedArgs==std::set<int>{1,2,8} && read(out.gameStop).savedArgs==std::set<int>{1,2};
    bool current=offsets[0]==0x20 && read(out.gamePlay).savedArgs==std::set<int>{1,2,8,9} && read(out.gameStop).savedArgs==std::set<int>{1,2,8};
    for (const auto& name:{"emitShort","emitStart","emitStop"}) {
        const auto& b=bodies.at(name); uint32_t at=anchors.at(name);
        std::vector<Store> command,entity,flags;
        for (auto s:b.stores) if (s.at==at && s.offset==0x30 && s.immediate) command.push_back(s);
        auto cmd=only(command,"command store");
        for (auto s:b.stores) if (s.base==cmd.base) {
            if (s.at<at && !s.immediate) entity.push_back(s);
            if (s.at>at && s.immediate && s.value==1) flags.push_back(s);
        }
        auto e=only(entity,"entity store");
        old=old && e.offset==0x34 && flags.empty();
        current=current && e.offset==0x38 && flags.size()==1 && flags[0].offset==0x34;
    }
    if (old==current) throw std::runtime_error("Unknown or ambiguous instrument ABI");
    out.extraFlag=current;
    std::vector<uint32_t> pumps;
    for (auto hit:hits.at("mainThread")) {
        auto b=read(hit); if (b.start!=hit || b.refs.size()!=2) continue;
        auto call=b.refs[0],tail=b.refs[1];
        if (call.tail || !tail.tail || call.at!=hit+4 || tail.at!=hit+38) continue;
        auto exec=read(tail.target);
        bool queue=false; for (auto l:exec.loads) if (l.second==0x18) queue=true;
        if (exec.savedArgs==std::set<int>{1} && queue && std::find(exec.indirectOffsets.begin(),exec.indirectOffsets.end(),0x18)!=exec.indirectOffsets.end()) pumps.push_back(hit);
    }
    out.mainThread=only(pumps,"UnitySynchronizationContext.ExecuteTasks"); return out;
}
}
