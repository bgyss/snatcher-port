// Pad-input replay: which buttons are held from which boot frame (System::frame_count()). Text format, no disc data.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace scd {

struct ReplayEvent { uint64_t frame; uint16_t buttons; };

uint16_t buttons_from_string(const std::string& s);
std::string buttons_to_string(uint16_t b);

struct Replay {
    std::string disc_sha1, engine, bram_sha1 = "none";
    double cd_speed = 1.0;
    bool justifier = false;
    std::vector<ReplayEvent> events;   // sorted by frame; buttons are held from `frame` until the next event

    void add(uint64_t frame, uint16_t buttons);          // ignores an event that repeats the current state
    uint16_t buttons_at(uint64_t frame) const;
    std::string serialize() const;
    static bool parse(const std::string& text, Replay* out, std::string* err);
};

// Lowercase hex SHA-1 of a file, or "none" when it cannot be opened.
std::string sha1_file(const std::string& path);
// SHA-1 of the first FILE named in a cue sheet (identity of a one-.bin disc; for split layouts, of track 1).
std::string disc_sha1_from_cue(const std::string& cue_path);

}  // namespace scd
