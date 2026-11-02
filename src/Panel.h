#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
namespace panel {
// Standalone Dear ImGui/DX11 window and message loop on the panel thread.
int Run(HINSTANCE module,bool preview,bool smoke=false,bool snapshot=false);
void Show();
}
