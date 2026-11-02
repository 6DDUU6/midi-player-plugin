#include "ImGuiHost.h"
#include "../vendor/ImGui/imgui_impl_win32.h"
#include "../vendor/ImGui/imgui_impl_dx11.h"
#include <d3d11.h>
#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <vector>

namespace ui {
namespace {
HWND window=nullptr;
ID3D11Device* device=nullptr;
ID3D11DeviceContext* context=nullptr;
IDXGISwapChain* swap=nullptr;
ID3D11RenderTargetView* target=nullptr;
unsigned resizeWidth=0,resizeHeight=0;
float dpi=1;
bool win32Ready=false,dxReady=false;
bool testInput=false;
struct TestTarget {ImVec2 min,max;};
std::map<std::string,TestTarget> testTargets;
template<class T> void Release(T*& item) {if (item) {item->Release();item=nullptr;}}
bool CreateTarget() {
    ID3D11Texture2D* buffer=nullptr;
    if (FAILED(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&buffer)))) return false;
    HRESULT hr=device->CreateRenderTargetView(buffer,nullptr,&target);buffer->Release();return SUCCEEDED(hr);
}
void Theme() {
    ImGui::StyleColorsDark();auto& style=ImGui::GetStyle();
    style.WindowPadding=ImVec2(16,14);style.FramePadding=ImVec2(9,6);
    style.ItemSpacing=ImVec2(10,9);style.ItemInnerSpacing=ImVec2(7,5);
    style.CellPadding=ImVec2(9,7);style.ScrollbarSize=12;
    style.WindowRounding=4;style.ChildRounding=4;style.FrameRounding=3;
    style.PopupRounding=4;style.GrabRounding=3;style.TabRounding=3;
    style.WindowBorderSize=1;style.ChildBorderSize=1;style.FrameBorderSize=1;
    auto c=style.Colors;
    c[ImGuiCol_Text]=ImVec4(.88f,.90f,.94f,1);c[ImGuiCol_TextDisabled]=ImVec4(.46f,.51f,.60f,1);
    c[ImGuiCol_WindowBg]=ImVec4(.075f,.086f,.11f,1);c[ImGuiCol_ChildBg]=ImVec4(.095f,.108f,.135f,1);
    c[ImGuiCol_PopupBg]=ImVec4(.105f,.12f,.15f,.99f);c[ImGuiCol_Border]=ImVec4(.19f,.22f,.28f,1);
    c[ImGuiCol_FrameBg]=ImVec4(.13f,.15f,.19f,1);c[ImGuiCol_FrameBgHovered]=ImVec4(.18f,.22f,.28f,1);
    c[ImGuiCol_FrameBgActive]=ImVec4(.19f,.27f,.36f,1);
    c[ImGuiCol_TitleBg]=ImVec4(.10f,.13f,.18f,1);c[ImGuiCol_TitleBgActive]=ImVec4(.14f,.21f,.30f,1);
    c[ImGuiCol_Button]=ImVec4(.16f,.20f,.26f,1);c[ImGuiCol_ButtonHovered]=ImVec4(.22f,.32f,.43f,1);
    c[ImGuiCol_ButtonActive]=ImVec4(.17f,.39f,.60f,1);
    c[ImGuiCol_Header]=ImVec4(.15f,.29f,.43f,.85f);c[ImGuiCol_HeaderHovered]=ImVec4(.19f,.36f,.53f,.9f);
    c[ImGuiCol_HeaderActive]=ImVec4(.21f,.40f,.60f,1);
    c[ImGuiCol_CheckMark]=ImVec4(.35f,.70f,1,1);c[ImGuiCol_SliderGrab]=ImVec4(.27f,.59f,.88f,1);
    c[ImGuiCol_SliderGrabActive]=ImVec4(.45f,.76f,1,1);
    c[ImGuiCol_TableHeaderBg]=ImVec4(.12f,.15f,.20f,1);
    c[ImGuiCol_TableBorderStrong]=ImVec4(.22f,.25f,.31f,1);c[ImGuiCol_TableBorderLight]=ImVec4(.15f,.18f,.23f,1);
    c[ImGuiCol_TableRowBg]=ImVec4(0,0,0,0);c[ImGuiCol_TableRowBgAlt]=ImVec4(.22f,.28f,.37f,.13f);
    c[ImGuiCol_Separator]=c[ImGuiCol_Border];
    style.ScaleAllSizes(dpi);style.FontScaleDpi=dpi;
}
bool Capture(const std::wstring& path) {
    ID3D11Texture2D *source=nullptr,*staging=nullptr;
    if (FAILED(swap->GetBuffer(0,__uuidof(ID3D11Texture2D),reinterpret_cast<void**>(&source)))) return false;
    D3D11_TEXTURE2D_DESC desc{};source->GetDesc(&desc);
    desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;desc.MiscFlags=0;
    HRESULT hr=device->CreateTexture2D(&desc,nullptr,&staging);
    if (FAILED(hr)) {Release(source);return false;}
    context->CopyResource(staging,source);D3D11_MAPPED_SUBRESOURCE pixels{};
    hr=context->Map(staging,0,D3D11_MAP_READ,0,&pixels);
    if (FAILED(hr)) {Release(source);Release(staging);return false;}
    BITMAPFILEHEADER file{};BITMAPINFOHEADER info{};
    info.biSize=sizeof(info);info.biWidth=LONG(desc.Width);info.biHeight=-LONG(desc.Height);
    info.biPlanes=1;info.biBitCount=32;info.biCompression=BI_RGB;
    file.bfType=0x4d42;file.bfOffBits=sizeof(file)+sizeof(info);file.bfSize=file.bfOffBits+desc.Width*desc.Height*4;
    std::ofstream out(path,std::ios::binary);out.write(reinterpret_cast<char*>(&file),sizeof(file));
    out.write(reinterpret_cast<char*>(&info),sizeof(info));std::vector<unsigned char> row(desc.Width*4);
    for (unsigned y=0;y<desc.Height;++y) {
        auto input=static_cast<const unsigned char*>(pixels.pData)+y*pixels.RowPitch;
        for (unsigned x=0;x<desc.Width;++x) {row[x*4]=input[x*4+2];row[x*4+1]=input[x*4+1];row[x*4+2]=input[x*4];row[x*4+3]=255;}
        out.write(reinterpret_cast<char*>(row.data()),row.size());
    }
    context->Unmap(staging,0);Release(source);Release(staging);return bool(out);
}
}
bool Initialize(HWND hwnd) {
    window=hwnd;DXGI_SWAP_CHAIN_DESC desc{};desc.BufferCount=2;
    desc.BufferDesc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.OutputWindow=hwnd;desc.SampleDesc.Count=1;desc.Windowed=TRUE;desc.SwapEffect=DXGI_SWAP_EFFECT_DISCARD;
    D3D_FEATURE_LEVEL levels[]={D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0},level;
    HRESULT hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
    if (FAILED(hr)) {
        Release(swap);Release(context);Release(device);
        hr=D3D11CreateDeviceAndSwapChain(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,levels,2,D3D11_SDK_VERSION,&desc,&swap,&device,&level,&context);
    }
    if (FAILED(hr) || !CreateTarget()) {Shutdown();return false;}
    IMGUI_CHECKVERSION();ImGui::CreateContext();auto& io=ImGui::GetIO();
    io.ConfigFlags|=ImGuiConfigFlags_NavEnableKeyboard;io.IniFilename=nullptr;io.LogFilename=nullptr;
    wchar_t windows[32768]{};GetWindowsDirectoryW(windows,32768);
    std::wstring fonts=std::wstring(windows)+L"\\Fonts\\";
    const wchar_t* candidates[]={L"msyh.ttc",L"simhei.ttf",L"simsun.ttc"};
    bool fontLoaded=false;
    for (auto name:candidates) {auto path=fonts+name;if (GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES) {
        fontLoaded=io.Fonts->AddFontFromFileTTF(Utf8(path).c_str(),17)!=nullptr;if (fontLoaded) break;
    }}
    if (!fontLoaded) {Shutdown();return false;}
    dpi=std::max(1.f,ImGui_ImplWin32_GetDpiScaleForHwnd(hwnd));Theme();
    win32Ready=ImGui_ImplWin32_Init(hwnd);
    if (win32Ready) dxReady=ImGui_ImplDX11_Init(device,context);
    if (!dxReady) {Shutdown();return false;}
    return true;
}
void Shutdown() {
    if (dxReady) ImGui_ImplDX11_Shutdown();if (win32Ready) ImGui_ImplWin32_Shutdown();dxReady=win32Ready=false;
    if (ImGui::GetCurrentContext()) ImGui::DestroyContext();
    Release(target);Release(swap);Release(context);Release(device);window=nullptr;
}
void Resize(unsigned width,unsigned height) {resizeWidth=width;resizeHeight=height;}
bool NewFrame() {
    if (testInput) testTargets.clear();
    if (resizeWidth && resizeHeight) {
        context->OMSetRenderTargets(0,nullptr,nullptr);Release(target);
        if (FAILED(swap->ResizeBuffers(0,resizeWidth,resizeHeight,DXGI_FORMAT_UNKNOWN,0)) || !CreateTarget()) return false;
        resizeWidth=resizeHeight=0;
    }
    float newDpi=std::max(1.f,ImGui_ImplWin32_GetDpiScaleForHwnd(window));
    if (dpi!=newDpi) {dpi=newDpi;ImGui::GetStyle()=ImGuiStyle();Theme();}
    ImGui_ImplDX11_NewFrame();ImGui_ImplWin32_NewFrame();ImGui::NewFrame();return true;
}
bool Render(const std::wstring& snapshot) {
    ImGui::Render();const float color[]={.075f,.086f,.11f,1};
    context->OMSetRenderTargets(1,&target,nullptr);context->ClearRenderTargetView(target,color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    if (!snapshot.empty() && !Capture(snapshot)) return false;
    HRESULT hr=swap->Present(1,0);return SUCCEEDED(hr);
}
float Scale(float value) {return value*dpi;}
std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};int size=WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),nullptr,0,nullptr,nullptr);
    std::string out(size,0);WideCharToMultiByte(CP_UTF8,0,text.data(),int(text.size()),&out[0],size,nullptr,nullptr);return out;
}
std::wstring Widen(const std::string& text) {
    if (text.empty()) return {};UINT cp=CP_UTF8;int count=MultiByteToWideChar(cp,MB_ERR_INVALID_CHARS,text.data(),int(text.size()),nullptr,0);
    if (!count) {cp=936;count=MultiByteToWideChar(cp,0,text.data(),int(text.size()),nullptr,0);}
    if (!count) return L"?";std::wstring out(count,0);MultiByteToWideChar(cp,0,text.data(),int(text.size()),&out[0],count);return out;
}
std::string Time(double ms,bool milliseconds) {
    auto value=static_cast<int64_t>(std::max(0.,ms)+.5);std::ostringstream out;
    out<<value/60000<<":"<<std::setfill('0')<<std::setw(2)<<(value/1000)%60;
    if (milliseconds) out<<"."<<std::setw(3)<<value%1000;return out.str();
}
void Heading(const char* text) {ImGui::TextColored(ImVec4(.43f,.73f,1,1),"%s",text);ImGui::Separator();}
void Muted(const char* text) {ImGui::PushStyleColor(ImGuiCol_Text,ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));ImGui::TextWrapped("%s",text);ImGui::PopStyleColor();}
void EnableTestInput(bool enabled) {testInput=enabled;testTargets.clear();}
void Mark(const char* name) {
    if (!testInput) return;auto a=ImGui::GetItemRectMin(),b=ImGui::GetItemRectMax();
    testTargets[name]={a,b};
}
bool TestMouse(const char* name,bool down,float fraction) {
    if (!testInput) return false;auto item=testTargets.find(name);if (item==testTargets.end()) return false;
    const auto& bounds=item->second;auto& io=ImGui::GetIO();
    io.AddMousePosEvent(bounds.min.x+(bounds.max.x-bounds.min.x)*fraction,(bounds.min.y+bounds.max.y)*.5f);io.AddMouseButtonEvent(0,down);return true;
}
}
