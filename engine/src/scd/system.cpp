#include "system.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" {
#include "m68k.h"
}

namespace scd {

namespace {

constexpr int kMainClock = 7670454;
constexpr int kSubClock = 12500000;
constexpr int kLines = 262;
constexpr int kMainPerLine = kMainClock / 60 / kLines;   // 488
constexpr int kSubPerLine = kSubClock / 60 / kLines;     // 795
constexpr int kSlices = 4;
constexpr uint32_t kBiosCdbios = 0x5F22, kBiosBuram = 0x5F16, kBiosWaitVsync = 0x5F10, kBiosSetJmp = 0x5F0A;
constexpr uint32_t kCdbstatArea = 0x5000;  // BIOS work area handed back by CDBSTAT

enum Trap {
    kTrapWaitVsync = 1, kTrapBuram, kTrapCdbios, kTrapSetJmp,
    kTrapMainLicense = 10, kTrapMainSetVint, kTrapMainRestart, kTrapMainBuram,
    kTrapMainVector = 100,  // + vector number
    kTrapSubVector = 200,
};

void log(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    std::vfprintf(stderr, fmt, ap);
    va_end(ap);
}

uint8_t bcd(unsigned v) { return uint8_t(((v / 10) << 4) | (v % 10)); }

inline void put16(uint8_t* p, uint32_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
inline void put32(uint8_t* p, uint32_t v) { put16(p, v >> 16); put16(p + 2, v); }
inline uint32_t get32(const uint8_t* p) { return uint32_t(p[0]) << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

}  // namespace

// ------------------------------------------------------- Musashi callbacks

extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return System::instance().read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return System::instance().read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) {
    System& s = System::instance();
    return s.read16(a) << 16 | s.read16(a + 2);
}
unsigned int m68k_read_disassembler_8(unsigned int a) { return System::instance().read8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a) { return System::instance().read16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a) {
    System& s = System::instance();
    return s.read16(a) << 16 | s.read16(a + 2);
}
void m68k_write_memory_8(unsigned int a, unsigned int v) { System::instance().write8(a, v); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { System::instance().write16(a, v); }
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    System& s = System::instance();
    s.write16(a, v >> 16);
    s.write16(a + 2, v & 0xFFFF);
}
int scd_illg_handler(int opcode) { return System::instance().illegal(opcode); }
int scd_int_ack(int level) { return System::instance().int_ack(level); }
void scd_instruction_hook(unsigned int pc) { System::instance().trace_pc(pc); }
}

uint32_t current_pc_for_debug() { return m68k_get_reg(nullptr, M68K_REG_PPC) & 0xFFFFFF; }

System& System::instance() {
    static System s;
    return s;
}

// ------------------------------------------------------------------ setup

bool System::init(std::unique_ptr<Disc> disc, const std::string& save_path, std::string* error) {
    disc_ = std::move(disc);
    save_path_ = save_path;
    trace_bios_ = std::getenv("SCD_TRACE_BIOS") != nullptr;
    if (const char* w = std::getenv("SCD_STACKAT")) stack_at_ = uint32_t(std::strtoul(w, nullptr, 16));
    if (const char* w = std::getenv("SCD_WATCH")) watch_ = uint32_t(std::strtoul(w, nullptr, 16));

    uint8_t boot[0x8000];
    for (int i = 0; i < 16; ++i)
        if (!disc_->read_data(i, boot + i * kUserData)) {
            if (error) *error = "cannot read disc boot area";
            return false;
        }
    if (std::memcmp(boot, "SEGADISCSYSTEM", 14) != 0) {
        if (error) *error = "not a Sega CD disc (missing SEGADISCSYSTEM header)";
        return false;
    }

    std::memset(main_ram_, 0, sizeof main_ram_);
    std::memset(prg_ram_, 0, sizeof prg_ram_);
    std::memset(word_ram_, 0, sizeof word_ram_);
    std::memset(bram_, 0, sizeof bram_);
    std::memset(z80_ram_, 0, sizeof z80_ram_);
    if (!save_path_.empty()) {
        if (FILE* f = std::fopen(save_path_.c_str(), "rb")) {
            size_t n = std::fread(bram_, 1, sizeof bram_, f);
            (void)n;
            std::fclose(f);
        }
    }

    // IP (with the region security block) goes to Main work RAM, SP to Sub PRG RAM.
    std::memcpy(main_ram_, boot + 0x200, 0x6600);
    std::memcpy(prg_ram_ + 0x6000, boot + 0x6800, 0x1800);

    build_main_rom();
    build_sub_bios();
    bram_files_.clear();
    bram_load();

    vdp_.reset();
    vdp_.dma_read = [this](uint32_t a) { return uint16_t(main_read16(a)); };
    vdp_.irq_changed = [this] { update_main_irq(); };
    pcm_.reset();
    psg_.reset();
    OPN2_SetChipType(ym3438_mode_ym2612);
    OPN2_Reset(&ym_);

    m68k_init();
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    ctx_[0].resize(m68k_context_size());
    ctx_[1].resize(m68k_context_size());

    // Sub CPU first (reset vectors come from PRG RAM), then Main.
    cur_ = kSub;
    m68k_pulse_reset();
    m68k_get_context(ctx_[kSub].data());
    cur_ = kMain;
    m68k_pulse_reset();
    // The Main BIOS jumps straight to the IP; do the same.
    m68k_set_reg(M68K_REG_SP, 0xFFFFFD00);
    m68k_set_reg(M68K_REG_PC, 0xFF0000);
    m68k_set_reg(M68K_REG_SR, 0x2700);

    frames_ = 0;
    line_ = 0;
    halted_ = false;
    return true;
}

void System::shutdown() {
    if (bram_dirty_ && !save_path_.empty()) {
        if (FILE* f = std::fopen(save_path_.c_str(), "wb")) {
            std::fwrite(bram_, 1, sizeof bram_, f);
            std::fclose(f);
        }
        bram_dirty_ = false;
    }
}

void System::build_main_rom() {
    main_rom_.assign(0x20000, 0);
    uint8_t* r = main_rom_.data();
    put32(r + 0, 0xFFFFFD00);
    put32(r + 4, 0x000200 + 4 * 1);
    // Exceptions: each vector gets its own trap stub at 0x200 + 4*vector.
    for (int v = 2; v < 64; ++v) {
        uint32_t stub = 0x200 + 4 * v;
        put32(r + 4 * v, stub);
        put16(r + stub, 0x4AFC);
        put16(r + stub + 2, 0x4E75);
        hle_[stub] = kTrapMainVector + v;
    }
    // Interrupt autovectors go through RAM jump slots like the real BIOS.
    put32(r + 0x68, 0xFFFFFD12);
    put32(r + 0x70, 0xFFFFFD0C);
    put32(r + 0x78, 0xFFFFFD06);
    const uint32_t rte = 0x9A4;
    put16(r + rte, 0x4E73);
    for (uint32_t slot = 0xFD06; slot < 0xFD7E; slot += 6) {
        put16(main_ram_ + slot, 0x4EF9);
        put32(main_ram_ + slot + 2, rte);
    }
    auto trap = [&](uint32_t addr, int id) {
        put16(r + addr, 0x4AFC);
        put16(r + addr + 2, 0x4E75);
        hle_[addr] = id;
    };
    trap(0x364, kTrapMainLicense);   // region licence screen: skipped
    trap(0x368, kTrapMainSetVint);   // a1 = V-INT handler
    trap(0x28C, kTrapMainRestart);   // return to the BIOS control panel
    trap(0x70EE, kTrapMainBuram);    // Main CPU backup RAM entry, reached through the RAM slot at $FFFDAE
    put16(main_ram_ + 0xFDA8, 0x4EF9);
    put32(main_ram_ + 0xFDAA, 0x414);
    put16(main_ram_ + 0xFDAE, 0x4EF9);
    put32(main_ram_ + 0xFDB0, 0x70EE);
    std::memcpy(r + 0x100, "SEGA MEGA DRIVE ", 16);
}

void System::build_sub_bios() {
    uint8_t* p = prg_ram_;
    put32(p + 0, 0x00005F00);
    put32(p + 4, 0x00000200);
    for (int v = 2; v < 64; ++v) {
        uint32_t stub = 0x400 + 4 * v;
        put32(p + 4 * v, stub);
        put16(p + stub, 0x4AFC);
        put16(p + stub + 2, 0x4E75);
        hle_[stub] = kTrapSubVector + v;
    }
    // Level n autovector -> RAM jump slot at 0x5F70 + 6n (the SUBCODE patches the level 3 slot).
    const uint32_t rte = 0x0190;
    put16(p + rte, 0x4E73);
    for (int lv = 1; lv <= 7; ++lv) {
        uint32_t slot = 0x5F70 + 6 * lv;
        put32(p + 0x60 + 4 * lv, slot);
        put16(p + slot, 0x4EF9);
        put32(p + slot + 2, rte);
    }
    // Level 2 calls the SP's int2 entry (SP header offset table at 0x6020).
    const uint32_t int2 = 0x0180;
    static const uint8_t wrap[] = {
        0x48, 0xE7, 0xFF, 0xFE,              // movem.l d0-d7/a0-a6,-(sp)
        0x4E, 0xB9, 0x00, 0x00, 0x60, 0xBE,  // jsr $60BE
        0x4C, 0xDF, 0x7F, 0xFF,              // movem.l (sp)+,d0-d7/a0-a6
        0x4E, 0x73,                          // rte
    };
    std::memcpy(p + int2, wrap, sizeof wrap);
    put32(p + 0x5F7C + 2, int2);
    // Boot stub: SP init, enable interrupts, then call the SP main forever.
    static const uint8_t boot[] = {
        0x4E, 0xB9, 0x00, 0x00, 0x60, 0x2E,  // jsr $602E
        0x46, 0xFC, 0x20, 0x00,              // move #$2000,sr
        0x4E, 0xB9, 0x00, 0x00, 0x60, 0xA8,  // loop: jsr $60A8
        0x60, 0xF8,                          // bra loop
    };
    std::memcpy(p + 0x200, boot, sizeof boot);
    // BIOS entry points.
    struct { uint32_t a; int id; } entries[] = {
        {kBiosSetJmp, kTrapSetJmp}, {kBiosWaitVsync, kTrapWaitVsync},
        {kBiosBuram, kTrapBuram}, {kBiosCdbios, kTrapCdbios},
    };
    for (auto& e : entries) {
        put16(p + e.a, 0x4AFC);
        put16(p + e.a + 2, 0x4E75);
        hle_[e.a] = e.id;
    }
}

// -------------------------------------------------------------- scheduling

void System::select_cpu(Cpu c) {
    if (cur_ == c) return;
    m68k_get_context(ctx_[cur_].data());
    m68k_set_context(ctx_[c].data());
    cur_ = c;
    if (irq_dirty_[c]) {
        m68k_set_irq(cpu_irq_[c]);
        irq_dirty_[c] = false;
    }
}

void System::refresh_irq(Cpu c) {
    int lvl = c == kMain ? main_irq_level() : sub_irq_level();
    if (cpu_irq_[c] != lvl || irq_dirty_[c]) {
        cpu_irq_[c] = lvl;
        if (cur_ == c) {
            m68k_set_irq(lvl);
            irq_dirty_[c] = false;
        } else {
            irq_dirty_[c] = true;
        }
    }
}

int System::main_irq_level() const {
    int v = vdp_.irq_level();
    if (v) return v;
    if (main_int2_pending_ && ien2_) return 2;
    return 0;
}

int System::sub_irq_level() const {
    for (int lv = 6; lv >= 1; --lv)
        if ((sub_pending_ & (1 << lv)) && (int_mask_ & (1 << lv))) return lv;
    return 0;
}

void System::update_main_irq() { refresh_irq(kMain); }
void System::update_sub_irq() { refresh_irq(kSub); }

void System::trace_pc(uint32_t pc) {
    if (stack_at_ && (pc & 0xFFFFFF) == stack_at_ && frames_ >= trace_from_ && stack_left_ > 0) {
        --stack_left_;
        uint32_t sp = reg_get(M68K_REG_SP);
        std::fprintf(stderr, "[stack %s pc=%06x sp=%06x]:", cur_ ? "S" : "M", pc, sp);
        for (int i = 0; i < 16; ++i) std::fprintf(stderr, " %04x", read16(sp + 2 * i));
        std::fprintf(stderr, "\n");
    }
    if (trace_cpu_ == int(cur_) && trace_left_ > 0 && frames_ >= trace_from_) {
        --trace_left_;
        std::fprintf(stderr, "%s %06x  d0=%08x d1=%08x a0=%08x a1=%08x\n", cur_ ? "S" : "M", pc & 0xFFFFFF,
                     reg_get(M68K_REG_D0), reg_get(M68K_REG_D1), reg_get(M68K_REG_A0), reg_get(M68K_REG_A1));
    }
}

int System::int_ack(int level) {
    if (cur_ == kMain) {
        if (level == 6 || level == 4) vdp_.irq_ack(level);
        else if (level == 2) main_int2_pending_ = false;
        refresh_irq(kMain);
    } else {
        sub_pending_ &= ~(1 << level);
        refresh_irq(kSub);
    }
    return M68K_INT_ACK_AUTOVECTOR;
}

void System::advance_sub_time(int cycles) {
    sub_tick_accum_ += cycles;
    while (sub_tick_accum_ >= 384) {
        sub_tick_accum_ -= 384;
        stopwatch_ = (stopwatch_ + 1) & 0xFFF;
        if (timer_reg_) {
            if (--timer_count_ <= 0) {
                timer_count_ = timer_reg_;
                sub_pending_ |= 1 << 3;
                update_sub_irq();
            }
        }
    }
}

void System::run_cpu(Cpu c, int cycles) {
    if (c == kSub && !sub_run_) {
        advance_sub_time(cycles);
        return;
    }
    cpu_debt_[c] -= cycles;  // negative debt = budget
    int budget = -cpu_debt_[c];
    if (budget <= 0) return;
    select_cpu(c);
    refresh_irq(c);
    int used = m68k_execute(budget);
    cpu_debt_[c] = used - budget;
    if (profile_) ++pc_hist_[c][reg_get(M68K_REG_PC) & 0xFFFFFF];
    if (c == kSub) advance_sub_time(used);
}

void System::run_frame() {
    if (halted_) return;
    const int active = vdp_.active_lines();
    for (line_ = 0; line_ < kLines; ++line_) {
        vdp_.begin_line(line_);
        refresh_irq(kMain);
        for (int s = 0; s < kSlices; ++s) {
            main_cycle_in_line_ = s * (kMainPerLine / kSlices);
            run_cpu(kMain, kMainPerLine / kSlices);
            run_cpu(kSub, kSubPerLine / kSlices);
            if (s == 0) { vdp_.line_progress(); refresh_irq(kMain); }
        }
        if (line_ < active) vdp_.render_line(line_);
        audio_frac_ += double(kSampleRate) / (60.0 * kLines);
        int n = int(audio_frac_);
        audio_frac_ -= n;
        audio_run(n);
    }
    ++frames_;
}

// ------------------------------------------------------------------ memory

uint32_t System::read8(uint32_t a) { return cur_ == kMain ? main_read8(a) : sub_read8(a); }

uint32_t System::read16(uint32_t a) {
    if (cur_ == kMain) return main_read16(a);
    return sub_read8(a) << 8 | sub_read8(a + 1);
}

void System::write8(uint32_t a, uint32_t v) {
    if (cur_ == kMain) main_write8(a, v);
    else sub_write8(a, v);
}

void System::write16(uint32_t a, uint32_t v) {
    if (cur_ == kMain) main_write16(a, v);
    else {
        sub_write8(a, v >> 8);
        sub_write8(a + 1, v & 0xFF);
    }
}

uint8_t* System::word_ram_ptr(Cpu who, uint32_t off) {
    if (!mode_1m_) {
        if (word_owner_ != (who == kMain ? 0 : 1)) return nullptr;
        return word_ram_ + (off & 0x3FFFF);
    }
    // 1M mode: each CPU sees 128 KB; the banks swap on RET.
    int bank = (who == kMain) ? word_owner_ : 1 - word_owner_;
    return word_ram_ + bank * 0x20000 + (off & 0x1FFFF);
}

uint32_t System::main_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x20000) {
        if (a == 0x72) return hint_vec_ >> 8;
        if (a == 0x73) return hint_vec_ & 0xFF;
        return main_rom_[a];
    }
    if (a < 0x40000) return prg_ram_[(prg_bank_ << 17) | (a & 0x1FFFF)];
    if (a >= 0x200000 && a < 0x240000) {
        uint8_t* p = word_ram_ptr(kMain, a - 0x200000);
        return p ? *p : 0;
    }
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a >= 0xA04000 && a < 0xA04004) return OPN2_Read(&ym_, a & 3);
        if (a < 0xA02000) return z80_ram_[a & 0x1FFF];
        return 0;
    }
    if (a >= 0xA10000 && a < 0xA10020) return io_read(a & 0x1F);
    if (a == 0xA11100) return z80_busreq_ ? 0 : 1;
    if (a == 0xA11101) return 0;
    if (a >= 0xA12000 && a < 0xA12040) return ga_read(kMain, a & 0x3F);
    if (a >= 0xC00000 && a < 0xE00000) {
        uint32_t r = a & 0x1F;
        if (r < 4) return vdp_.read_data() >> ((a & 1) ? 0 : 8);
        if (r < 8) return vdp_.read_status() >> ((a & 1) ? 0 : 8);
        if (r < 16) return vdp_.read_hv(line_, main_cycle_in_line_, kMainPerLine) >> ((a & 1) ? 0 : 8);
        return 0;
    }
    if (a >= 0xE00000) return main_ram_[a & 0xFFFF];
    return 0;
}

