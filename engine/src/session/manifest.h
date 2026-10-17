// Session manifest (manifest.json): which build, settings and disc produced a session (docs/PLAYTEST.md "Sessions").
#pragma once
#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace scd {

struct Manifest {
    std::string app;                  // "snatcher" or "scd_headless"
    std::string build;                // "translated" or "interpreter" (System::translated())
    std::string os, sdl, renderer;    // sdl / renderer "" are written as null (headless)
    std::vector<std::string> args;    // command line without the disc path
    double cd_speed = 1.0;
    bool justifier = false;
    int ckpt_every = 300;             // run.py --session replays with the same interval
    std::string disc_sha1, bram_sha1 = "none";
    std::string started, ended;       // iso8601_local(); ended "" is written as null (session still open, or the app died)
    bool has_frames = false;          // false: "frames": null
    uint64_t frames = 0;
    std::string exit;                 // "quit", "disc_changed", "halted"; "" is written as null
    int markers = 0, failures = 0;
};

// JSON, one key per line, with "format": 1 and git_commit / git_dirty / build_type from the build stamp.
std::string manifest_json(const Manifest& m);
std::string json_escape(const std::string& s);   // contents of a JSON string literal
std::string iso8601_local(std::time_t t);        // 2026-10-16T20:14:03-07:00
std::string host_os();                           // "macOS 27.2", "Linux 6.8.0-40-generic", "Windows"
const char* build_git_commit();                  // short hash, or "unknown" outside a git checkout
bool build_git_dirty();

}  // namespace scd
