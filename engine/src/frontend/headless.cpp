// Headless runner: boots a disc for N frames, optionally dumping frames and audio.
//   scd_headless <disc.cue> [--frames N] [--ppm-every N --out DIR] [--wav out.wav] [--video out.mp4]
//                [--press FRAME:BUTTONS:HOLD ...] [--replay FILE [--bram FILE]] [--ckpt-out FILE [--ckpt-every N]] [--watch [--hang-frames N]]
// --video writes a 1440x1080 AAC demo video of the run, H.264 or with --video-codec h265 H.265 (needs ffmpeg, see recorder.h).
// BUTTONS is a string of U D L R B C A S. Output stays in the directory given by --out.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "recorder.h"
#include "replay.h"
#include "system.h"
#include "watchdog.h"

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

// Everything needed to look at a watchdog trip later: what tripped, both CPUs' PCs, the screen, RAM, and the input that got here.
void write_failure_bundle(System& sys, const std::string& out, const Replay& trace, const WatchVerdict& v) {
    uint32_t mpc, spc;
    sys.cpu_pcs(&mpc, &spc);
    const char* kind = v.kind == WatchVerdict::kHang ? "hang" : "halted";
    if (FILE* f = std::fopen((out + "/report.json").c_str(), "w")) {
        std::fprintf(f, "{\"kind\":\"%s\",\"frame\":%llu,\"since\":%llu,\"e020\":\"0x%04x\",\"e022\":\"0x%04x\",\"e06c\":\"0x%04x\",\"main_pc\":\"0x%06x\",\"sub_pc\":\"0x%06x\"}\n",
                     kind, (unsigned long long)sys.frame_count(), (unsigned long long)v.since, sys.e020(), sys.e022(), sys.e06c(), mpc, spc);
        std::fclose(f);
    }
    write_ppm(out + "/final.ppm", sys.framebuffer(), sys.width(), sys.height());
    if (FILE* f = std::fopen((out + "/mainram.bin").c_str(), "wb")) { std::fwrite(sys.main_ram(), 1, 0x10000, f); std::fclose(f); }
    if (FILE* f = std::fopen((out + "/subram.bin").c_str(), "wb")) { std::fwrite(sys.prg_ram() + 0x7000, 1, 0x6000, f); std::fclose(f); }
    if (FILE* f = std::fopen((out + "/repro.replay").c_str(), "w")) { std::fputs(trace.serialize().c_str(), f); std::fclose(f); }
    sys.dump_state();
    std::fprintf(stderr, "[watch] %s at frame %llu (e020=%04x main_pc=%06x sub_pc=%06x); bundle in %s\n", kind, (unsigned long long)sys.frame_count(), sys.e020(), mpc, spc, out.c_str());
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s disc.cue [--frames N] [--ppm-every N] [--out DIR] [--wav FILE] [--video FILE.mp4 [--video-codec h264|h265]] [--press F:BTNS:HOLD]\n", argv[0]);
        return 2;
    }
    std::string cue = argv[1], out = ".", wav, video, codec = "h264";
    int frames = 600, ppm_every = 0, ckpt_every = 300, hang_frames = 300;
    bool watch = false;
    std::string replay_path, ckpt_path, bram;
    std::vector<Press> presses;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--frames" && i + 1 < argc) frames = std::atoi(argv[++i]);
        else if (a == "--ppm-every" && i + 1 < argc) ppm_every = std::atoi(argv[++i]);
        else if (a == "--out" && i + 1 < argc) out = argv[++i];
        else if (a == "--wav" && i + 1 < argc) wav = argv[++i];
        else if (a == "--video" && i + 1 < argc) video = argv[++i];
        else if (a == "--video-codec" && i + 1 < argc) codec = argv[++i];
        else if (a == "--replay" && i + 1 < argc) replay_path = argv[++i];
        else if (a == "--ckpt-out" && i + 1 < argc) ckpt_path = argv[++i];
        else if (a == "--ckpt-every" && i + 1 < argc) ckpt_every = std::atoi(argv[++i]);
        else if (a == "--bram" && i + 1 < argc) bram = argv[++i];
        else if (a == "--watch") watch = true;
        else if (a == "--hang-frames" && i + 1 < argc) hang_frames = std::atoi(argv[++i]);
        else if (a == "--press" && i + 1 < argc) {
            int f, h;
            char b[16];
            if (std::sscanf(argv[++i], "%d:%15[^:]:%d", &f, b, &h) == 3) presses.push_back({f, parse_buttons(b), h});
        }
    }
    if (const char* lf = std::getenv("SCD_LOG")) {   // debug log file, same as the app's --log
        if (!System::instance().open_log(lf)) { std::fprintf(stderr, "error: cannot open %s\n", lf); return 1; }
    }
    if (const char* cs = std::getenv("SCD_CD_SPEED")) System::instance().set_cd_speed(std::atof(cs));
    std::string err;
    auto disc = Disc::open(cue, &err);
    if (!disc) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    System& sys = System::instance();
    if (bram.empty() && std::getenv("SCD_SAVE")) bram = std::getenv("SCD_SAVE");
    const std::string bram_sha_at_start = bram.empty() ? "none" : sha1_file(bram);   // the core rewrites the file at shutdown
    if (!sys.init(std::move(disc), bram, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }

    Replay replay;
    if (!replay_path.empty()) {
        FILE* rf = std::fopen(replay_path.c_str(), "rb");
        if (!rf) { std::fprintf(stderr, "error: cannot open %s\n", replay_path.c_str()); return 1; }
        std::string text;
        char rbuf[4096];
        size_t rn;
        while ((rn = std::fread(rbuf, 1, sizeof rbuf, rf)) > 0) text.append(rbuf, rn);
        std::fclose(rf);
        if (!Replay::parse(text, &replay, &err)) { std::fprintf(stderr, "error: %s: %s\n", replay_path.c_str(), err.c_str()); return 1; }
        std::string dsha = disc_sha1_from_cue(cue);
        if (!replay.disc_sha1.empty() && replay.disc_sha1 != dsha) { std::fprintf(stderr, "error: replay was recorded for disc %s, this disc is %s\n", replay.disc_sha1.c_str(), dsha.c_str()); return 4; }
        if (replay.bram_sha1 != bram_sha_at_start) { std::fprintf(stderr, "error: replay needs backup RAM %s, got %s (pass --bram)\n", replay.bram_sha1.c_str(), bram_sha_at_start.c_str()); return 4; }
        sys.set_replay_play(&replay);
    }
    Watchdog dog({hang_frames, 1800});
    const uint64_t stick_from = std::getenv("SCD_TEST_STICK_E020") ? std::strtoull(std::getenv("SCD_TEST_STICK_E020"), nullptr, 10) : 0;
    uint16_t stuck_val = 0;
    bool stuck_set = false;
    Replay trace;   // the pad stream actually applied, for the failure bundle's repro.replay
    if (watch) {
        trace.disc_sha1 = disc_sha1_from_cue(cue);
        trace.bram_sha1 = bram_sha_at_start;
        trace.engine = "scd_headless --watch";
        sys.set_replay_record(&trace);
    }
    FILE* ckpt = nullptr;
    uint16_t last22 = 0xFFFF, last6c = 0xFFFF;
    if (!ckpt_path.empty() && !(ckpt = std::fopen(ckpt_path.c_str(), "w"))) { std::fprintf(stderr, "error: cannot open %s\n", ckpt_path.c_str()); return 1; }
    auto emit_ckpt = [&]() {
        StateHash h = sys.state_hash();
        std::fprintf(ckpt, "@0x%llx scene=E022:%04x/E06C:%04x vram=%016llx ram=%016llx fb=%016llx\n", (unsigned long long)sys.frame_count(),
                     sys.e022(), sys.e06c(), (unsigned long long)h.vram, (unsigned long long)h.ram, (unsigned long long)h.fb);
    };

    const int profile_from = std::getenv("SCD_PROFILE") ? std::atoi(std::getenv("SCD_PROFILE")) : -1;
    if (const char* t = std::getenv("SCD_TRACE")) {  // cpu:frame:count
        int cpu, n; unsigned long long fr;
        if (std::sscanf(t, "%d:%llu:%d", &cpu, &fr, &n) == 3) sys.set_trace(cpu, fr, n);
    }
    if (std::getenv("SCD_STACKAT") && std::getenv("SCD_FROM")) sys.set_trace_from(std::strtoull(std::getenv("SCD_FROM"), nullptr, 10));
    if (std::getenv("SCD_BRAM_SELFTEST")) { bool ok = sys.bram_selftest(); sys.shutdown(); std::printf("backup RAM selftest: %s\n", ok ? "PASS" : "FAIL"); return ok ? 0 : 1; }
    if (const char* g = std::getenv("SCD_GUN")) { int gx, gy; if (std::sscanf(g, "%d,%d", &gx, &gy) == 2) { sys.set_gun_connected(true); sys.set_gun(gx, gy, true, 0); } }
    struct PrgPoll { uint32_t addr; uint8_t last; };
    std::vector<PrgPoll> prg_poll;
    if (const char* pl = std::getenv("SCD_PRGPOLL"))
        for (const char* q = pl; *q;) {
            char* e;
            unsigned long a = std::strtoul(q, &e, 16);
            if (e == q) break;
            prg_poll.push_back({uint32_t(a) & 0x7FFFF, 0});
            q = *e ? e + 1 : e;
        }
    std::vector<int16_t> all_audio;
    Recorder rec;
    if (!video.empty() && !rec.start(video, sys.height(), codec, &err)) { std::fprintf(stderr, "error: %s\n", err.c_str()); return 1; }
    const bool frametime = std::getenv("SCD_FRAMETIME") != nullptr;   // per-frame host cost report (stutter hunting)
    std::vector<std::pair<double, int>> ft;
    for (int f = 0; f < frames && !sys.halted(); ++f) {
        uint16_t pad = 0;
        for (auto& p : presses) if (f >= p.frame && f < p.frame + p.hold) pad |= p.buttons;
        if (profile_from >= 0 && f == frames - profile_from) sys.enable_profile(true);
        sys.set_pad(0, pad);
        auto t0 = std::chrono::steady_clock::now();
        sys.run_frame();
        if (frametime) ft.push_back({std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(), f + 1});
        rec.frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
        if (watch) {
            uint16_t e20 = sys.e020();
            if (stick_from && sys.frame_count() >= stick_from) { if (!stuck_set) { stuck_val = e20; stuck_set = true; } e20 = stuck_val; }   // SCD_TEST_STICK_E020: test hook, samples only
            WatchVerdict v = dog.feed({sys.frame_count(), e20, sys.state_hash().fb, sys.halted()});
            if (v.kind == WatchVerdict::kStillScreen) std::fprintf(stderr, "[watch] warning: screen unchanged since frame %llu\n", (unsigned long long)v.since);
            else if (v.kind != WatchVerdict::kOk) {
                if (ckpt) { emit_ckpt(); std::fclose(ckpt); }
                write_failure_bundle(sys, out, trace, v);
                sys.shutdown();
                return 3;
            }
        }
        if (ckpt) {
            bool changed = sys.e022() != last22 || sys.e06c() != last6c;
            if (changed || (ckpt_every > 0 && sys.frame_count() % ckpt_every == 0)) emit_ckpt();
            last22 = sys.e022(); last6c = sys.e06c();
        }
        if (!wav.empty()) {
            all_audio.insert(all_audio.end(), sys.audio().begin(), sys.audio().end());
        }
        sys.audio().clear();
        if (!prg_poll.empty()) {  // SCD_PRGPOLL=hexaddr,hexaddr: report changes of Sub PRG RAM bytes
            for (auto& pp : prg_poll) {
                uint8_t v = sys.prg_ram()[pp.addr];
                if (v != pp.last) { std::printf("[prg] f=%d %06x %02x -> %02x\n", f + 1, pp.addr, pp.last, v); pp.last = v; }
            }
        }
        if (std::getenv("SCD_STATE")) {
            const uint8_t* m = sys.main_ram();
            const uint8_t* p = sys.prg_ram();
            std::printf("%d e022=%04x e06c=%04x e020=%04x f=%02x%02x comm=%s\n", f + 1, m[0xE022] << 8 | m[0xE023], m[0xE06C] << 8 | m[0xE06D], m[0xE020] << 8 | m[0xE021], sys.main_flag(), sys.sub_flag(), sys.comm_string().c_str()); (void)p;
        }
        if (const char* s = std::getenv("SCD_STOP_E020")) {
            const uint8_t* m = sys.main_ram();
            unsigned e20 = m[0xE020] << 8 | m[0xE021], e22 = m[0xE022] << 8 | m[0xE023];
            if (e20 >= std::strtoul(s, nullptr, 16) && e22 >= 3 && e22 < 0x100) break;
        }
        if (const char* ds = std::getenv("SCD_DUMP_AT")) {
            for (const char* q = ds; *q;) {
                char* e;
                unsigned long v = std::strtoul(q, &e, 10);
                if (e == q) break;
                if (v == unsigned(f + 1)) {
                    FILE* o = std::fopen((out + "/mainram_" + std::to_string(v) + ".bin").c_str(), "wb");
                    std::fwrite(sys.main_ram(), 1, 0x10000, o);
                    std::fclose(o);
                    o = std::fopen((out + "/subram_" + std::to_string(v) + ".bin").c_str(), "wb");
                    std::fwrite(sys.prg_ram() + 0x7000, 1, 0x6000, o);
                    std::fclose(o);
                }
                q = *e ? e + 1 : e;
            }
        }
        if (const char* dp = std::getenv("SCD_DUMP_PRG")) {  // frame:hexaddr:hexlen:path
            unsigned long fr, ad, ln; char path[512];
            if (std::sscanf(dp, "%lu:%lx:%lx:%511s", &fr, &ad, &ln, path) == 4 && unsigned(f + 1) == fr) {
                FILE* o = std::fopen(path, "wb");
                std::fwrite(sys.prg_ram() + ad, 1, ln, o);
                std::fclose(o);
            }
        }
        if (ppm_every && f % ppm_every == 0) {
            char name[64];
            std::snprintf(name, sizeof name, "/frame_%05d.ppm", f);
            write_ppm(out + name, sys.framebuffer(), sys.width(), sys.height());
        }
    }
    if (ckpt) { emit_ckpt(); std::fclose(ckpt); }
    if (frametime && !ft.empty()) {
        double sum = 0; int over = 0;
        for (auto& [ms, fr] : ft) { sum += ms; over += ms > 1000.0 / 59.9227; }
        std::vector<std::pair<double, int>> s = ft;
        std::sort(s.begin(), s.end());
        auto pct = [&](double p) { return s[std::min(s.size() - 1, size_t(p * s.size()))].first; };
        std::fprintf(stderr, "frame ms: mean %.3f p50 %.3f p99 %.3f p99.9 %.3f max %.3f, %d/%zu over 16.69ms\nworst:", sum / ft.size(), pct(0.5), pct(0.99), pct(0.999), s.back().first, over, ft.size());
        for (size_t i = 0; i < std::min<size_t>(10, s.size()); ++i) std::fprintf(stderr, " f%d=%.2f", s[s.size() - 1 - i].second, s[s.size() - 1 - i].first);
        std::fprintf(stderr, "\n");
    }
    if (std::getenv("SCD_DEBUG")) { sys.dump_state(); std::fprintf(stderr, "YM writes: %llu translated=%d fallback_steps=%llu verified=%llu\n", (unsigned long long)sys.ym_write_count(), int(sys.translated()), (unsigned long long)sys.fallback_steps(), (unsigned long long)sys.verified_count()); }
    if (profile_from >= 0) sys.dump_profile(12);
    if (std::getenv("SCD_OVERLAYS")) sys.dump_overlay_hist();
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
    if (rec.active()) {
        std::fprintf(stderr, "encoding %s (%llu frames)...\n", video.c_str(), (unsigned long long)rec.frames());
        if (!rec.finish(&err)) std::fprintf(stderr, "error: %s\n", err.c_str());
    }
    std::printf("ran %llu frames%s\n", (unsigned long long)sys.frame_count(), sys.halted() ? " (halted)" : "");
    sys.shutdown();
    return 0;
}
