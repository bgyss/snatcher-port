// Ricoh RF5C164 PCM sound source (Sega CD).
#pragma once
#include <cstdint>

namespace scd {

class Pcm {
public:
    Pcm();
    void reset();
    uint8_t read(uint32_t addr);               // addr relative to 0xFF0000
    void write(uint32_t addr, uint8_t value);
    // Produces one output sample (32.552 kHz) for the stereo pair.
    void clock(int16_t* left, int16_t* right);
    // DMA helper: writes one byte at the current wave RAM bank + offset.
    uint8_t* wave_ram() { return ram_; }
    int bank() const { return bank_; }

private:
    struct Channel {
        uint8_t env = 0, pan = 0xFF;
        uint16_t fd = 0, ls = 0;
        uint8_t st = 0;
        uint32_t addr = 0;  // 16.11 fixed point
    };
    Channel ch_[8];
    uint8_t ram_[0x10000];
    uint8_t ctrl_ = 0;
    uint8_t enable_ = 0xFF;  // bit set = channel off
    int bank_ = 0;
    int sel_ = 0;
    bool on_ = false;
};

}  // namespace scd
