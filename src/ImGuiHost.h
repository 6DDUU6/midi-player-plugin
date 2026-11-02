#pragma once
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <string>
#include "../vendor/ImGui/imgui.h"

namespace ui {
bool Initialize(HWND window);
void Shutdown();
void Resize(unsigned width,unsigned height);
bool NewFrame();
bool Render(const std::wstring& snapshot=L"");
float Scale(float value);
std::string Utf8(const std::wstring& text);
std::wstring Widen(const std::string& text);
std::string Time(double ms,bool milliseconds=false);
void Heading(const char* text);
void Muted(const char* text);
// Synthetic input is enabled only by the preview's --smoke run.
void EnableTestInput(bool enabled);
void Mark(const char* name);
void MarkRect(const char* name,const ImVec2& min,const ImVec2& max);
bool TestMouse(const char* name,bool down,float fraction=.5f,float vertical=.5f);
bool TestWheel(const char* name,bool control,float delta);
}
