#pragma once
#include "Player.h"
#include <string>
#include <vector>
namespace instrumentui {
std::wstring InstrumentLabel(int id);
bool DefaultSustained(int id);
int PresetCount();
int PresetId(int index);
void SetInstrumentText(char* text,size_t size,int id);
bool ReadInstrument(const char* text,int& id);
bool InstrumentInput(player::TrackConfig& config,char* text,size_t size);
const char* NoteMode(const player::TrackConfig& config);
bool SustainedInput(player::TrackConfig& config);
bool Validate(player::TrackConfig& config,const char* instrument,std::string& error);
std::string Pitch(int pitch);
std::string MappedKey(const player::TrackConfig& config,int pitch);
struct Editor {
    bool open=false,request=false,pending=false,onlyUsed=false,reveal=false;
    player::TrackConfig draft;
    char instrument[96]{};
    int source=48,target=60,counts[128]{};
    std::string title,error;
    void Open(const midi::Track& track,const std::string& trackTitle,const player::TrackConfig& config);
    void SelectPitch(int pitch);
    bool SetMapping();
    bool Accept(player::TrackConfig& result);
    bool Draw(player::TrackConfig& result);
};
struct NotesViewer {
    bool open=false,focus=false,onlyCustom=false;
    int pitch=-1;
    std::string title;
    std::shared_ptr<const midi::Song> song;
    size_t track=0;
    player::TrackConfig config;
    std::vector<size_t> order,filtered;
    void Open(std::shared_ptr<const midi::Song> source,size_t index,const std::string& trackTitle,const player::TrackConfig& settings);
    void Filter();
    std::string Cell(size_t row,int column) const;
    void Draw();
};
}
