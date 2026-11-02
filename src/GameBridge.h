#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "Locator.h"
#include "Player.h"
#include <functional>

namespace game {
struct Calibration {
    bool ready=false,shortReady=false;
    DWORD threadId=0;
    int lastInstrument=0,lastNoteKey=0;
    bool extraFlag=false;
    std::string status;
};
struct ManualNote {
    int instrument=0,noteKey=0;
    bool sustained=false,fromPlugin=false,hasExtraFlag=false;
    uint8_t flag=0,synchronize=0,extraFlag=0;
    void* owner=nullptr;
    void* position=nullptr;
    void* method=nullptr;
    SYSTEMTIME localTime{};
};
std::string FormatManualNote(const ManualNote& note);
class Bridge:public player::Backend {
public:
    bool Initialize(const std::function<void(double)>& tick,const std::function<void(const std::string&)>& progress);
    // Dependency injection for native tests; production calls Discover instead.
    bool Install(const locator::Result& result,uintptr_t base,const std::function<void(double)>& tick,std::string& error);
    void Shutdown();
    Calibration Status() const;
    void SetManualLogging(bool enabled);
    std::vector<ManualNote> ManualNotes() const;
    bool ManualShort(int instrument,int key,std::string& error);
    player::StartResult ManualStart(int instrument,int key,uint32_t& handle,std::string& error);
    bool Short(int instrument,int key,std::string& error) override;
    player::StartResult Start(int instrument,int key,uint32_t& handle,std::string& error) override;
    bool Stop(uint32_t handle,std::string& error) override;
};
double NowMs();
}
