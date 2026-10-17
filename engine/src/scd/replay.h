// Input replay: pad buttons (and, from v2, the Konami Justifier) held from which boot frame (System::frame_count()).
// Text format, no disc data (docs/PLAYTEST.md "Replay format").
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace scd {

struct ReplayEvent { uint64_t frame; uint16_t buttons; };

// Justifier on port 2: x,y in game screen pixels, inside = aimed at the screen, buttons bit0 trigger, bit1 start.
struct GunState {
    int x = 0, y = 0;
    bool inside = false;
    uint8_t buttons = 0;
    bool operator==(const GunState&) const = default;
};
struct GunEvent { uint64_t frame; GunState gun; };

uint16_t buttons_from_string(const std::string& s);
std::string buttons_to_string(uint16_t b);
// "143,87,in,T" <-> GunState. Buttons: T (trigger), S (start), TS, or - for none.
std::string gun_to_string(const GunState& g);
bool gun_from_string(const std::string& s, GunState* out);

struct Replay {
    int version = 2;   // header version parse() read; serialize() always writes v2
    std::string disc_sha1, engine, bram_sha1 = "none";
    double cd_speed = 1.0;
    bool justifier = false;
    std::vector<ReplayEvent> events;    // sorted by frame; buttons are held from `frame` until the next event
    std::vector<GunEvent> gun_events;   // v2, sorted by frame; held until the next event, GunState{} before the first

    void add(uint64_t frame, uint16_t buttons);          // ignores an event that repeats the current state
    void add_gun(uint64_t frame, const GunState& g);     // same, for the gun
    uint16_t buttons_at(uint64_t frame) const;
    GunState gun_at(uint64_t frame) const;
    std::string serialize() const;                       // serialize_header() + serialize_events(0, 0)
    std::string serialize_header() const;
    // Event lines for events[pad_from..] and gun_events[gun_from..], merged by frame (pad first on a tie). Appending
    // the output of successive calls (each from the previous end) reproduces serialize_events(0, 0).
    std::string serialize_events(size_t pad_from, size_t gun_from) const;
    static bool parse(const std::string& text, Replay* out, std::string* err);
};

// Lowercase hex SHA-1 of a file, or "none" when it cannot be opened.
std::string sha1_file(const std::string& path);
// SHA-1 of the first FILE named in a cue sheet (identity of a one-.bin disc; for split layouts, of track 1).
std::string disc_sha1_from_cue(const std::string& cue_path);

}  // namespace scd
