// Differential test: every translated instruction (lift_cases.inc, generated from your disc) against Musashi on random states.
#include <cstdio>
#include <cstring>
#include <vector>
#include "cpu68k.h"
extern "C" {
#include "m68k.h"
int scd_illg_handler(int) { return 0; }
int scd_int_ack(int) { return M68K_INT_ACK_AUTOVECTOR; }
void scd_instruction_hook(unsigned) {}
}

static std::vector<uint8_t> g_mem;          // Musashi's image
struct FlatBus : lift::Bus {
    std::vector<uint8_t> m;
    uint8_t r8(uint32_t a) override { return m[a & 0xFFFFFF]; }
    uint16_t r16(uint32_t a) override { return uint16_t(m[a & 0xFFFFFF] << 8 | m[(a + 1) & 0xFFFFFF]); }
    void w8(uint32_t a, uint8_t v) override { m[a & 0xFFFFFF] = v; }
    void w16(uint32_t a, uint16_t v) override { m[a & 0xFFFFFF] = v >> 8; m[(a + 1) & 0xFFFFFF] = uint8_t(v); }
};
extern "C" {
unsigned m68k_read_memory_8(unsigned a) { return g_mem[a & 0xFFFFFF]; }
unsigned m68k_read_memory_16(unsigned a) { return g_mem[a & 0xFFFFFF] << 8 | g_mem[(a + 1) & 0xFFFFFF]; }
unsigned m68k_read_memory_32(unsigned a) { return m68k_read_memory_16(a) << 16 | m68k_read_memory_16(a + 2); }
unsigned m68k_read_disassembler_8(unsigned a) { return m68k_read_memory_8(a); }
unsigned m68k_read_disassembler_16(unsigned a) { return m68k_read_memory_16(a); }
unsigned m68k_read_disassembler_32(unsigned a) { return m68k_read_memory_32(a); }
void m68k_write_memory_8(unsigned a, unsigned v) { g_mem[a & 0xFFFFFF] = uint8_t(v); }
void m68k_write_memory_16(unsigned a, unsigned v) { m68k_write_memory_8(a, v >> 8); m68k_write_memory_8(a + 1, v); }
void m68k_write_memory_32(unsigned a, unsigned v) { m68k_write_memory_16(a, v >> 16); m68k_write_memory_16(a + 2, v); }
}
#include "lift_cases.inc"

static uint64_t rng_s = 88172645463325252ull;
static uint32_t rnd() { rng_s ^= rng_s << 13; rng_s ^= rng_s >> 7; rng_s ^= rng_s << 17; return uint32_t(rng_s >> 16); }

