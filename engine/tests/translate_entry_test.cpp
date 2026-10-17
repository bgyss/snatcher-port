// Program selection for translated code (ADR-003): first CRC match wins, per-range checks re-run after a generation bump.
#undef NDEBUG   // tests rely on assert even in Release builds
#include <cassert>
#include <cstdio>
#include <vector>

#include "translate_entry.h"

using namespace scd;

namespace {

lift::Exit no_run(lift::Cpu&, uint32_t pc, int32_t&) { return lift::Exit{pc, lift::Exit::NotTranslated}; }
bool has_ovl(uint32_t pc) { return (pc >= 0x28000 && pc < 0x28004) || (pc >= 0x29000 && pc < 0x29004); }
bool has_ram(uint32_t pc) { return pc >= 0xFFE100 && pc < 0xFFE104; }
bool has_top(uint32_t pc) { return pc >= 0xFFFFFC; }
bool has_none(uint32_t) { return false; }

uint32_t crc_of(std::vector<uint8_t> bytes) { return crc_range(bytes.data(), 0, uint32_t(bytes.size())); }

}  // namespace

int main() {
    // Two overlays at $28000 with the same first bytes, different code at $29000 (one image key, two programs).
    const uint32_t shared = crc_of({0x4E, 0x71, 0x4E, 0x71});
    const lift::Span ovl_a[] = {{0x28000, 0x28004, shared}, {0x29000, 0x29004, crc_of({0x70, 0x01, 0x4E, 0x75})}};
    const lift::Span ovl_b[] = {{0x28000, 0x28004, shared}, {0x29000, 0x29004, crc_of({0x70, 0x02, 0x4E, 0x75})}};
    std::vector<uint8_t> prg(0x80000, 0);
    const uint8_t head[] = {0x4E, 0x71, 0x4E, 0x71};
    for (int i = 0; i < 4; ++i) prg[0x28000 + i] = head[i];
    auto load = [&](uint8_t v) { prg[0x29000] = 0x70; prg[0x29001] = v; prg[0x29002] = 0x4E; prg[0x29003] = 0x75; };
    std::vector<TEntry> ovls = {
        {has_ovl, no_run, ovl_a, 2, 0, true},
        {has_ovl, no_run, ovl_b, 2, 0, true},
    };
    bool claimed = false;
    {   // overlay B in memory: A claims pc but its range differs, B is picked
        load(0x02);
        TEntry* te = pick_entry(ovls, prg.data(), 0x29000, 1, claimed);
        assert(claimed && te == &ovls[1]);
    }
    {   // overlay A loaded (new generation): A is picked again
        load(0x01);
        TEntry* te = pick_entry(ovls, prg.data(), 0x29000, 2, claimed);
        assert(claimed && te == &ovls[0]);
    }
    {   // neither program matches: claimed, no entry (the census calls this code_mismatch)
        load(0x03);
        TEntry* te = pick_entry(ovls, prg.data(), 0x29000, 3, claimed);
        assert(claimed && te == nullptr);
        // the shared range still matches the first program
        assert(pick_entry(ovls, prg.data(), 0x28000, 3, claimed) == &ovls[0]);
    }
    {   // without a generation bump, the cached per-range result stands
        load(0x02);
        assert(pick_entry(ovls, prg.data(), 0x29000, 3, claimed) == nullptr);
        assert(pick_entry(ovls, prg.data(), 0x29000, 4, claimed) == &ovls[1]);
    }
    {   // pc nobody claims
        TEntry* te = pick_entry(ovls, prg.data(), 0x2A000, 4, claimed);
        assert(!claimed && te == nullptr);
    }
    {   // Main RAM code at $FFE100, checked through a 64 KB array (mask 0xFFFF); overwritten after its check
        std::vector<uint8_t> ram(0x10000, 0);
        const uint8_t code[] = {0x60, 0x00, 0x00, 0x10};
        for (int i = 0; i < 4; ++i) ram[0xE100 + i] = code[i];
        const lift::Span r[] = {{0xFFE100, 0xFFE104, crc_of({0x60, 0x00, 0x00, 0x10})}};
        std::vector<TEntry> mains = {{has_ram, no_run, r, 1, 0, true, 0xFFFF}};
        uint32_t gen = 0;
        assert(pick_entry(mains, ram.data(), 0xFFE100, gen, claimed) == &mains[0]);
        ram[0xE102] = 0x12;   // the code is overwritten; the writer bumps the generation
        ++gen;
        assert(pick_entry(mains, ram.data(), 0xFFE100, gen, claimed) == nullptr && claimed);
        ram[0xE102] = 0x00;
        ++gen;
        assert(pick_entry(mains, ram.data(), 0xFFE100, gen, claimed) == &mains[0]);
    }
    {   // a range ending at the top of the address space maps to the end of the 64 KB array
        std::vector<uint8_t> ram(0x10000, 0);
        ram[0xFFFC] = 0x4E; ram[0xFFFD] = 0x71; ram[0xFFFE] = 0x4E; ram[0xFFFF] = 0x75;
        const lift::Span r[] = {{0xFFFFFC, 0x1000000, crc_of({0x4E, 0x71, 0x4E, 0x75})}};
        std::vector<TEntry> mains = {{has_top, no_run, r, 1, 0, false, 0xFFFF}};
        assert(pick_entry(mains, ram.data(), 0xFFFFFE, 0, claimed) == &mains[0]);
    }
    {   // non-overlay entries ignore the generation (checked once)
        std::vector<TEntry> none = {{has_none, no_run, ovl_a, 2, 0, false}};
        assert(pick_entry(none, prg.data(), 0x28000, 9, claimed) == nullptr && !claimed);
    }
    {   // ADR-003 M2 dispatch loop: deliver above the mask, never twice in a row, idle while stopped
        assert(loop_step(false, false, 6, 0x2500) == LoopStep::kDeliver);   // level 6 above mask 5
        assert(loop_step(false, false, 5, 0x2500) == LoopStep::kRun);       // equal to the mask: blocked
        assert(loop_step(false, false, 0, 0x2000) == LoopStep::kRun);
        assert(loop_step(true, false, 6, 0x2000) == LoopStep::kRun);        // after an entry: the handler's first instruction first
        assert(loop_step(false, true, 2, 0x2300) == LoopStep::kIdle);       // stopped, the pending level is masked
        assert(loop_step(false, true, 4, 0x2300) == LoopStep::kDeliver);    // stopped, an interrupt wakes it
        assert(loop_step(false, false, 4, 0x0300) == LoopStep::kDeliver);   // user mode: the same rule
    }
    std::puts("translate_entry_test: ok");
    return 0;
}
