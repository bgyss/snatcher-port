// Registry entries for translated programs and the per-range code checks that pick one (ADR-002, ADR-003).
// Header-only so engine/tests/translate_entry_test.cpp can exercise program selection without a System.
#pragma once
#include <cstdint>
#include <vector>

#include "cpu68k.h"

namespace scd {

struct TEntry {
    bool (*has)(uint32_t);
    lift::Exit (*run)(lift::Cpu&, uint32_t, int32_t&);
    const lift::Span* ranges;
    int nranges;
    uint32_t crc;
    bool overlay;                 // re-checked whenever the caller's generation changes (disc loads, $FFE100 writes)
    uint32_t mask = 0xFFFFFF;     // CPU address -> offset into the memory array checked (Main RAM: 0xFFFF)
    uint32_t checked_gen = 0xFFFFFFFF;
    std::vector<uint8_t> state;   // per range: 0 unchecked, 1 ok, 2 differs
    int last = 0;
};

inline uint32_t crc_range(const uint8_t* mem, uint32_t lo, uint32_t hi) {
    uint32_t crc = 0xFFFFFFFF;
    for (uint32_t a = lo; a < hi; ++a) {
        crc ^= mem[a];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

// True when the code range containing pc is in memory exactly as translated (checked once per range per generation).
inline bool entry_code_ok(TEntry& e, const uint8_t* mem, uint32_t pc, uint32_t gen) {
    if (e.checked_gen != gen) { e.checked_gen = gen; e.state.assign(size_t(e.nranges), 0); }
    if (!(pc >= e.ranges[e.last].lo && pc < e.ranges[e.last].hi)) {
        int lo = 0, hi = e.nranges - 1, found = -1;
        while (lo <= hi) { int m = (lo + hi) / 2; if (pc < e.ranges[m].lo) hi = m - 1; else if (pc >= e.ranges[m].hi) lo = m + 1; else { found = m; break; } }
        if (found < 0) return false;
        e.last = found;
    }
    uint8_t& st = e.state[size_t(e.last)];
    const lift::Span& r = e.ranges[e.last];
    if (st == 0) st = crc_range(mem, r.lo & e.mask, ((r.hi - 1) & e.mask) + 1) == r.crc ? 1 : 2;
    return st == 1;
}

// First program that claims pc AND whose code range at pc is in memory as translated: two overlays with the same
// image key but different code both claim the pc, and only the one actually loaded matches. `claimed` reports whether
// any program has pc (claimed without a match is a code mismatch). Overlay entries are re-checked when `ovl_gen` changes.
inline TEntry* pick_entry(std::vector<TEntry>& entries, const uint8_t* mem, uint32_t pc, uint32_t ovl_gen, bool& claimed) {
    claimed = false;
    for (auto& e : entries) {
        if (!e.has(pc)) continue;
        claimed = true;
        if (entry_code_ok(e, mem, pc, e.overlay ? ovl_gen : 0)) return &e;
    }
    return nullptr;
}

// What the translated-code dispatch loop does next (ADR-003 M2). Interrupts are taken where Musashi's m68k_execute checks them, at the
// top of each step, except right after an entry (force_one): M1's irq_entry Musashi step ran the handler's first instruction with
// the entry, and the loop keeps that. A stopped CPU idles unless an interrupt is deliverable.
enum class LoopStep { kDeliver, kIdle, kRun };

inline LoopStep loop_step(bool force_one, bool stopped, int ipl, uint32_t sr) {
    if (!force_one && ipl > int((sr >> 8) & 7)) return LoopStep::kDeliver;
    if (stopped) return LoopStep::kIdle;
    return LoopStep::kRun;
}

}  // namespace scd