uint32_t System::main_read16(uint32_t a) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xE00000) {
        uint32_t r = a & 0x1F;
        if (r < 4) return vdp_.read_data();
        if (r < 8) return vdp_.read_status();
        if (r < 16) return vdp_.read_hv(line_, main_cycle_in_line_, kMainPerLine);
        return 0;
    }
    return main_read8(a) << 8 | main_read8(a + 1);
}

void System::main_write8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    v &= 0xFF;
    if (watch_ && (a & 0xFFFF) == (watch_ & 0xFFFF) && a >= 0xE00000)
        std::fprintf(stderr, "[watch] frame %llu line %d PC=%06x write %02x\n", (unsigned long long)frames_, line_, reg_get(M68K_REG_PPC) & 0xFFFFFF, v);
    if (a < 0x20000) return;
    if (a < 0x40000) {
        prg_ram_[(prg_bank_ << 17) | (a & 0x1FFFF)] = uint8_t(v);
        return;
    }
    if (a >= 0x200000 && a < 0x240000) {
        if (uint8_t* p = word_ram_ptr(kMain, a - 0x200000)) *p = uint8_t(v);
        return;
    }
    if (a >= 0xA00000 && a < 0xA10000) {
        if (a >= 0xA04000 && a < 0xA04004) ym_write(a & 3, uint8_t(v));
        else if (a < 0xA02000) z80_ram_[a & 0x1FFF] = uint8_t(v);
        return;
    }
    if (a >= 0xA10000 && a < 0xA10020) { io_write(a & 0x1F, uint8_t(v)); return; }
    if (a == 0xA11100) { z80_busreq_ = v & 1; return; }
    if (a == 0xA11200) { z80_reset_ = !(v & 1); return; }
    if (a >= 0xA12000 && a < 0xA12040) { ga_write(kMain, a & 0x3F, uint8_t(v)); return; }
    if (a >= 0xC00000 && a < 0xE00000) {
        uint32_t r = a & 0x1F;
        if (r < 4) vdp_.write_data(uint16_t(v << 8 | v));
        else if (r < 8) vdp_.write_ctrl(uint16_t(v << 8 | v));
        else if (r >= 0x11 && r < 0x18 && (r & 1)) psg_.write(uint8_t(v));
        return;
    }
    if (a >= 0xE00000) main_ram_[a & 0xFFFF] = uint8_t(v);
}

