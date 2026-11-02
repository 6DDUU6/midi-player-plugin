#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace midi {
struct Note {
    uint64_t startTick = 0, endTick = 0;
    double startMs = 0, endMs = 0;
    int pitch = 0, velocity = 0, channel = 0;
};
struct Track {
    std::string name;
    std::vector<Note> notes;
    uint16_t channels = 0;
    std::vector<int> programs;
    uint64_t endTick = 0;
};
struct Song {
    int format = 0, division = 0;
    double durationMs = 0;
    std::vector<Track> tracks;
    std::vector<std::string> warnings;
};
// SMF 0/1; PPQ tempo maps and SMPTE timing. Throws std::runtime_error on malformed input.
Song Parse(const std::vector<uint8_t>& bytes);
}
