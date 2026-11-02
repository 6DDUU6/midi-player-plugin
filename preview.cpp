#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include "src/App.h"
#include "src/Panel.h"
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR command,int) {
    app::Initialize(true);return panel::Run(instance,true,wcsstr(command,L"--smoke")!=nullptr,wcsstr(command,L"--snapshot")!=nullptr);
}