void System::main_write16(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    if (a >= 0xC00000 && a < 0xE00000) {
        uint32_t r = a & 0x1F;
        if (r < 4) vdp_.write_data(uint16_t(v));
        else if (r < 8) vdp_.write_ctrl(uint16_t(v));
        else if (r >= 0x10 && r < 0x18) psg_.write(uint8_t(v));
        return;
    }
    main_write8(a, v >> 8);
    main_write8(a + 1, v & 0xFF);
}

uint32_t System::sub_read8(uint32_t a) {
    a &= 0xFFFFFF;
    if (a < 0x80000) return prg_ram_[a];
    if (a < 0xC0000) {
        if (mode_1m_) return 0;
        uint8_t* p = word_ram_ptr(kSub, a - 0x80000);
        return p ? *p : 0;
    }
    if (a < 0xE0000) {
        if (!mode_1m_) return 0;
        uint8_t* p = word_ram_ptr(kSub, a - 0xC0000);
        return p ? *p : 0;
    }
    if (a >= 0xFE0000 && a < 0xFE4000) return (a & 1) ? bram_[(a >> 1) & 0x1FFF] : 0xFF;
    if (a >= 0xFF0000 && a < 0xFF4000) return pcm_.read(a & 0x3FFF);
    if (a >= 0xFF8000 && a < 0xFF8200) return ga_read(kSub, a & 0x7F);
    return 0;
}

