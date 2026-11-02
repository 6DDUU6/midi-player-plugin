#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "Locator.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace locator {
Result Discover(const std::function<void(const std::string&)>& progress) {
    auto base=reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr));
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic!=IMAGE_DOS_SIGNATURE || dos->e_lfanew<=0 || dos->e_lfanew>0x100000) throw std::runtime_error("Invalid main PE header");
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS64*>(base+dos->e_lfanew);
    if (nt->Signature!=IMAGE_NT_SIGNATURE || nt->FileHeader.Machine!=IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic!=IMAGE_NT_OPTIONAL_HDR64_MAGIC) throw std::runtime_error("Requires x64 PE32+");
    uint32_t size=nt->OptionalHeader.SizeOfImage; auto d=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!d.VirtualAddress || d.Size<12 || d.Size%12 || uint64_t(d.VirtualAddress)+d.Size>size) throw std::runtime_error("Missing PE exception directory");
    auto rows=reinterpret_cast<RUNTIME_FUNCTION*>(base+d.VirtualAddress); size_t count=d.Size/12;
    Reader reader=[&](uint32_t rva,const std::vector<uint32_t>& targets) {
        auto it=std::upper_bound(rows,rows+count,rva,[](uint32_t x,const RUNTIME_FUNCTION& row){return x<row.BeginAddress;});
        if (it==rows) throw std::runtime_error("No unwind boundary for candidate"); --it;
        if (rva>=it->EndAddress || it->EndAddress>size) throw std::runtime_error("No exact unwind boundary");
        if (!targets.empty() && !ContainsReference(base+it->BeginAddress,it->EndAddress-it->BeginAddress,it->BeginAddress,targets)) return Body{};
        return Decode(base+it->BeginAddress,it->EndAddress-it->BeginAddress,it->BeginAddress);
    };
    Hits hits;
    // Scan each executable region in 4 MiB chunks; memchr rejects most bytes cheaply.
    for (const auto& item:Patterns()) {
        progress("Scanning "+item.first); auto pattern=Pattern(item.second); auto& output=hits[item.first];
        uintptr_t pos=reinterpret_cast<uintptr_t>(base),end=pos+size;
        while (pos<end) {
            MEMORY_BASIC_INFORMATION m{};
            if (!VirtualQuery(reinterpret_cast<void*>(pos),&m,sizeof(m))) throw std::runtime_error("VirtualQuery failed");
            auto top=std::min(end,reinterpret_cast<uintptr_t>(m.BaseAddress)+m.RegionSize);
            DWORD protect=m.Protect&255;
            bool exec=protect==PAGE_EXECUTE_READ || protect==PAGE_EXECUTE_READWRITE || protect==PAGE_EXECUTE_WRITECOPY;
            if (m.State==MEM_COMMIT && !(m.Protect&(PAGE_GUARD|PAGE_NOACCESS)) && exec) {
                for (auto chunk=pos;chunk<top;) {
                    auto next=std::min(top,chunk+4*1024*1024);
                    auto scanEnd=std::min(top,next+pattern.size()-1);
                    auto matches=Scan(reinterpret_cast<const uint8_t*>(chunk),scanEnd-chunk,uint32_t(chunk-reinterpret_cast<uintptr_t>(base)),pattern);
                    for (auto hit:matches) if (uint64_t(hit)+reinterpret_cast<uintptr_t>(base)<next) output.push_back(hit);
                    if (output.size()>4096) throw std::runtime_error("Signature match limit exceeded");
                    chunk=next; Sleep(1);
                }
            }
            if (top<=pos) throw std::runtime_error("Invalid memory region"); pos=top;
        }
    }
    progress("Validating call graph and ABI"); return Resolve(hits,reader);
}
}
