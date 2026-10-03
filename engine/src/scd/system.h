// Sega CD (Mega CD) system: Main 68000 + Sub 68000 + VDP + Gate Array + PCM,
// with the BIOS (boot ROM, _CDBIOS, _BURAM) emulated in host code.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "disc.h"
#include "pcm.h"
#include "psg.h"
#include "vdp.h"

extern "C" {
#include "ym3438.h"
}

namespace scd {

enum Button : uint16_t {
    kUp = 1 << 0, kDown = 1 << 1, kLeft = 1 << 2, kRight = 1 << 3,
    kB = 1 << 4, kC = 1 << 5, kA = 1 << 6, kStart = 1 << 7,
};

constexpr int kSampleRate = 48000;

class System {
public:
    static System& instance();

    // Loads IP/SP from the disc and resets. Returns false with *error set on failure.
    bool init(std::unique_ptr<Disc> disc, const std::string& save_path, std::string* error);
    void shutdown();  // flushes backup RAM
    void run_frame();

    int width() const { return vdp_.width(); }
    int height() const { return vdp_.height(); }
    const uint32_t* framebuffer() const { return vdp_.framebuffer(); }
    void set_pad(int port, uint16_t buttons) { pad_[port & 1] = buttons; }
    // Konami Justifier on port 2. x,y are in game screen pixels; buttons: bit0 trigger, bit1 start.
    void set_gun_connected(bool on) { gun_connected_ = on; }
    void set_gun(int x, int y, bool inside, uint8_t buttons) { gun_x_ = x; gun_y_ = y; gun_inside_ = inside; gun_buttons_ = buttons; }

    // Interleaved stereo samples produced since the last call.
    std::vector<int16_t>& audio() { return audio_; }

    // Debug.
    uint64_t frame_count() const { return frames_; }
    uint8_t main_flag() const { return main_flag_; }
    uint8_t sub_flag() const { return sub_flag_; }
    std::string comm_string() const;
    uint64_t ym_write_count() const { return ym_writes_; }
    bool translated() const { return translate_; }
    uint64_t fallback_steps() const { return fallback_steps_; }
    uint64_t verified_count() const { return verified_; }
    bool halted() const { return halted_; }
    uint8_t* main_ram() { return main_ram_; }
    uint8_t* prg_ram() { return prg_ram_; }
    uint8_t* word_ram() { return word_ram_; }
    Vdp& vdp() { return vdp_; }
    // Prints PCs and key registers of both CPUs to stderr.
    void dump_state();
    bool bram_selftest();
    void enable_profile(bool on) { profile_ = on; pc_hist_[0].clear(); pc_hist_[1].clear(); }
    void dump_profile(int top);
    void dump_overlay_hist();
    // Per-instruction trace of one CPU (0 = Main, 1 = Sub) starting at a frame.
    void set_trace(int cpu, uint64_t from_frame, int count) { trace_cpu_ = cpu; trace_from_ = from_frame; trace_left_ = count; }
    void set_trace_from(uint64_t f) { trace_from_ = f; }
    void trace_pc(uint32_t pc);

    // Memory callbacks (Musashi).
    uint32_t read8(uint32_t a);
    uint32_t read16(uint32_t a);
    void write8(uint32_t a, uint32_t v);
    void write16(uint32_t a, uint32_t v);
    int illegal(int opcode);
    int int_ack(int level);

private:
    System() = default;
    enum Cpu { kMain = 0, kSub = 1 };
    friend struct SubBusAdapter;
    friend struct MainBusAdapter;
    int run_sub_translated(int cycles);
    uint32_t prg_ovl_gen_ = 0;
    int run_main_translated(int cycles);
    int32_t* main_budget_ = nullptr;
    bool main_span_checked_ = false, main_span_ok_ = false;
    bool translate_ = false;
    bool span_ok_[4] = {false, false, false, false};
    bool span_checked_[4] = {false, false, false, false};
    uint64_t translated_instr_budget_ = 0, fallback_steps_ = 0, verified_ = 0;
    uint32_t exit_ring_[16] = {};
    uint32_t exit_pos_ = 0;

    // CPU scheduling
    void select_cpu(Cpu c);
    void run_cpu(Cpu c, int cycles);
    void refresh_irq(Cpu c);
    void update_main_irq();
    void update_sub_irq();
    int sub_irq_level() const;
    int main_irq_level() const;
    void advance_sub_time(int cycles);

    // Memory maps
    uint32_t main_read8(uint32_t a);
    uint32_t main_read16(uint32_t a);
    void main_write8(uint32_t a, uint32_t v);
    void main_write16(uint32_t a, uint32_t v);
    uint32_t sub_read8(uint32_t a);
    void sub_write8(uint32_t a, uint32_t v);
    uint8_t ga_read(Cpu who, uint32_t off);
    void ga_write(Cpu who, uint32_t off, uint8_t v);
    uint8_t io_read(uint32_t reg);
    void io_write(uint32_t reg, uint8_t v);
    uint8_t* word_ram_ptr(Cpu who, uint32_t off);  // nullptr when the CPU does not own it
    void ym_write(int port, uint8_t v);