int main(int argc, char** argv) {
    int trials = argc > 1 ? atoi(argv[1]) : 12;
    m68k_init(); m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    g_mem.assign(1 << 24, 0);
    FlatBus bus; bus.m.assign(1 << 24, 0);
    std::vector<uint32_t> failures_by_case(sizeof(kCases) / sizeof(kCases[0]), 0);
    int fails = 0, checked = 0, skipped = 0;
    for (size_t ci = 0; ci < sizeof(kCases) / sizeof(kCases[0]); ++ci) {
        const Case& k = kCases[ci];
        for (int t = 0; t < trials; ++t) {
            // random memory image (sparse refresh keeps it fast): fill around the places registers may point
            uint32_t seed = rnd();
            lift::Cpu c; c.bus = &bus;
            for (int i = 0; i < 8; ++i) {
                uint32_t v = rnd() ^ (rnd() << 16);
                if (t & 1) v &= (i % 3 == 0) ? 0xFF : 0xFFFFFF;           // sometimes small values
                c.d[i] = v;
                uint32_t av = 0x100000 + (rnd() & 0xFFFFF) * 2 + ((t & 4) ? 1 : 0) * 0;
                if (t & 2) av = (rnd() & 0xFFFFFF) & ~1u;
                c.a[i] = av;
            }
            c.a[7] = 0x800000 + (rnd() & 0xFFF) * 2;
            c.set_ccr(rnd() & 0x1F);
            // memory: deterministic function of seed, set only for touched windows afterwards via full init (cheap enough: 16 MB memset)
            uint64_t s = seed | 1;
            auto fill = [&](uint8_t* m) {
                // fill 64KB windows around each register plus the code and stack
                for (int w = 0; w < 18; ++w) {
                    uint32_t base = w < 8 ? c.a[w] : (w < 16 ? c.d[w - 8] : (w == 16 ? k.pc : c.a[7]));
                    base = (base - 0x4000) & 0xFFFFFF;
                    for (uint32_t i = 0; i < 0x8000; ++i) { s ^= s << 13; s ^= s >> 7; s ^= s << 17; m[(base + i) & 0xFFFFFF] = uint8_t(s >> 24); }
                }
            };
            fill(bus.m.data());
            std::memcpy(g_mem.data(), bus.m.data(), g_mem.size());
            for (int i = 0; i < k.size; ++i) { bus.m[(k.pc + i) & 0xFFFFFF] = k.bytes[i]; g_mem[(k.pc + i) & 0xFFFFFF] = k.bytes[i]; }

            // Musashi side
            m68k_pulse_reset(); m68k_execute(1);
            for (int i = 0; i < 8; ++i) { m68k_set_reg(m68k_register_t(M68K_REG_D0 + i), c.d[i]); if (i < 7) m68k_set_reg(m68k_register_t(M68K_REG_A0 + i), c.a[i]); }
            m68k_set_reg(M68K_REG_SR, 0x2000 | c.ccr());
            m68k_set_reg(M68K_REG_SP, c.a[7]);
            m68k_set_reg(M68K_REG_PC, k.pc);
            lift::Cpu orig = c;
            std::vector<uint8_t> before = bus.m;
            uint32_t npc = 0;
            lift::Exit ex = k.fn(c, npc);
            if (ex.kind != lift::Exit::None) { ++skipped; continue; }
            m68k_execute(1);
            ++checked;
            bool bad = false;
            auto cmp = [&](const char* what, uint32_t a, uint32_t b) { if (a != b) { if (!bad && fails < 25) std::printf("case %zu pc=%X bytes=%02x%02x%02x%02x: %s lift=%08X musashi=%08X\n", ci, k.pc, k.bytes[0], k.bytes[1], k.bytes[2], k.bytes[3], what, a, b); bad = true; } };
            for (int i = 0; i < 8; ++i) {
                char nm[8]; std::snprintf(nm, 8, "d%d", i); cmp(nm, c.d[i], m68k_get_reg(nullptr, m68k_register_t(M68K_REG_D0 + i)));
                std::snprintf(nm, 8, "a%d", i); cmp(nm, c.a[i], m68k_get_reg(nullptr, i < 7 ? m68k_register_t(M68K_REG_A0 + i) : M68K_REG_SP));
            }
            cmp("ccr", c.ccr(), m68k_get_reg(nullptr, M68K_REG_SR) & 0x1F);
            cmp("pc", npc, m68k_get_reg(nullptr, M68K_REG_PC));
            for (uint32_t i = 0; i < (1u << 24); i += 1) {}  // (placeholder removed below)
            if (!bad) {
                // memory compare restricted to touched windows
                for (int w = 0; w < 18 && !bad; ++w) {
                    uint32_t base = w < 8 ? orig.a[w] : (w < 16 ? orig.d[w - 8] : (w == 16 ? k.pc : orig.a[7]));
                    base = (base - 0x4000) & 0xFFFFFF;
                    for (uint32_t i = 0; i < 0x8000; ++i) if (bus.m[(base + i) & 0xFFFFFF] != g_mem[(base + i) & 0xFFFFFF]) { cmp("memory", bus.m[(base + i) & 0xFFFFFF], g_mem[(base + i) & 0xFFFFFF]); break; }
                }
            }
            if (bad) { ++fails; ++failures_by_case[ci]; break; }
        }
    }
    std::printf("checked %d samples, skipped %d, failing cases %d of %zu\n", checked, skipped, fails, sizeof(kCases) / sizeof(kCases[0]));
    return fails ? 1 : 0;
}
