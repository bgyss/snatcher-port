// SN76489 programmable sound generator.
#pragma once
#include <cstdint>

namespace scd {

class Psg {
public:
    Psg() { reset(); }
    void reset();
    void write(uint8_t v);
    // One sample at clock/16 (223.7 kHz); caller resamples.
    int16_t clock();

private:
    uint16_t tone_[3];
    uint8_t vol_[4];
    uint8_t noise_ctl_;
    uint16_t counter_[4];
    uint8_t out_[4];
    uint16_t lfsr_;
    uint8_t latch_;
};

}  // namespace scd