void System::sub_write8(uint32_t a, uint32_t v) {
    a &= 0xFFFFFF;
    v &= 0xFF;
    if (a < 0x80000) {
        if (a >= uint32_t(write_protect_) * 0x200) prg_ram_[a] = uint8_t(v);
        return;
    }
    if (a < 0xC0000) {
        if (!mode_1m_)
            if (uint8_t* p = word_ram_ptr(kSub, a - 0x80000)) *p = uint8_t(v);
        return;
    }
    if (a < 0xE0000) {
        if (mode_1m_)
            if (uint8_t* p = word_ram_ptr(kSub, a - 0xC0000)) *p = uint8_t(v);
        return;
    }
    if (a >= 0xFE0000 && a < 0xFE4000) {
        if (a & 1) {
            bram_[(a >> 1) & 0x1FFF] = uint8_t(v);
            bram_dirty_ = true;
        }
        return;
    }
    if (a >= 0xFF0000 && a < 0xFF4000) { pcm_.write(a & 0x3FFF, uint8_t(v)); return; }
    if (a >= 0xFF8000 && a < 0xFF8200) ga_write(kSub, a & 0x7F, uint8_t(v));
}

// ------------------------------------------------------------- Gate Array

uint8_t System::ga_read(Cpu who, uint32_t off) {
    off &= 0x3F;
    switch (off) {
        case 0x00: return who == kMain ? uint8_t(ien2_ << 7) : 0;
        case 0x01: return who == kMain ? uint8_t(sub_run_ ? 1 : 0) : 0x01;
        case 0x02: return write_protect_;
        case 0x03: {
            uint8_t v = uint8_t(mode_1m_ << 2);
            if (who == kMain) {
                v |= uint8_t(prg_bank_ << 6);
                v |= uint8_t((word_owner_ == 1) << 1) | uint8_t(word_owner_ == 0);
            } else {
                v |= uint8_t((word_owner_ == 1) << 1) | uint8_t(word_owner_ == 0);
            }
            return v;
        }
        case 0x04: return uint8_t(cdc_dest_ | (cdc_dsr_ ? 0x40 : 0) | (cdc_edt_ ? 0x80 : 0));
        case 0x05: return 0;
        case 0x06: return who == kMain ? uint8_t(hint_vec_ >> 8) : 0;
        case 0x07: return who == kMain ? uint8_t(hint_vec_) : 0;
        case 0x0A: return uint8_t(dma_addr_ >> 8);
        case 0x0B: return uint8_t(dma_addr_);
        case 0x0C: return uint8_t(stopwatch_ >> 8);
        case 0x0D: return uint8_t(stopwatch_);
        case 0x0E: return main_flag_;
        case 0x0F: return sub_flag_;
        case 0x30: return 0;
        case 0x31: return timer_reg_;
        case 0x32: return uint8_t(int_mask_ >> 8);
        case 0x33: return uint8_t(int_mask_);
    }
    if (off >= 0x10 && off < 0x20) return cmd_[off - 0x10];
    if (off >= 0x20 && off < 0x30) return stat_[off - 0x20];
    return 0;
}

