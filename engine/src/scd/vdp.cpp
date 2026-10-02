#include "vdp.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {
int layer_mask() {
    static int m = std::getenv("SCD_LAYERS") ? std::atoi(std::getenv("SCD_LAYERS")) : 7;  // 1=B 2=A 4=sprites
    return m;
}
}  // namespace

namespace scd {

uint32_t current_pc_for_debug();

namespace {
uint32_t g_watch_lo = 1, g_watch_hi = 0;
void init_watch() {
    static bool done = false;
    if (done) return;
    done = true;
    if (const char* s = std::getenv("SCD_VRAMWATCH")) std::sscanf(s, "%x-%x", &g_watch_lo, &g_watch_hi);
}
}  // namespace

Vdp::Vdp() { reset(); }

void Vdp::reset() {
    std::memset(vram_, 0, sizeof vram_);
    std::memset(cram_, 0, sizeof cram_);
    std::memset(vsram_, 0, sizeof vsram_);
    std::memset(reg_, 0, sizeof reg_);
    std::memset(fb_, 0, sizeof fb_);
    cmd_pending_ = false;
    code_ = 0;
    addr_ = 0;
    read_buf_ = 0;
    dma_fill_pending_ = vint_pending_ = vint_arm_ = hint_pending_ = vblank_ = hblank_ = false;
    sprite_overflow_ = sprite_collision_ = odd_frame_ = false;
    hint_counter_ = 0;
    cur_line_ = 0;
    reg_[15] = 2;
}

// ---------------------------------------------------------------- ports

uint16_t Vdp::fetch_word(uint8_t code, uint32_t addr) const {
    switch (code & 0x0F) {
        case 0x00: return uint16_t(vram_[addr & 0xFFFF] << 8 | vram_[(addr + 1) & 0xFFFF]);
        case 0x04: return vsram_[(addr >> 1) & 0x3F];
        case 0x08: return cram_[(addr >> 1) & 0x3F];
    }
    return 0;
}

uint16_t Vdp::read_data() {
    cmd_pending_ = false;
    uint16_t r = fetch_word(code_, addr_ & ~1u);
    addr_ = uint16_t(addr_ + reg_[15]);
    return r;
}

uint16_t Vdp::read_status() {
    cmd_pending_ = false;
    uint16_t s = 0x3400;  // FIFO empty (bit 9) plus fixed pattern bits
    s = 0x0200;
    if (vint_pending_) s |= 0x80;
    if (sprite_overflow_) s |= 0x40;
    if (sprite_collision_) s |= 0x20;
    if (odd_frame_) s |= 0x10;
    if (vblank_ || !display_enabled()) s |= 0x08;
    if (hblank_) s |= 0x04;
    vint_pending_ = vint_pending_;  // cleared only by acknowledge
    sprite_overflow_ = sprite_collision_ = false;
    return s;
}

uint16_t Vdp::read_hv(int line, int cycle_in_line, int cycles_per_line) {
    int v = line;
    if (v > 0xEA) v -= 6;  // NTSC V28 counter jump (262-line frame)
    int h = cycle_in_line * 0xB6 / cycles_per_line;
    if (h > 0x93) h += 0xE9 - 0x93;  // H40 counter skip
    return uint16_t((v & 0xFF) << 8 | (h & 0xFF));
}

void Vdp::write_word(uint16_t v) {
    init_watch();
    if ((code_ & 0x0F) == 1 && addr_ >= g_watch_lo && addr_ <= g_watch_hi)
        std::fprintf(stderr, "[vram] pc=%06x addr=%04x val=%04x (line %d)\n", current_pc_for_debug(), addr_, v, cur_line_);
    switch (code_ & 0x0F) {
        case 0x01:
            if (addr_ & 1) v = uint16_t(v << 8 | v >> 8);
            vram_[addr_ & 0xFFFE] = uint8_t(v >> 8);
            vram_[(addr_ & 0xFFFE) | 1] = uint8_t(v);
            break;
        case 0x03: cram_[(addr_ >> 1) & 0x3F] = v & 0x0EEE; break;
        case 0x05: vsram_[(addr_ >> 1) & 0x3F] = v & 0x07FF; break;
        default: break;
    }
    addr_ = uint16_t(addr_ + reg_[15]);
}

void Vdp::write_data(uint16_t v) {
    if (std::getenv("SCD_VDPPORT")) std::fprintf(stderr, "D %04x pc=%06x\n", v, current_pc_for_debug());
    cmd_pending_ = false;
    if (dma_fill_pending_) {
        dma_fill_pending_ = false;
        dma_fill(v);
        return;
    }
    write_word(v);
}

void Vdp::write_ctrl(uint16_t v) {
    if (std::getenv("SCD_VDPPORT")) std::fprintf(stderr, "C %04x pc=%06x\n", v, current_pc_for_debug());
    if (cmd_pending_) {
        cmd_pending_ = false;
        code_ = uint8_t((code_ & 0x03) | ((v >> 2) & 0x3C));
        addr_ = uint16_t((cmd_first_ & 0x3FFF) | ((v & 3) << 14));
        if ((code_ & 0x20) && (reg_[1] & 0x10)) {
            do_dma();
        }
        return;
    }
    if ((v & 0xE000) == 0x8000) {
        int r = (v >> 8) & 0x1F;
        reg_[r] = uint8_t(v);
        if (r == 0 || r == 1) update_irq();
        return;
    }
    cmd_first_ = v;
    cmd_pending_ = true;
    code_ = uint8_t(v >> 14);
    addr_ = uint16_t((addr_ & 0xC000) | (v & 0x3FFF));
}

// ------------------------------------------------------------------ DMA

void Vdp::do_dma() {
    uint32_t len = reg_[19] | (reg_[20] << 8);
    if (len == 0) len = 0x10000;
    uint32_t src = (reg_[21] | (reg_[22] << 8) | ((reg_[23] & 0x7F) << 16));
    int type = reg_[23] >> 6;
    if (type == 2) {  // VRAM fill: waits for a data port write
        dma_fill_pending_ = true;
        return;
    }
    if (type == 3) {  // VRAM copy
        dma_copy();
        return;
    }
    if (std::getenv("SCD_VDPLOG"))
        std::fprintf(stderr, "[vdp dma] line %d code=%02x addr=%04x src=%06x len=%u\n", cur_line_, code_, addr_, src << 1, len);
    uint32_t a = src << 1;
    // Sega CD: a DMA that reads Word RAM is one word late: the first word written is stale and the last
    // source word is never transferred. The IP compensates (source + 2, plus a fix-up write after the DMA).
    const bool word_ram_src = a >= 0x200000 && a < 0x240000;
    uint16_t delayed = 0;
    for (uint32_t i = 0; i < len; ++i) {
        uint16_t w = dma_read ? dma_read(a) : 0;
        if (word_ram_src) std::swap(w, delayed);
        write_word(w);
        a = (a & 0xFE0000) | ((a + 2) & 0x1FFFF);
    }
    src = (a >> 1) & 0x7FFFFF;
    reg_[21] = uint8_t(src);
    reg_[22] = uint8_t(src >> 8);
    reg_[19] = reg_[20] = 0;
}

void Vdp::dma_fill(uint16_t v) {
    uint32_t len = reg_[19] | (reg_[20] << 8);
    if (len == 0) len = 0x10000;
    // The first write goes through the normal path, then the high byte fills.
    write_word(v);
    uint8_t hi = uint8_t(v >> 8);
    for (uint32_t i = 0; i < len; ++i) {
        vram_[(addr_ ^ 1) & 0xFFFF] = hi;
        addr_ = uint16_t(addr_ + reg_[15]);
    }
    reg_[19] = reg_[20] = 0;
}

void Vdp::dma_copy() {
    uint32_t len = reg_[19] | (reg_[20] << 8);
    if (len == 0) len = 0x10000;
    uint16_t src = uint16_t(reg_[21] | (reg_[22] << 8));
    for (uint32_t i = 0; i < len; ++i) {
        vram_[addr_] = vram_[src];
        src = uint16_t(src + 1);
        addr_ = uint16_t(addr_ + reg_[15]);
    }
    reg_[21] = uint8_t(src);
    reg_[22] = uint8_t(src >> 8);
    reg_[19] = reg_[20] = 0;
}

// ------------------------------------------------------------ interrupts

int Vdp::irq_level() const {
    if (vint_pending_ && (reg_[1] & 0x20)) return 6;
    if (hint_pending_ && (reg_[0] & 0x10)) return 4;
    return 0;
}

void Vdp::irq_ack(int level) {
    if (level == 6) vint_pending_ = false;
    else if (level == 4) hint_pending_ = false;
}

void Vdp::begin_line(int line) {
    cur_line_ = line;
    int active = active_lines();
    if (line == 0) {
        hint_counter_ = reg_[10];
        vblank_ = false;
        odd_frame_ = !odd_frame_;
    }
    if (line < active) {
        if (hint_counter_ == 0) {
            hint_pending_ = true;
            hint_counter_ = reg_[10];
        } else {
            --hint_counter_;
        }
    } else if (line == active) {
        vblank_ = true;
        vint_arm_ = true;
    }
    update_irq();
}

void Vdp::line_progress() {
    if (vint_arm_) {
        vint_arm_ = false;
        vint_pending_ = true;
        update_irq();
    }
}

// ------------------------------------------------------------- rendering

uint32_t Vdp::color_rgb(int idx, int mode) const {
    uint16_t c = cram_[idx & 0x3F];
    int r = (c >> 1) & 7, g = (c >> 5) & 7, b = (c >> 9) & 7;
    int sc, off;
    switch (mode) {
        case 1: sc = 18; off = 0; break;      // shadow
        case 2: sc = 18; off = 126; break;    // highlight
        default: sc = 36; off = 0; break;
    }
    return uint32_t((r * sc + off) << 16 | (g * sc + off) << 8 | (b * sc + off));
}

void Vdp::render_plane(int plane, int line, Px* out, const bool* window_mask) {
    const bool h40 = width() == 320;
    const int cells = h40 ? 40 : 32;
    static const int kSizes[4] = {32, 64, 32, 128};
    const int pw = kSizes[reg_[16] & 3];
    const int ph = kSizes[(reg_[16] >> 4) & 3];
    const bool is_a = plane == 0;
    const uint32_t nt = is_a ? uint32_t(reg_[2] & 0x38) << 10 : uint32_t(reg_[4] & 0x07) << 13;
    const uint32_t wnt = (h40 ? uint32_t(reg_[3] & 0x3C) : uint32_t(reg_[3] & 0x3E)) << 10;

    // Horizontal scroll.
    const uint32_t hs_base = uint32_t(reg_[13] & 0x3F) << 10;
    int hmode = reg_[11] & 3;
    uint32_t hs_addr = hs_base;
    if (hmode == 2) hs_addr += uint32_t(line & ~7) * 4;
    else if (hmode == 3) hs_addr += uint32_t(line) * 4;
    hs_addr += is_a ? 0 : 2;
    int hscroll = ((vram_[hs_addr & 0xFFFF] << 8) | vram_[(hs_addr + 1) & 0xFFFF]) & 0x3FF;

    const bool vcol = reg_[11] & 4;

    for (int x = 0; x < cells * 8; ++x) {
        Px& o = out[x];
        o.idx = 0;
        o.prio = false;
        const bool in_window = is_a && window_mask[x];
        uint32_t base;
        int px, py, pwc, phc;
        if (in_window) {
            base = wnt;
            px = x;
            py = line;
            pwc = h40 ? 64 : 32;
            phc = 32;
        } else {
            int vs = vcol ? vsram_[(x >> 4) * 2 + (is_a ? 0 : 1)] : vsram_[is_a ? 0 : 1];
            vs &= 0x3FF;
            px = (x - hscroll) & (pw * 8 - 1);
            py = (line + vs) & (ph * 8 - 1);
            base = nt;
            pwc = pw;
            phc = ph;
        }
        (void)phc;
        int cx = px >> 3, cy = py >> 3;
        uint32_t ea = base + uint32_t((cy * pwc + cx) * 2);
        uint16_t e = uint16_t(vram_[ea & 0xFFFF] << 8 | vram_[(ea + 1) & 0xFFFF]);
        int row = py & 7, col = px & 7;
        if (e & 0x1000) row = 7 - row;
        if (e & 0x0800) col = 7 - col;
        uint32_t ta = uint32_t(e & 0x7FF) * 32 + row * 4 + (col >> 1);
        uint8_t b = vram_[ta & 0xFFFF];
        int c = (col & 1) ? (b & 0x0F) : (b >> 4);
        if (c) {
            o.idx = uint8_t(((e >> 13) & 3) << 4 | c);
            o.prio = (e & 0x8000) != 0;
        }
    }
}

void Vdp::render_sprites(int line, Px* out, bool* is_hi) {
    const bool h40 = width() == 320;
    const int max_sprites = h40 ? 80 : 64;
    const int max_line = h40 ? 20 : 16;
    const int max_pixels = h40 ? 320 : 256;
    const uint32_t sat = (h40 ? uint32_t(reg_[5] & 0x7E) : uint32_t(reg_[5] & 0x7F)) << 9;
    const int screen_w = h40 ? 320 : 256;

    int idx = 0, visited = 0, on_line = 0, pixels = 0;
    bool masked_ok = false;
    for (;;) {
        uint32_t a = sat + idx * 8;
        uint16_t w0 = uint16_t(vram_[a & 0xFFFF] << 8 | vram_[(a + 1) & 0xFFFF]);
        uint16_t w1 = uint16_t(vram_[(a + 2) & 0xFFFF] << 8 | vram_[(a + 3) & 0xFFFF]);
        uint16_t w2 = uint16_t(vram_[(a + 4) & 0xFFFF] << 8 | vram_[(a + 5) & 0xFFFF]);
        uint16_t w3 = uint16_t(vram_[(a + 6) & 0xFFFF] << 8 | vram_[(a + 7) & 0xFFFF]);
        int y = (w0 & 0x3FF) - 128;
        int hs = (w1 >> 10) & 3, vs = (w1 >> 8) & 3;
        int link = w1 & 0x7F;
        int height = (vs + 1) * 8;
        if (line >= y && line < y + height) {
            if (on_line >= max_line) {
                sprite_overflow_ = true;
                break;
            }
            ++on_line;
            int x = (w3 & 0x1FF) - 128;
            if ((w3 & 0x1FF) == 0 && masked_ok) break;  // sprite mask
            masked_ok = true;
            int wc = hs + 1;
            int ly = line - y;
            if (w2 & 0x1000) ly = height - 1 - ly;
            int cy = ly >> 3, row = ly & 7;
            for (int cxi = 0; cxi < wc; ++cxi) {
                int cx = (w2 & 0x0800) ? wc - 1 - cxi : cxi;
                uint32_t tile = uint32_t((w2 & 0x7FF) + cx * (vs + 1) + cy);
                for (int col = 0; col < 8; ++col) {
                    if (++pixels > max_pixels) goto done;
                    int sx = x + cxi * 8 + col;
                    if (sx < 0 || sx >= screen_w) continue;
                    int c8 = (w2 & 0x0800) ? 7 - col : col;
                    uint8_t b = vram_[(tile * 32 + row * 4 + (c8 >> 1)) & 0xFFFF];
                    int c = (c8 & 1) ? (b & 0x0F) : (b >> 4);
                    if (!c) continue;
                    if (out[sx].idx & 0x0F) {
                        sprite_collision_ = true;
                        continue;
                    }
                    out[sx].idx = uint8_t(((w2 >> 13) & 3) << 4 | c);
                    out[sx].prio = (w2 & 0x8000) != 0;
                    is_hi[sx] = out[sx].prio;
                }
            }
        }
        idx = link;
        if (idx == 0 || ++visited >= max_sprites) break;
    }
done:;
}

void Vdp::render_line(int line) {
    if (std::getenv("SCD_LINELOG") && line % 56 == 0)
        std::fprintf(stderr, "[line %d] r0=%02x r1=%02x r11=%02x r12=%02x vs=%03x %03x %03x %03x %03x %03x\n", line, reg_[0], reg_[1], reg_[11], reg_[12], vsram_[0], vsram_[1], vsram_[2], vsram_[3], vsram_[4], vsram_[5]);
    const int w = width();
    uint32_t* dst = fb_ + line * 320;
    if (!display_enabled()) {
        uint32_t bg = color_rgb((reg_[7] & 0x3F), 0);
        for (int x = 0; x < w; ++x) dst[x] = bg;
        return;
    }
    Px pa[320], pb[320], ps[320];
    bool shi[320];
    bool wmask[320];
    std::memset(ps, 0, sizeof ps);
    std::memset(shi, 0, sizeof shi);

    // Window region (plane A replacement).
    int wh = (reg_[17] & 0x1F) * 16, wv = (reg_[18] & 0x1F) * 8;
    bool right = reg_[17] & 0x80, down = reg_[18] & 0x80;
    bool vwin = down ? line >= wv : line < wv;
    for (int x = 0; x < w; ++x) {
        bool hwin = right ? x >= wh : x < wh;
        wmask[x] = vwin || hwin;
    }
    // A zero-sized window disables it entirely.
    if (!(reg_[17] & 0x9F) && !(reg_[18] & 0x9F)) std::memset(wmask, 0, sizeof wmask);

    std::memset(pa, 0, sizeof pa);
    std::memset(pb, 0, sizeof pb);
    if (layer_mask() & 1) render_plane(1, line, pb, wmask);
    if (layer_mask() & 2) render_plane(0, line, pa, wmask);
    if (layer_mask() & 4) render_sprites(line, ps, shi);

    const bool sh = reg_[12] & 0x08;
    const int bg_idx = reg_[7] & 0x3F;
    for (int x = 0; x < w; ++x) {
        int color = bg_idx;
        bool shadow = sh, highlight = false;
        auto paint_plane = [&](const Px& p, bool want_hi) {
            if ((p.idx & 0x0F) && p.prio == want_hi) {
                color = p.idx;
                shadow = sh && !want_hi;
                highlight = false;
            }
        };
        auto paint_sprite = [&](bool want_hi) {
            const Px& p = ps[x];
            if (!(p.idx & 0x0F) || p.prio != want_hi) return;
            if (sh && (p.idx & 0x3F) == 0x3E) {  // palette 3, colour 14: highlight
                if (shadow) shadow = false; else highlight = true;
                return;
            }
            if (sh && (p.idx & 0x3F) == 0x3F) {  // palette 3, colour 15: shadow
                shadow = true;
                highlight = false;
                return;
            }
            color = p.idx;
            shadow = sh && !want_hi;
            highlight = false;
        };
        paint_plane(pb[x], false);
        paint_plane(pa[x], false);
        paint_sprite(false);
        paint_plane(pb[x], true);
        paint_plane(pa[x], true);
        paint_sprite(true);
        dst[x] = color_rgb(color, shadow ? 1 : (highlight ? 2 : 0));
    }
}

}  // namespace scd
