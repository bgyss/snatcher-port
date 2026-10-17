#include "checkpoint.h"

#include <cstdio>

namespace scd {

std::string checkpoint_line(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h) {
    char line[160];
    std::snprintf(line, sizeof line, "@0x%llx scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n", (unsigned long long)frame,
                  e022, e06c, (unsigned long long)h.vram, (unsigned long long)h.ram, (unsigned long long)h.fb);
    return line;
}

bool Checkpointer::due(uint64_t frame, uint16_t e022, uint16_t e06c) {
    const bool changed = e022 != last22_ || e06c != last6c_;
    last22_ = e022;
    last6c_ = e06c;
    return changed || (every_ > 0 && frame % uint64_t(every_) == 0);
}

std::string Checkpointer::emit(uint64_t frame, uint16_t e022, uint16_t e06c, const StateHash& h) {
    if (frame == last_emitted_) return "";
    last_emitted_ = frame;
    return checkpoint_line(frame, e022, e06c, h);
}

std::string Checkpointer::after_frame(System& sys) {
    const uint64_t f = sys.frame_count();
    if (!due(f, sys.e022(), sys.e06c()) || f == last_emitted_) return "";
    return emit(f, sys.e022(), sys.e06c(), sys.state_hash());
}

std::string Checkpointer::final_line(System& sys) {
    const uint64_t f = sys.frame_count();
    if (f == last_emitted_) return "";
    return emit(f, sys.e022(), sys.e06c(), sys.state_hash());
}

}  // namespace scd