void System::ga_write(Cpu who, uint32_t off, uint8_t v) {
    off &= 0x3F;
    if (who == kMain) {
        switch (off) {
            case 0x00:
                ien2_ = v & 0x80;
                if (v & 0x01) {  // IFL2: interrupt the Sub CPU at level 2
                    sub_pending_ |= 1 << 2;
                    update_sub_irq();
                }
                update_main_irq();
                return;
            case 0x01:
                sub_run_ = v & 1;
                return;
            case 0x02: write_protect_ = v; return;
            case 0x03:
                prg_bank_ = (v >> 6) & 3;
                if (v & 0x02) word_owner_ = 1;  // DMNA: hand Word RAM to the Sub CPU
                return;
            case 0x06: hint_vec_ = uint16_t((hint_vec_ & 0x00FF) | (v << 8)); return;
            case 0x07: hint_vec_ = uint16_t((hint_vec_ & 0xFF00) | v); return;
            case 0x0E: main_flag_ = v; return;
        }
        if (off >= 0x10 && off < 0x20) cmd_[off - 0x10] = v;
        return;
    }
    switch (off) {
        case 0x00:
            if (v & 0x01) {  // IFL2: interrupt the Main CPU at level 2
                main_int2_pending_ = true;
                update_main_irq();
            }
            return;
        case 0x03:
            mode_1m_ = v & 0x04;
            if (v & 0x01) word_owner_ = 0;  // RET: give Word RAM back to Main
            return;
        case 0x04: cdc_dest_ = v & 7; return;
        case 0x0A: dma_addr_ = uint16_t((dma_addr_ & 0x00FF) | (v << 8)); return;
        case 0x0B: dma_addr_ = uint16_t((dma_addr_ & 0xFF00) | v); return;
        case 0x0C:
        case 0x0D: stopwatch_ = 0; return;
        case 0x0F: sub_flag_ = v; return;
        case 0x31:
            timer_reg_ = v;
            timer_count_ = v;
            return;
        case 0x32: int_mask_ = uint16_t((int_mask_ & 0x00FF) | (v << 8)); return;
        case 0x33:
            int_mask_ = uint16_t((int_mask_ & 0xFF00) | v);
            update_sub_irq();
            return;
    }
    if (off >= 0x20 && off < 0x30) stat_[off - 0x20] = v;
}

// -------------------------------------------------------------------- I/O

uint8_t System::io_read(uint32_t reg) {
    switch (reg) {
        case 0x01: return 0x81;  // overseas, NTSC, Mega-CD attached
        case 0x03:
        case 0x05: {
            int p = reg == 0x03 ? 0 : 1;
            uint16_t b = pad_[p];
            uint8_t th = io_data_[p] & 0x40;
            uint8_t v;
            if (th) v = uint8_t(0x40 | (~b & 0x0F) | ((~b & kB) ? 0x10 : 0) | ((~b & kC) ? 0x20 : 0));
            else v = uint8_t(0x00 | (~b & 0x03) | ((~b & kA) ? 0x10 : 0) | ((~b & kStart) ? 0x20 : 0));
            uint8_t ctrl = io_ctrl_[p];
            return uint8_t((v & ~ctrl) | (io_data_[p] & ctrl));
        }
        case 0x07: return io_data_[2];
        case 0x09: return io_ctrl_[0];
        case 0x0B: return io_ctrl_[1];
        case 0x0D: return io_ctrl_[2];
    }
    return 0;
}

void System::io_write(uint32_t reg, uint8_t v) {
    switch (reg) {
        case 0x03: io_data_[0] = v; break;
        case 0x05: io_data_[1] = v; break;
        case 0x07: io_data_[2] = v; break;
        case 0x09: io_ctrl_[0] = v; break;
        case 0x0B: io_ctrl_[1] = v; break;
        case 0x0D: io_ctrl_[2] = v; break;
    }
}

void System::ym_write(int port, uint8_t v) { OPN2_Write(&ym_, uint32_t(port), v); }

// -------------------------------------------------------------- HLE BIOS

uint32_t System::reg_get(int r) { return m68k_get_reg(nullptr, m68k_register_t(r)); }
void System::reg_set(int r, uint32_t v) { m68k_set_reg(m68k_register_t(r), v); }

void System::set_carry(bool c) {
    uint32_t sr = reg_get(M68K_REG_SR);
    reg_set(M68K_REG_SR, c ? (sr | 1) : (sr & ~1u));
}

int System::illegal(int opcode) {
    if (opcode != 0x4AFC) return 0;
    uint32_t pc = reg_get(M68K_REG_PPC) & 0xFFFFFF;
    auto it = hle_.find(pc);
    if (it == hle_.end()) return 0;
    int id = it->second;
    if ((id >= kTrapMainVector && id < kTrapMainVector + 64) || (id >= kTrapSubVector && id < kTrapSubVector + 64)) {
        bool sub = id >= kTrapSubVector;
        uint32_t sp = reg_get(M68K_REG_SP);
        uint32_t fpc = read16(sp + 2) << 16 | read16(sp + 4);
        log("[%s CPU] unexpected exception vector %d at PC=%06x SR=%04x (frame %llu); code:",
            sub ? "Sub" : "Main", id - (sub ? kTrapSubVector : kTrapMainVector), fpc & 0xFFFFFF, read16(sp),
            (unsigned long long)frames_);
        for (int i = -8; i < 8; i += 2) log(" %04x", read16(fpc + i));
        log("\n");
        halted_ = true;
        m68k_end_timeslice();
        return 1;
    }
    switch (id) {
        case kTrapWaitVsync:
        case kTrapSetJmp:
        case kTrapMainLicense:
            break;
        case kTrapMainSetVint:
            put32(main_ram_ + 0xFD08, reg_get(M68K_REG_A1));
            break;
        case kTrapMainRestart:
            log("[Main CPU] game requested a BIOS restart (jmp $28C)\n");
            halted_ = true;
            m68k_end_timeslice();
            break;
        case kTrapCdbios: cdbios(int(reg_get(M68K_REG_D0) & 0xFFFF)); break;
        case kTrapBuram: buram(int(reg_get(M68K_REG_D0) & 0xFFFF)); break;
        case kTrapMainBuram: {
            // The Main-side _BURAM ($FFFDAE) drives the RAM cartridge, which is never present.
            int fn = int(reg_get(M68K_REG_D0) & 0xFFFF);
            if (trace_bios_) log("MAIN BURAM %02x (no cartridge)\n", fn);
            uint32_t d0 = fn == 0 ? 0 : 0xFFFF, d1 = fn == 0 ? 0 : 0xFFFF;
            reg_set(M68K_REG_D0, d0);
            reg_set(M68K_REG_D1, d1);
            set_carry(true);
            break;
        }
    }
    return 1;
}

bool System::cdc_load_sector() {
    if (cdc_buf_valid_) return true;
    if (!cdc_reading_ || cdc_remaining_ == 0) return false;
    if (!disc_->read_data(cdc_lba_, cdc_buf_)) {
        std::memset(cdc_buf_, 0, sizeof cdc_buf_);
    }
    uint32_t t = cdc_lba_ + 150;
    cdc_header_[0] = bcd(t / 75 / 60);
    cdc_header_[1] = bcd(t / 75 % 60);
    cdc_header_[2] = bcd(t % 75);
    cdc_header_[3] = 1;
    cdc_buf_valid_ = true;
    return true;
}

