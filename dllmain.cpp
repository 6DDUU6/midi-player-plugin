#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "src/App.h"
#include "src/Panel.h"

static DWORD WINAPI InitializeHooks(LPVOID) {app::StartGameHooks();return 0;}
static DWORD WINAPI Main(LPVOID module) {
    app::Initialize(false);
    HANDLE worker=CreateThread(nullptr,0,InitializeHooks,nullptr,0,nullptr);
    if (worker) CloseHandle(worker);
    return DWORD(panel::Run(static_cast<HINSTANCE>(module),false));
}
extern "C" __declspec(dllexport) void OpenMidiPlayer() {panel::Show();}
extern "C" __declspec(dllexport) void StopMidiPlayer() {app::Command c;c.action=app::Action::Stop;app::Send(c);}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if (reason==DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        HANDLE thread=CreateThread(nullptr,0,Main,module,0,nullptr);
        if (!thread) return FALSE;CloseHandle(thread);
    }
    // Resident for process lifetime. No waits or hook removal while holding the loader lock.
    return TRUE;
}
