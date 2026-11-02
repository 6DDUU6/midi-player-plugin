#pragma once
#include "GameBridge.h"
#include <memory>
namespace app {
enum class Action { Load, Play, Pause, Stop, Seek, Test, ManualOn, ManualOff, ManualRelease };
struct Command {
    Action action=Action::Stop;
    std::shared_ptr<const player::Plan> plan;
    double value=0; int instrument=1,key=player::DefaultMiddleDo; bool sustained=false;
    int slot=0;
};
struct Status {
    player::Snapshot transport;
    game::Calibration game;
    bool preview=false; std::string message;
    size_t manualVoices=0; std::string manualError;
};
void Initialize(bool preview);
void StartGameHooks();
bool Send(const Command& command);
Status GetStatus();
void SetManualLogging(bool enabled);
std::vector<game::ManualNote> ManualNotes();
void PreviewTick();
void PreviewTick(double nowMs); // Deterministic preview clock for native unit tests.
}