void System::cdc_do_transfer() {
    uint32_t base = uint32_t(dma_addr_) << 3;
    switch (cdc_dest_) {
        case 4: {  // PCM wave RAM
            uint8_t* ram = pcm_.wave_ram();
            if (trace_bios_) log("  PCM DMA addr_reg=%04x bank=%d\n", dma_addr_, pcm_.bank());
            for (int i = 0; i < kUserData; ++i) ram[(uint32_t(pcm_.bank()) << 12 | ((base + i) & 0xFFF)) & 0xFFFF] = cdc_buf_[i];
            break;
        }
        case 5:  // PRG RAM
            for (int i = 0; i < kUserData; ++i) prg_ram_[(base + i) & 0x7FFFF] = cdc_buf_[i];
            break;
        case 7:  // Word RAM
            for (int i = 0; i < kUserData; ++i) {
                uint8_t* p = word_ram_ptr(kSub, (base + i));
                if (p) *p = cdc_buf_[i];
            }
            break;
        default: break;
    }
    dma_addr_ = uint16_t(dma_addr_ + kUserData / 8);
}

void System::cdbios(int fn) {
    const uint32_t a0 = reg_get(M68K_REG_A0), a1 = reg_get(M68K_REG_A1);
    if (trace_bios_) log("CDBIOS %02x a0=%06x a1=%06x d1=%08x\n", fn, a0, a1, reg_get(M68K_REG_D1));
    switch (fn) {
        case 0x02: cdda_playing_ = false; set_carry(false); break;             // MSCSTOP
        case 0x03: case 0x04: set_carry(false); break;                         // pause on/off
        case 0x08: case 0x09: set_carry(false); break;                         // ROMPAUSEON/OFF
        case 0x10: set_carry(false); break;                                    // DRVINIT
        case 0x11: case 0x12: case 0x13: {                                     // MSCPLAY / PLAY1 / PLAYR
            int track = int(((uint32_t(sub_read8(a0)) << 8) | sub_read8(a0 + 1)));
            const auto& tr = disc_->tracks();
            if (track >= 1 && track <= int(tr.size()) && tr[track - 1].audio) {
                cdda_playing_ = true;
                cdda_track_ = track;
                cdda_lba_ = tr[track - 1].start_lba;
                cdda_end_ = tr[track - 1].end_lba;
                cdda_loop_ = fn == 0x13;
                cdda_pos_ = kRawSector;
            } else {
                cdda_playing_ = false;
            }
            set_carry(false);
            break;
        }
        case 0x20: {                                                           // ROMREADN
            cdc_lba_ = (uint32_t(sub_read8(a0)) << 24) | (sub_read8(a0 + 1) << 16) | (sub_read8(a0 + 2) << 8) | sub_read8(a0 + 3);
            cdc_remaining_ = (uint32_t(sub_read8(a0 + 4)) << 24) | (sub_read8(a0 + 5) << 16) | (sub_read8(a0 + 6) << 8) | sub_read8(a0 + 7);
            if (trace_bios_) log("  ROMREADN lba=%u count=%u (frame %llu)\n", cdc_lba_, cdc_remaining_, (unsigned long long)frames_);
            cdc_reading_ = true;
            cdc_buf_valid_ = false;
            cdc_edt_ = cdc_dsr_ = false;
            set_carry(false);
            break;
        }
        case 0x80: set_carry(false); break;                                    // CDBCHK
        case 0x81: {                                                           // CDBSTAT
            uint8_t* st = prg_ram_ + kCdbstatArea;
            std::memset(st, 0, 0x20);
            put32(st + 8, 0xFFFFFFFF);
            put32(st + 12, 0xFFFFFFFF);
            if (cdda_playing_) {
                put16(st + 0, 0x0100);
                put32(st + 8, (uint32_t(bcd((cdda_lba_ + 150) / 75 / 60)) << 16) | (bcd((cdda_lba_ + 150) / 75 % 60) << 8) | bcd((cdda_lba_ + 150) % 75));
                put32(st + 12, uint32_t(cdda_track_) << 24);
            }
            reg_set(M68K_REG_A0, kCdbstatArea);
            set_carry(false);
            break;
        }
        case 0x84: set_carry(false); break;                                    // CDBPAUSE
        case 0x89:                                                             // CDCSTOP
            cdc_reading_ = false;
            cdc_buf_valid_ = false;
            cdc_edt_ = cdc_dsr_ = false;
            set_carry(false);
            break;
        case 0x8A:                                                             // CDCSTAT
            cdc_load_sector();
            set_carry(!cdc_buf_valid_);
            break;
        case 0x8B:                                                             // CDCREAD
            if (!cdc_load_sector()) {
                set_carry(true);
                break;
            }
            reg_set(M68K_REG_D0, get32(cdc_header_));
            if (cdc_dest_ == 3 || cdc_dest_ == 2) cdc_dsr_ = true;
            else {
                cdc_do_transfer();
                cdc_edt_ = true;
            }
            set_carry(false);
            break;
        case 0x8C: {                                                           // CDCTRN
            if (!cdc_buf_valid_) {
                set_carry(true);
                break;
            }
            for (int i = 0; i < kUserData; ++i) sub_write8(a0 + i, cdc_buf_[i]);
            for (int i = 0; i < 4; ++i) sub_write8(a1 + i, cdc_header_[i]);
            reg_set(M68K_REG_A0, a0 + kUserData);
            reg_set(M68K_REG_A1, a1 + 4);
            cdc_edt_ = true;
            set_carry(false);
            break;
        }
        case 0x8D:                                                             // CDCACK
            if (cdc_buf_valid_) {
                cdc_buf_valid_ = false;
                ++cdc_lba_;
                if (cdc_remaining_ && --cdc_remaining_ == 0) cdc_reading_ = false;
            }
            cdc_edt_ = cdc_dsr_ = false;
            set_carry(false);
            break;
        default:
            log("[BIOS] unimplemented _CDBIOS function %02x (frame %llu)\n", fn, (unsigned long long)frames_);
            set_carry(true);
            break;
    }
}

