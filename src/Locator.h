#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace locator {
struct Ref { uint32_t at,target; bool tail; };
struct Store { uint32_t at; int base,offset; bool immediate; uint32_t value; };
struct Body {
    uint32_t start=0,end=0; std::vector<Ref> refs;
    std::map<uint32_t,int> loads; std::set<uint32_t> instructions;
    std::vector<Store> stores; std::set<int> savedArgs;
    std::vector<int> indirectOffsets; bool highBit=false;
};
struct Result {
    uint32_t shortPlay=0,emitShort=0,gamePlay=0,gameStop=0,sender=0,mainThread=0;
    std::vector<uint32_t> shortUi;
    bool extraFlag=false;
};
using Hits=std::map<std::string,std::vector<uint32_t>>;
using Reader=std::function<Body(uint32_t,const std::vector<uint32_t>&)>;
const std::map<std::string,std::string>& Patterns();
std::vector<int> Pattern(const std::string& text);
std::vector<uint32_t> Scan(const uint8_t* bytes,size_t size,uint32_t rva,const std::vector<int>& pattern);
Body Decode(const uint8_t* bytes,size_t size,uint32_t startRva);
bool ContainsReference(const uint8_t* bytes,size_t size,uint32_t startRva,const std::vector<uint32_t>& targets);
Result Resolve(const Hits& hits,const Reader& read);
// Scan committed executable memory in the main PE, then validate exact unwind boundaries and ABI.
Result Discover(const std::function<void(const std::string&)>& progress);
}
