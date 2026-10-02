// Headless runner: boots a disc for N frames, optionally dumping frames and audio.
//   scd_headless <disc.cue> [--frames N] [--ppm-every N --out DIR] [--wav out.wav]
//                [--press FRAME:BUTTONS:HOLD ...]
// BUTTONS is a string of U D L R B C A S. Output stays in the directory given by --out.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "system.h"

using namespace scd;

namespace {

void write_ppm(const std::string& path, const uint32_t* fb, int w, int h) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint32_t p = fb[y * 320 + x];
            unsigned char rgb[3] = {uint8_t(p >> 16), uint8_t(p >> 8), uint8_t(p)};
            std::fwrite(rgb, 1, 3, f);
        }
    std::fclose(f);
}

void write_wav(const std::string& path, const std::vector<int16_t>& s) {
    FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    uint32_t data = uint32_t(s.size() * 2), rate = kSampleRate, byte_rate = rate * 4, riff = 36 + data;
    uint16_t fmt = 1, ch = 2, align = 4, bits = 16;
    uint32_t fmt_len = 16;
    std::fwrite("RIFF", 1, 4, f); std::fwrite(&riff, 4, 1, f); std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmt_len, 4, 1, f); std::fwrite(&fmt, 2, 1, f); std::fwrite(&ch, 2, 1, f);
    std::fwrite(&rate, 4, 1, f); std::fwrite(&byte_rate, 4, 1, f); std::fwrite(&align, 2, 1, f);
    std::fwrite(&bits, 2, 1, f); std::fwrite("data", 1, 4, f); std::fwrite(&data, 4, 1, f);
    std::fwrite(s.data(), 2, s.size(), f);
    std::fclose(f);
}

struct Press { int frame; uint16_t buttons; int hold; };

uint16_t parse_buttons(const std::string& s) {
    uint16_t b = 0;
    for (char c : s) switch (c) {
        case 'U': b |= kUp; break; case 'D': b |= kDown; break;
        case 'L': b |= kLeft; break; case 'R': b |= kRight; break;
        case 'B': b |= kB; break; case 'C': b |= kC; break;
        case 'A': b |= kA; break; case 'S': b |= kStart; break;
    }
    return b;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s disc.cue [--frames N] [--ppm-every N] [--out DIR] [--wav FILE] [--press F:BTNS:HOLD]\n", argv[0]);
        return 2;
    }
    std::string cue = argv[1], out = ".", wav;
    int frames = 600, ppm_every = 0;
    std::vector<Press> presses;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--ppm-every" && i + 1 < argc) ppm_every = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--wav" && i + 1 < argc) wav = argv[++i];
        else if (a == "--press" && i + 1 < argc) {
            int f, h;
            char b[16];
            if (std::sscanf(argv[++i], "%d:%15[^:]:%d", &f, b, &h) == 3) presses.push_back({f, parse_buttons(b), h});
        }
    }
    std::string err;
    auto disc = Disc::open(cue, &err);
    if (!disc) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    System& sys = System::instance();
    if (!sys.init(std::move(disc), "", &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    const int profile_from = std::getenv("SCD_PROFILE") ? std::atoi(std::getenv("SCD_PROFILE")) : -1;
    if (const char* t = std::getenv("SCD_TRACE")) {  // cpu:frame:count
        int cpu, n; unsigned long long fr;
        if (std::sscanf(t, "%d:%llu:%d", &cpu, &fr, &n) == 3) sys.set_trace(cpu, fr, n);
    }
    if (std::getenv("SCD_STACKAT") && std::getenv("SCD_FROM")) sys.set_trace_from(std::strtoull(std::getenv("SCD_FROM"), nullptr, 10));
    std::vector<int16_t> all_audio;
    for (int f = 0; f < frames && !sys.halted(); ++f) {
        uint16_t pad = 0;
        for (auto& p : presses) if (f >= p.frame && f < p.frame + p.hold) pad |= p.buttons;
        if (profile_from >= 0 && f == frames - profile_from) sys.enable_profile(true);
        sys.set_pad(0, pad);
        sys.run_frame();
        if (!wav.empty()) {
            all_audio.insert(all_audio.end(), sys.audio().begin(), sys.audio().end());
        }
        sys.audio().clear();
        if (std::getenv("SCD_STATE")) {
            const uint8_t* m = sys.main_ram();
            const uint8_t* p = sys.prg_ram();
            std::printf("%d e028=%02x e022=%04x e020=%04x s7840=%04x\n", f + 1, m[0xE028], m[0xE022] << 8 | m[0xE023], m[0xE020] << 8 | m[0xE021], p[0x7840] << 8 | p[0x7841]);
        }
        if (const char* s = std::getenv("SCD_STOP_E020")) {
            const uint8_t* m = sys.main_ram();
            unsigned e20 = m[0xE020] << 8 | m[0xE021], e22 = m[0xE022] << 8 | m[0xE023];
            if (e20 >= std::strtoul(s, nullptr, 16) && e22 >= 3 && e22 < 0x100) break;
        }
        if (ppm_every && f % ppm_every == 0) {
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%05d.ppm", f);
            write_ppm(out + name, sys.framebuffer(), sys.width(), sys.height());
        }
    }
    if (std::getenv("SCD_DEBUG")) sys.dump_state();
    if (profile_from >= 0) sys.dump_profile(12);
    if (std::getenv("SCD_DUMP_RAM")) {
        auto dump = [&](const char* n, const uint8_t* p, size_t len) { FILE* f = std::fopen((out + n).c_str(), "wb"); std::fwrite(p, 1, len, f); std::fclose(f); };
        dump("/main_ram.bin", sys.main_ram(), 0x10000);
        dump("/prg_ram.bin", sys.prg_ram(), 0x80000);
        dump("/word_ram.bin", sys.word_ram(), 0x40000);
    }
    if (std::getenv("SCD_VRAM")) {
        FILE* f = std::fopen((out + "/vram.bin").c_str(), "wb");
        std::fwrite(sys.vdp().vram(), 1, 0x10000, f);
        std::fwrite(sys.vdp().cram(), 2, 64, f);
        std::fwrite(sys.vdp().vsram(), 2, 64, f);
        for (int i = 0; i < 24; ++i) std::fputc(sys.vdp().reg(i), f);
        std::fclose(f);
    }
    write_ppm(out + "/final.ppm", sys.framebuffer(), sys.width(), sys.height());
    if (!wav.empty()) write_wav(wav, all_audio);
    std::printf("ran %llu frames%s\n", (unsigned long long)sys.frame_count(), sys.halted() ? " (halted)" : "");
    sys.shutdown();
    return 0;
}