// ------------------------------------------------------------ backup RAM
// Internal backup RAM is 8 KB of 64-byte blocks. The BIOS _BURAM calls are emulated with a
// simple file table that is serialised into the image: file data from the start, 0x20-byte
// directory entries growing down from the header block at the end.

namespace {
constexpr int kBramSize = 0x2000;
constexpr int kBramHeader = kBramSize - 0x40;
constexpr int kBramBlocks = kBramSize / 0x40;
constexpr const char kBramMagic[] = "SEGA_CD_ROM";
}  // namespace

int System::bram_free_blocks() const {
    int used = 3;  // header, plus the BIOS reserves two more
    for (auto& f : bram_files_) used += int((f.data.size() + 0x20 + 0x3F) / 0x40);
    return std::max(0, kBramBlocks - used);
}

void System::bram_load() {
    bram_files_.clear();
    const uint8_t* h = bram_ + kBramHeader;
    if (std::memcmp(h + 0x20, kBramMagic, 11) != 0) {
        // Unformatted: format it like the BIOS does.
        std::memset(bram_, 0, sizeof bram_);
        std::memcpy(h == bram_ ? bram_ : bram_ + kBramHeader + 0x20, kBramMagic, 11);
        bram_store();
        return;
    }
    int count = bram_[kBramHeader + 0x18] << 8 | bram_[kBramHeader + 0x19];
    uint32_t offset = 0;
    for (int i = 0; i < count && i < 100; ++i) {
        const uint8_t* e = bram_ + kBramHeader - 0x20 * (i + 1);
        BramFile f;
        std::memcpy(f.name, e, 11);
        f.flag = e[11];
        f.blocks = uint16_t(e[12] << 8 | e[13]);
        size_t len = size_t(f.blocks) * 32;
        if (offset + len > kBramHeader - 0x20 * (count)) break;
        f.data.assign(bram_ + offset, bram_ + offset + len);
        offset += uint32_t(len);
        bram_files_.push_back(std::move(f));
    }
}

void System::bram_store() {
    std::memset(bram_, 0, sizeof bram_);
    uint32_t offset = 0;
    for (size_t i = 0; i < bram_files_.size(); ++i) {
        const BramFile& f = bram_files_[i];
        std::memcpy(bram_ + offset, f.data.data(), f.data.size());
        offset += uint32_t(f.data.size());
        uint8_t* e = bram_ + kBramHeader - 0x20 * (i + 1);
        std::memcpy(e, f.name, 11);
        e[11] = f.flag;
        put16(e + 12, f.blocks);
    }
    uint8_t* h = bram_ + kBramHeader;
    std::memcpy(h, "___________\0\0\0\0\x40", 16);
    int free = bram_free_blocks();
    for (int i = 0; i < 4; ++i) put16(h + 0x10 + 2 * i, uint16_t(free));
    put16(h + 0x18, uint16_t(bram_files_.size()));
    std::memcpy(h + 0x20, kBramMagic, 11);
    h[0x2C] = 1;
    std::memcpy(h + 0x30, "RAM_CARTRIDGE___", 16);
    bram_dirty_ = true;
}

int System::bram_find(const uint8_t* name) const {
    for (size_t i = 0; i < bram_files_.size(); ++i)
        if (std::memcmp(bram_files_[i].name, name, 11) == 0) return int(i);
    return -1;
}

void System::buram(int fn) {
    const uint32_t a0 = reg_get(M68K_REG_A0), a1 = reg_get(M68K_REG_A1), d1 = reg_get(M68K_REG_D1);
    if (trace_bios_) log("BURAM %02x a0=%06x a1=%06x d1=%08x ret=%06x (frame %llu)\n", fn, a0, a1, d1, (read16(reg_get(M68K_REG_SP)) << 16 | read16(reg_get(M68K_REG_SP) + 2)) & 0xFFFFFF, (unsigned long long)frames_);
    if (bram_files_.empty() && std::memcmp(bram_ + kBramHeader + 0x20, kBramMagic, 11) != 0) bram_load();
    auto read_name = [&](uint32_t a, uint8_t* n) { for (int i = 0; i < 11; ++i) n[i] = uint8_t(read8(a + i)); };
    switch (fn) {
        case 0: {  // BRMINIT
            for (int i = 0; i < 12; ++i) write8(a1 + i, "SEGA_CD_ROM"[i]);
            reg_set(M68K_REG_D0, kBramSize >> 13);  // capacity in 8 KB units
            set_carry(false);
            break;
        }
        case 1:  // BRMSTAT: d0 = free blocks, d1 = number of files
            reg_set(M68K_REG_D0, uint32_t(bram_free_blocks()));
            reg_set(M68K_REG_D1, uint32_t(bram_files_.size()));
            set_carry(false);
            break;
        case 2: {  // BRMSERCH
            uint8_t n[11];
            read_name(a0, n);
            int i = bram_find(n);
            if (i < 0) { set_carry(true); break; }
            for (int k = 0; k < 11; ++k) write8(a1 + k, bram_files_[i].name[k]);
            write8(a1 + 11, bram_files_[i].flag);
            write8(a1 + 12, bram_files_[i].blocks >> 8);
            write8(a1 + 13, bram_files_[i].blocks & 0xFF);
            reg_set(M68K_REG_D0, bram_files_[i].blocks);
            set_carry(false);
            break;
        }
        case 3: {  // BRMREAD
            uint8_t n[11];
            read_name(a0, n);
            int i = bram_find(n);
            if (i < 0) { set_carry(true); break; }
            for (size_t k = 0; k < bram_files_[i].data.size(); ++k) write8(a1 + uint32_t(k), bram_files_[i].data[k]);
            reg_set(M68K_REG_D0, bram_files_[i].blocks);
            set_carry(false);
            break;
        }
        case 4: {  // BRMWRITE: a0 = name[11] flag blocks.w, a1 = data
            BramFile f;
            read_name(a0, f.name);
            f.flag = uint8_t(read8(a0 + 11));
            f.blocks = uint16_t(read8(a0 + 12) << 8 | read8(a0 + 13));
            f.data.resize(size_t(f.blocks) * 32);
            for (size_t k = 0; k < f.data.size(); ++k) f.data[k] = uint8_t(read8(a1 + uint32_t(k)));
            int old = bram_find(f.name);
            BramFile saved;
            if (old >= 0) { saved = bram_files_[old]; bram_files_.erase(bram_files_.begin() + old); }
            bram_files_.push_back(f);
            if (bram_free_blocks() == 0 && int(bram_files_.size()) > 1 && (f.data.size() + 0x20) / 0x40 > 0) {
                // Out of space: roll back.
                bram_files_.pop_back();
                if (old >= 0) bram_files_.insert(bram_files_.begin() + old, saved);
                set_carry(true);
                break;
            }
            bram_store();
            set_carry(false);
            break;
        }
        case 5: {  // BRMDEL
            uint8_t n[11];
            read_name(a0, n);
            int i = bram_find(n);
            if (i < 0) { set_carry(true); break; }
            bram_files_.erase(bram_files_.begin() + i);
            bram_store();
            set_carry(false);
            break;
        }
        case 6:  // BRMFORMAT
            bram_files_.clear();
            bram_store();
            set_carry(false);
            break;
        case 7: {  // BRMDIR: a0 = name pattern, a1 = buffer of 0x20-byte entries, d1 = skip count
            uint32_t skip = d1 & 0xFFFF, n = 0;
            for (size_t i = skip; i < bram_files_.size(); ++i, ++n) {
                for (int k = 0; k < 11; ++k) write8(a1 + n * 0x20 + k, bram_files_[i].name[k]);
                write8(a1 + n * 0x20 + 11, bram_files_[i].flag);
                write8(a1 + n * 0x20 + 12, bram_files_[i].blocks >> 8);
                write8(a1 + n * 0x20 + 13, bram_files_[i].blocks & 0xFF);
            }
            reg_set(M68K_REG_D0, n);
            set_carry(false);
            break;
        }
        case 8: {  // BRMVERIFY
            uint8_t n[11];
            read_name(a0, n);
            int i = bram_find(n);
            bool ok = i >= 0;
            for (size_t k = 0; ok && k < bram_files_[i].data.size(); ++k)
                ok = bram_files_[i].data[k] == uint8_t(read8(a1 + uint32_t(k)));
            set_carry(!ok);
            break;
        }
        default:
            log("[BIOS] unimplemented _BURAM function %d\n", fn);
            set_carry(true);
            break;
    }
}