    // HLE BIOS
    void build_main_rom();
    void build_sub_bios();
    void hle_trap(uint32_t pc);
    void cdbios(int fn);
    void buram(int fn);
    struct BramFile { uint8_t name[11]; uint8_t flag; uint16_t blocks; std::vector<uint8_t> data; };
    void bram_load();
    void bram_store();
    int bram_find(const uint8_t* name) const;
    int bram_free_blocks() const;
    std::vector<BramFile> bram_files_;
    bool cdc_load_sector();
    void cdc_do_transfer();
    void set_carry(bool c);
    uint32_t reg_get(int r);
    void reg_set(int r, uint32_t v);

    // Audio
    void audio_run(int samples);
    int16_t cdda_sample(bool right);
    void cdda_fetch();

    std::unique_ptr<Disc> disc_;
    std::string save_path_;
    Vdp vdp_;
    Pcm pcm_;
    Psg psg_;
    ym3438_t ym_;

    // 68000 contexts
    std::vector<uint8_t> ctx_[2];
    Cpu cur_ = kMain;
    int cpu_irq_[2] = {0, 0};
    bool irq_dirty_[2] = {false, false};
    int cpu_debt_[2] = {0, 0};
    int cpu_scratch_ = 0;

    // Memory
    std::vector<uint8_t> main_rom_;       // 128 KB stub BIOS
    uint8_t main_ram_[0x10000];
    uint8_t prg_ram_[0x80000];
    uint8_t word_ram_[0x40000];
    uint8_t bram_[0x2000];
    uint8_t z80_ram_[0x2000];
    bool bram_dirty_ = false;

    // Gate array state
    uint8_t cmd_[16] = {}, stat_[16] = {};
    uint8_t main_flag_ = 0, sub_flag_ = 0;
    bool ien2_ = false;             // Main: level 2 from Sub enabled
    bool sub_run_ = true;
    uint8_t write_protect_ = 0;
    int prg_bank_ = 0;
    bool mode_1m_ = false;
    int word_owner_ = 0;            // 2M: 0 = Main, 1 = Sub
    uint8_t cdc_dest_ = 0;
    bool cdc_edt_ = false, cdc_dsr_ = false;
    uint16_t dma_addr_ = 0;
    uint16_t hint_vec_ = 0xFD0C;
    uint8_t timer_reg_ = 0;
    int timer_count_ = 0;
    uint16_t int_mask_ = 0x0004;  // BIOS leaves level 2 (Main -> Sub) enabled
    int sub_pending_ = 0;           // bit n = level n pending
    bool main_int2_pending_ = false;
    uint32_t stopwatch_base_ = 0;
    int sub_tick_accum_ = 0;
    uint32_t stopwatch_ = 0;

    // Z80 bus (not emulated)
    bool z80_busreq_ = false;
    bool z80_reset_ = true;

    // Controllers
    uint16_t pad_[2] = {0, 0};
    bool gun_connected_ = false, gun_inside_ = false;
    int gun_x_ = 0, gun_y_ = 0;
    uint8_t gun_buttons_ = 0;
    uint8_t io_data_[3] = {0x7F, 0x7F, 0x7F};
    uint8_t io_ctrl_[3] = {0, 0, 0};

    // CDC / CDD
    bool cdc_reading_ = false;
    uint32_t cdc_lba_ = 0;
    uint32_t cdc_remaining_ = 0;
    bool cdc_buf_valid_ = false;
    uint64_t sub_cycles_ = 0;        // total Sub CPU cycles run (CD timing base)
    uint64_t cdc_ready_at_ = 0;      // sub cycle when the next sector is deliverable
    uint32_t cdc_last_lba_ = 0;
    bool sub_wait_vsync_ = false;    // _WAITVSYNC: Sub CPU parked until the next V-blank
    uint8_t cdc_buf_[kUserData];
    uint8_t cdc_header_[4] = {};
    bool cdda_playing_ = false;
    uint32_t cdda_lba_ = 0, cdda_end_ = 0;
    bool cdda_loop_ = false;
    int cdda_track_ = 0;
    uint8_t cdda_raw_[kRawSector];
    int cdda_pos_ = kRawSector;

    // Timing / audio
    uint64_t frames_ = 0;
    int line_ = 0;
    int main_cycle_in_line_ = 0;
    bool halted_ = false;
    std::vector<int16_t> audio_;
    double audio_frac_ = 0;
    double ym_phase_ = 0, pcm_phase_ = 0, psg_phase_ = 0;
    int16_t ym_l_ = 0, ym_r_ = 0, pcm_l_ = 0, pcm_r_ = 0, psg_s_ = 0;

    bool trace_bios_ = false;
    bool profile_ = false;
    bool ovl_log_ = false;
    uint32_t cur_load_lba_ = 0;
    std::map<uint32_t, uint64_t> ovl_hist_;
    bool trace_audio_ = false;
    uint64_t ym_writes_ = 0;
    uint32_t watch_ = 0, stack_at_ = 0;
    int stack_left_ = 20;
    int trace_cpu_ = -1, trace_left_ = 0;
    uint64_t trace_from_ = 0;
    std::map<uint32_t, int> pc_hist_[2];
    std::map<uint32_t, int> hle_;  // trap address -> id
};

}  // namespace scd
