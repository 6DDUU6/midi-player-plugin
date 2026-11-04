#include "Locator.h"
#include "../vendor/MinHook/src/hde/hde64.h"
#include <algorithm>
#include <cstring>
#include <sstream>
#include <stdexcept>

namespace locator {
const std::map<std::string,std::string>& Patterns() {
    static const std::map<std::string,std::string> p={
        {"emitShort","C7 ?? ?? 05 00 00 00 48 8B ?? ?? 48 85 C9"},
        {"emitStart","C7 ?? ?? 06 00 00 00 48 8B ?? ?? 48 85 C9"},
        {"emitStop","C7 ?? ?? 07 00 00 00 48 8B ?? ?? 48 85 C9"},
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
struct MessageLayout {int command,list,entity,flag;};
struct Message {
    Body body;uint32_t anchor,append,lastAppend;const MessageLayout* layout;
};
bool MatchMessage(Body body,uint32_t anchor,uint32_t value,int count,Message& message) {
    if (!body.instructions.count(anchor) || !body.loads.count(anchor+7)) return false;
    std::vector<Store> commands,entities,flags;
    for (auto s:body.stores) if (s.at==anchor && s.immediate && s.value==value) commands.push_back(s);
    if (commands.size()!=1) return false;const auto& command=commands[0];
    for (auto s:body.stores) if (s.base==command.base) {
        if (s.at<anchor && !s.immediate) entities.push_back(s);
        if (s.at>anchor && s.immediate && s.value==1) flags.push_back(s);
    }
    if (entities.size()!=1) return false;
    // Both supported releases use the extra-flag ABI with different field orders.
    static const MessageLayout layouts[]={
        {0x30,0x20,0x38,0x34},
        {0x38,0x28,0x34,0x30}
    };
    const MessageLayout* layout=nullptr;
    for (const auto& candidate:layouts) if (command.offset==candidate.command && body.loads.at(anchor+7)==candidate.list && entities[0].offset==candidate.entity &&
        flags.size()==1 && flags[0].offset==candidate.flag) {
        if (layout) return false;layout=&candidate;
    }
    if (!layout) return false;
    auto first=std::find_if(body.refs.begin(),body.refs.end(),[&](Ref r){return r.at>anchor && !r.tail;});
    if (first==body.refs.end()) return false;
    uint32_t append=first->target,last=0;int found=0;
    for (auto r:body.refs) if (r.target==append && !r.tail) {++found;last=r.at;}
    if (found!=count) return false;
    message={std::move(body),anchor,append,last,layout};return true;
}
}
Result Resolve(const Hits& hits,const Reader& reader) {
    auto read=[&](uint32_t rva) {return reader(rva,{});};
    std::map<std::string,Message> messages;
    for (const auto& role:std::map<std::string,std::pair<uint32_t,int>>{{"emitShort",{5,2}},{"emitStart",{6,3}},{"emitStop",{7,1}}}) {
        std::vector<Message> candidates;
        for (auto hit:hits.at(role.first)) {
            try {
                Message message;
                if (MatchMessage(read(hit),hit,role.second.first,role.second.second,message)) candidates.push_back(std::move(message));
            }catch (const std::runtime_error&) {
                // Broad signatures can hit unrelated code with unsupported unwind/decode shapes.
            }
        }
        messages[role.first]=only(candidates,role.first);
    }
    auto shortAnchor=only(hits.at("shortPlay"),"shortPlay");auto shortBody=read(shortAnchor);
    if (!shortBody.instructions.count(shortAnchor)) throw std::runtime_error("shortPlay: signature is not instruction-aligned");
    const auto& firstMessage=messages.at("emitShort");
    std::vector<std::set<uint32_t>> tails; std::set<uint32_t> setup;
    for (const auto& item:messages) {
        const auto& message=item.second;
        if (message.append!=firstMessage.append || message.layout!=firstMessage.layout) throw std::runtime_error("Messages do not share the same integer list and field layout");
        std::set<uint32_t> tail;
        for (auto r:message.body.refs) {if (r.at>message.lastAppend) tail.insert(r.target);if (r.at<message.anchor) setup.insert(r.target);}
        tails.push_back(std::move(tail));
    }
    std::vector<uint32_t> senders;
    for (auto target:tails[0]) if (!setup.count(target) && tails[1].count(target) && tails[2].count(target)) senders.push_back(target);
    Result out;out.sender=only(senders,"common sender");out.shortPlay=shortBody.start;out.emitShort=firstMessage.body.start;
    auto emitStart=messages.at("emitStart").body.start,emitStop=messages.at("emitStop").body.start;
    std::map<uint32_t,Body> candidates;
    for (auto hit:hits.at("component")) {
        auto b=reader(hit,{out.emitShort,emitStart,emitStop});
        if (!refs(b,out.emitShort) && !refs(b,emitStart) && !refs(b,emitStop)) continue;
        if (!b.instructions.count(hit)) throw std::runtime_error("Unaligned component signature");
        candidates[b.start]=std::move(b);
    }
    std::vector<uint32_t> plays,stops;
    for (const auto& c:candidates) {
        const auto& b=c.second;
        if (b.highBit && refs(b,emitStart)) plays.push_back(b.start);
        if (b.highBit && refs(b,emitStop)) stops.push_back(b.start);
        if (refs(b,out.emitShort) && refs(b,out.shortPlay)) out.shortUi.push_back(b.start);
    }
    out.gamePlay=only(plays,"sustained play"); out.gameStop=only(stops,"sustained stop");
    if (out.shortUi.empty()) throw std::runtime_error("Missing short-note UI callback");
    const std::set<int> playArgs={1,2,8,9},stopArgs={1,2,8};
    if (read(out.gamePlay).savedArgs!=playArgs || read(out.gameStop).savedArgs!=stopArgs) throw std::runtime_error("Unknown or ambiguous instrument ABI");
    out.extraFlag=true;
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