void System::dump_state() {
    Cpu keep = cur_;
    for (int i = 0; i < 2; ++i) {
        select_cpu(Cpu(i));
        log("%s PC=%06x SR=%04x SP=%06x D0=%08x A0=%08x A6=%08x\n", i ? "Sub " : "Main", reg_get(M68K_REG_PC) & 0xFFFFFF,
            reg_get(M68K_REG_SR), reg_get(M68K_REG_SP) & 0xFFFFFF, reg_get(M68K_REG_D0), reg_get(M68K_REG_A0), reg_get(M68K_REG_A6));
    }
    select_cpu(keep);
    log("VDP regs:");
    for (int i = 0; i < 24; ++i) log(" %02x", vdp_.reg(i));
    log("  ffef02=%02x%02x\n", main_ram_[0xef02], main_ram_[0xef03]);
    log("sub_run=%d owner=%d mode1m=%d main_flag=%02x sub_flag=%02x int_mask=%04x ien2=%d prg_bank=%d\n", sub_run_, word_owner_, mode_1m_,
        main_flag_, sub_flag_, int_mask_, ien2_, prg_bank_);
}

void System::dump_profile(int top) {
    for (int i = 0; i < 2; ++i) {
        std::vector<std::pair<int, uint32_t>> v;
        for (auto& kv : pc_hist_[i]) v.push_back({kv.second, kv.first});
        std::sort(v.rbegin(), v.rend());
        log("%s CPU top PCs:", i ? "Sub " : "Main");
        for (int k = 0; k < top && k < int(v.size()); ++k) log(" %06x:%d", v[k].second, v[k].first);
        log("\n");
    }
}

// ------------------------------------------------------------------ audio

void System::cdda_fetch() {
    if (cdda_lba_ >= cdda_end_) {
        if (cdda_loop_) cdda_lba_ = disc_->tracks()[cdda_track_ - 1].start_lba;
        else { cdda_playing_ = false; return; }
    }
    if (!disc_->read_raw(cdda_lba_, cdda_raw_)) {
        cdda_playing_ = false;
        return;
    }
    ++cdda_lba_;
    cdda_pos_ = 0;
}

void System::audio_run(int samples) {
    for (int i = 0; i < samples; ++i) {
        // YM2612: 53267 Hz, 24 Nuked clocks per sample.
        ym_phase_ += 53267.0 / kSampleRate;
        while (ym_phase_ >= 1.0) {
            ym_phase_ -= 1.0;
            int16_t buf[2] = {0, 0};
            for (int k = 0; k < 24; ++k) OPN2_Clock(&ym_, buf);
            ym_l_ = buf[0];
            ym_r_ = buf[1];
        }
        pcm_phase_ += 32552.0 / kSampleRate;
        while (pcm_phase_ >= 1.0) {
            pcm_phase_ -= 1.0;
            pcm_.clock(&pcm_l_, &pcm_r_);
        }
        psg_phase_ += 223722.0 / kSampleRate;
        int psg_n = 0, psg_acc = 0;
        while (psg_phase_ >= 1.0) {
            psg_phase_ -= 1.0;
            psg_acc += psg_.clock();
            ++psg_n;
        }
        if (psg_n) psg_s_ = int16_t(psg_acc / psg_n);

        int l = ym_l_ + pcm_l_ + psg_s_ / 2;
        int r = ym_r_ + pcm_r_ + psg_s_ / 2;
        if (cdda_playing_) {
            if (cdda_pos_ >= kRawSector) cdda_fetch();
            if (cdda_playing_) {
                // 44.1 kHz source consumed with a simple accumulator.
                static double phase = 0;
                phase += 44100.0 / kSampleRate;
                int16_t sl = int16_t(cdda_raw_[cdda_pos_] | cdda_raw_[cdda_pos_ + 1] << 8);
                int16_t sr = int16_t(cdda_raw_[cdda_pos_ + 2] | cdda_raw_[cdda_pos_ + 3] << 8);
                l += sl;
                r += sr;
                while (phase >= 1.0) {
                    phase -= 1.0;
                    cdda_pos_ += 4;
                    if (cdda_pos_ >= kRawSector) break;
                }
            }
        }
        audio_.push_back(int16_t(std::clamp(l, -32768, 32767)));
        audio_.push_back(int16_t(std::clamp(r, -32768, 32767)));
    }
}

}  // namespace scd
