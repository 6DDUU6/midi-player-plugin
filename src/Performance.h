#pragma once
#include "App.h"
#include <array>
namespace performance {
class View {
public:
    void Load(const std::wstring& ini);
    void Release(bool force=false);
    void Draw(const app::Status& status,float height,bool focused);
private:
    player::TrackConfig config;
    char instrument[96]{};
    std::wstring ini;
    std::string error;
    bool keyboardEnabled=true;
    std::array<bool,21> held{},keyboard{};
    int mouseSlot=-1;
    int Key(int slot) const;
    bool Save();
};
}
