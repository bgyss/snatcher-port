// Mega Drive VDP (Mode 5). Scanline renderer; DMA is instantaneous.
#pragma once
#include <cstdint>
#include <functional>

namespace scd {

class Vdp {
public:
    // Reads a word from the 68000 bus for 68K->VDP DMA.
    std::function<uint16_t(uint32_t)> dma_read;
    // Called whenever the interrupt level the VDP asserts may have changed.
    std::function<void()> irq_changed;

    Vdp();
    void reset();

    uint16_t read_data();
    uint16_t read_status();
    uint16_t read_hv(int line, int cycle_in_line, int cycles_per_line);
    void write_data(uint16_t v);
    void write_ctrl(uint16_t v);

    // Called once per scanline before CPU time runs for that line.
    void begin_line(int line);
    // Main CPU cycles the last 68K->VDP DMA kept the bus busy (returned once).
    int take_dma_stall() { int s = dma_stall_; dma_stall_ = 0; return s; }
    // Called ~110 Main CPU cycles into a line: the V-INT request trails the VBLANK flag.
    void line_progress();
    // Renders one active line into the framebuffer.
    void render_line(int line);

    // 0 = no interrupt; otherwise the 68000 level (6 = VINT, 4 = HINT).
    int irq_level() const;
    void irq_ack(int level);

    int width() const { return (reg_[12] & 0x81) ? 320 : 256; }
    int height() const { return (reg_[1] & 0x08) ? 240 : 224; }
    bool display_enabled() const { return reg_[1] & 0x40; }
    const uint32_t* framebuffer() const { return fb_; }  // 320 wide, 240 tall, 0x00RRGGBB
    int active_lines() const { return height(); }

    // Debug access.
    const uint8_t* vram() const { return vram_; }
    const uint16_t* cram() const { return cram_; }
    const uint16_t* vsram() const { return vsram_; }
    uint8_t reg(int n) const { return reg_[n]; }

private:
    uint8_t read_vram_byte(uint32_t a) const { return vram_[a & 0xFFFF]; }
    uint16_t fetch_word(uint8_t code, uint32_t addr) const;
    void write_word(uint16_t v);
    void do_dma();
    void dma_fill(uint16_t v);
    void dma_copy();
    void update_irq() { if (irq_changed) irq_changed(); }

    struct Px { uint8_t idx; bool prio; };  // idx = pal<<4|color, 0 low nibble = transparent
    void render_plane(int plane, int line, Px* out, const bool* window_mask);
    void render_sprites(int line, Px* out, bool* is_hi);
    uint32_t color_rgb(int idx, int mode) const;  // mode: 0 normal, 1 shadow, 2 highlight

    uint8_t vram_[0x10000];
    uint16_t cram_[64];
    uint16_t vsram_[64];
    uint8_t reg_[32];

    bool cmd_pending_ = false;
    uint16_t cmd_first_ = 0;
    uint8_t code_ = 0;
    uint16_t addr_ = 0;
    uint16_t read_buf_ = 0;
    bool dma_fill_pending_ = false;
    bool vint_pending_ = false;
    bool vint_arm_ = false;
    int dma_stall_ = 0;
    bool hint_pending_ = false;
    bool vblank_ = false;
    bool hblank_ = false;
    bool sprite_overflow_ = false;
    bool sprite_collision_ = false;
    bool odd_frame_ = false;
    int hint_counter_ = 0;
    int cur_line_ = 0;

    uint32_t fb_[320 * 240];
};

}  // namespace scd
