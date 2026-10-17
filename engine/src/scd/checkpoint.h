// Playtest checkpoints: one line on every $E022/$E06C change and every N frames, in the scd_headless --ckpt-out format
// (docs/PLAYTEST.md). Shared by scd_headless and session recording so the format cannot drift.
#pragma once
#include <cstdint>
#include <string>

#include "system.h"

namespace scd {

// "@0x<frame> scene=E022:<e022>/E06C:<e06c> vram=<16 hex> ram=<16 hex> fb=<16 hex>\n"
std::string checkpoint_line(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h);

class Checkpointer {
public:
    explicit Checkpointer(int every = 300) : every_(every) {}
    // Once after every run_frame(): true when $E022/$E06C changed since the previous call, or frame is a multiple of every (> 0).
    bool due(uint64_t frame, uint16_t e022, uint16_t e06c);
    // The line for this frame, or "" when one was already emitted for it.
    std::string emit(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h);
    // Front-end helpers. after_frame: due() then emit() (hashing only when a line is written). final_line: emit() at exit
    // or before a failure bundle.
    std::string after_frame(System& sys);
    std::string final_line(System& sys);

private:
    int every_;
    uint16_t last22_ = 0xFFFF, last6c_ = 0xFFFF;
    uint64_t last_emitted_ = ~0ull;
};

}  // namespace scd
