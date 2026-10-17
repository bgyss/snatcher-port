// Debug evidence on disk: screenshots, RAM dumps, watchdog failure bundles and user markers. Shared by scd_headless and
// session recording (docs/PLAYTEST.md).
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "replay.h"
#include "system.h"
#include "watchdog.h"

namespace scd {

bool write_file(const std::string& path, const void* data, size_t len);
bool write_text(const std::string& path, const std::string& text);
// P6 image of the w x h visible area of a 320-pixel-stride framebuffer.
bool write_ppm(const std::string& path, const uint32_t* fb, int w, int h);
// Main RAM ($FF0000, 64 KB) and Sub PRG RAM $7000..$D000 (the windows state_hash() covers).
bool write_main_ram(const std::string& path, System& sys);
bool write_sub_ram(const std::string& path, System& sys);
const char* verdict_name(WatchVerdict::Kind k);   // "hang", "halted", "still", "ok"
// A watchdog trip, into the existing directory `dir`: report.json (kind, frame, $E020/$E022/$E06C, both PCs), final.ppm,
// mainram.bin, subram.bin, repro.replay (the input that led there) and state.txt. Logs dump_state() and a [watch] line.
// False if a file could not be written.
bool write_failure_bundle(System& sys, const std::string& dir, const Replay& trace, const WatchVerdict& v);
// A user marker (F8/F10), into the existing directory `dir`: marker.json, screen.ppm, mainram.bin, subram.bin, state.txt.
bool write_marker(System& sys, const std::string& dir, const std::string& time_iso);

}  // namespace scd
