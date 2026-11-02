@echo off
setlocal
cd /d "%~dp0"
if not defined VSCMD_ARG_TGT_ARCH if not defined VCINSTALLDIR (
    if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" (
        for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvarsall.bat" amd64
    )
    if not defined VCINSTALLDIR call "%ProgramFiles(x86)%\Microsoft Visual Studio 14.0\VC\vcvarsall.bat" amd64
)
where cl.exe >nul 2>nul
if errorlevel 1 exit /b 1
if not exist build mkdir build
if not exist build\mh mkdir build\mh
if not exist build\imgui mkdir build\imgui
if not exist dist mkdir dist
cl.exe /nologo /c /O2 /MT /W3 /Ivendor\MinHook\include /Ivendor\MinHook\src /Ivendor\MinHook\src\hde vendor\MinHook\src\buffer.c vendor\MinHook\src\hook.c vendor\MinHook\src\trampoline.c vendor\MinHook\src\hde\hde64.c /Fobuild\mh\
if errorlevel 1 exit /b 1
lib.exe /nologo /OUT:build\MinHook.x64.lib build\mh\*.obj
if errorlevel 1 exit /b 1
set COMMON=/nologo /EHsc /O2 /MT /W4 /utf-8 /DUNICODE /D_UNICODE /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DIMGUI_DISABLE_DEFAULT_SHELL_FUNCTIONS /I. /Ivendor\ImGui
cl.exe %COMMON% /c vendor\ImGui\imgui.cpp vendor\ImGui\imgui_draw.cpp vendor\ImGui\imgui_tables.cpp vendor\ImGui\imgui_widgets.cpp vendor\ImGui\imgui_impl_win32.cpp vendor\ImGui\imgui_impl_dx11.cpp /Fobuild\imgui\
if errorlevel 1 exit /b 1
lib.exe /nologo /OUT:build\ImGui.x64.lib build\imgui\*.obj
if errorlevel 1 exit /b 1
set SOURCES=src\Midi.cpp src\Player.cpp src\Locator.cpp src\Discover.cpp src\GameBridge.cpp src\App.cpp src\ImGuiHost.cpp src\InstrumentConfig.cpp src\Timeline.cpp src\Performance.cpp src\Panel.cpp
set LIBS=build\MinHook.x64.lib build\ImGui.x64.lib user32.lib kernel32.lib comdlg32.lib gdi32.lib imm32.lib d3d11.lib dxgi.lib d3dcompiler.lib dwmapi.lib
cl.exe %COMMON% /LD dllmain.cpp %SOURCES% /Fobuild\ /Fedist\MidiPlayerPlugin.dll /link /MACHINE:X64 %LIBS%
if errorlevel 1 exit /b 1
cl.exe %COMMON% preview.cpp %SOURCES% /Fobuild\ /Fedist\MidiPlayerPreview.exe /link /SUBSYSTEM:WINDOWS /MACHINE:X64 %LIBS%
if errorlevel 1 exit /b 1
echo Build OK: dist\MidiPlayerPlugin.dll and dist\MidiPlayerPreview.exe
exit /b 0
