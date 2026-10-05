// SDL3 front-end: window, audio, keyboard and gamepad input for the Sega CD core.
//   snatcher <disc.cue> [--justifier] [--record FILE.mp4] [--video-codec h264|h265]   (or drop a .cue file onto the window)
//   --record-input FILE: save pad input per emulated frame (a .replay for scd_headless --replay) and the starting backup RAM (FILE.bram)
//   --justifier: mouse is the Konami Justifier on port 2 (left = trigger, right = start)
//   --record: record a demo video from boot; F9 starts/stops a recording at any time (default ~/Movies/snatcher-<time>.mp4).
//   Videos are 1440x1080 with AAC audio, H.264 unless --video-codec h265; the final encode runs in the background.
#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "recorder.h"
#include "replay.h"
#include "system.h"

using namespace scd;

namespace {

uint16_t keyboard_buttons() {
    const bool* k = SDL_GetKeyboardState(nullptr);
    uint16_t b = 0;
    if (k[SDL_SCANCODE_UP]) b |= kUp;
    if (k[SDL_SCANCODE_DOWN]) b |= kDown;
    if (k[SDL_SCANCODE_LEFT]) b |= kLeft;
    if (k[SDL_SCANCODE_RIGHT]) b |= kRight;
    if (k[SDL_SCANCODE_Z]) b |= kA;
    if (k[SDL_SCANCODE_X]) b |= kB;
    if (k[SDL_SCANCODE_C]) b |= kC;
    if (k[SDL_SCANCODE_RETURN]) b |= kStart;
    return b;
}

uint16_t gamepad_buttons(SDL_Gamepad* g) {
    if (!g) return 0;
    uint16_t b = 0;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= kUp;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= kDown;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= kLeft;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= kRight;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_SOUTH)) b |= kB;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_EAST)) b |= kC;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_WEST)) b |= kA;
    if (SDL_GetGamepadButton(g, SDL_GAMEPAD_BUTTON_START)) b |= kStart;
    Sint16 x = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTX), y = SDL_GetGamepadAxis(g, SDL_GAMEPAD_AXIS_LEFTY);
    if (x < -16000) b |= kLeft;
    if (x > 16000) b |= kRight;
    if (y < -16000) b |= kUp;
    if (y > 16000) b |= kDown;
    return b;
}

bool boot(const std::string& cue, bool* running_game) {
    std::string err;
    auto disc = Disc::open(cue, &err);
    if (!disc) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    char* pref = SDL_GetPrefPath("snatcher-port", "snatcher");
    std::string save = pref ? std::string(pref) + "bram.bin" : std::string();
    SDL_free(pref);
    if (!System::instance().init(std::move(disc), save, &err)) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    *running_game = true;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
#ifdef __APPLE__
    // SDL's Metal-backed renderers (metal, gpu) intermittently stall presents for up to the 1 s drawable timeout on
    // macOS (seen with SDL 3.4.10 on a 120 Hz panel); the OpenGL renderer presents every vsync. SDL_RENDER_DRIVER overrides.
    SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl,metal");
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_Window* window = SDL_CreateWindow("Snatcher", 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* renderer = window ? SDL_CreateRenderer(window, nullptr) : nullptr;
    if (!renderer) {
        std::fprintf(stderr, "window/renderer failed: %s\n", SDL_GetError());
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);
    if (std::getenv("SNATCHER_FPS")) std::fprintf(stderr, "renderer %s\n", SDL_GetRendererName(renderer));
    SDL_SetRenderLogicalPresentation(renderer, 320, 240, SDL_LOGICAL_PRESENTATION_LETTERBOX);
    SDL_Texture* tex = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    SDL_SetTextureScaleMode(tex, SDL_SCALEMODE_NEAREST);

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, kSampleRate};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio);

    SDL_Gamepad* pad = nullptr;
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        if (count > 0) pad = SDL_OpenGamepad(ids[0]);
        SDL_free(ids);
    }

    bool running_game = false;
    bool gun = false;
    bool want_log = std::getenv("SNATCHER_LOG") != nullptr;
    double cd_speed = std::getenv("SCD_CD_SPEED") ? std::atof(std::getenv("SCD_CD_SPEED")) : 1.0;
    std::string disc, record_path, record_input, codec = "h264";
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--justifier") gun = true;
        else if (a == "--log") want_log = true;
        else if (a == "--cd-speed" && i + 1 < argc) cd_speed = std::atof(argv[++i]);
        else if (a == "--record" && i + 1 < argc) record_path = argv[++i];
        else if (a == "--video-codec" && i + 1 < argc) codec = argv[++i];
        else if (a == "--record-input" && i + 1 < argc) record_input = argv[++i];
        else disc = a;
    }
    System::instance().set_cd_speed(cd_speed);
    if (want_log) {
        char* pref = SDL_GetPrefPath("snatcher-port", "snatcher");
        std::string path = std::string(pref ? pref : "") + "debug.log";
        SDL_free(pref);
        if (System::instance().open_log(path)) {
            const int v = SDL_GetVersion();
            scd::debug_log("platform %s, SDL %d.%d.%d, renderer %s\n", SDL_GetPlatform(), SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v),
                           SDL_VERSIONNUM_MICRO(v), SDL_GetRendererName(renderer));
            const char* adev = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
            scd::debug_log("audio: %s (%s); gamepad: %s; CD speed %.1fx; justifier %s\n", adev ? adev : "?", audio ? "stream ok" : "STREAM FAILED, no sound",
                           pad ? SDL_GetGamepadName(pad) : "none", cd_speed, gun ? "on" : "off");
            scd::debug_log("log file: %s\ndisc: %s\n", path.c_str(), disc.empty() ? "(none yet, drop a .cue on the window)" : disc.c_str());
        } else {
            std::fprintf(stderr, "could not open debug log %s\n", path.c_str());
        }
    }
    // --record-input: keep the backup RAM the session starts with (the replay is only valid against it) and log pad input per frame.
    Replay input_replay;
    if (!record_input.empty()) {   // an hour-long freeze hunt must not be lost to a missing directory: fail now, not at exit
        std::ofstream probe(record_input, std::ios::binary);
        if (!probe) { std::fprintf(stderr, "error: cannot write %s (does the directory exist?)\n", record_input.c_str()); return 1; }
    }
    bool recording_booted = false;   // the replay stays savable after a CPU halt, which clears running_game
    if (!record_input.empty() && !disc.empty()) {
        char* pref = SDL_GetPrefPath("snatcher-port", "snatcher");
        std::string bram = std::string(pref ? pref : "") + "bram.bin";
        SDL_free(pref);
        input_replay.bram_sha1 = sha1_file(bram);
        if (input_replay.bram_sha1 != "none") {
            std::ifstream src(bram, std::ios::binary);
            std::ofstream dst(record_input + ".bram", std::ios::binary);
            dst << src.rdbuf();
            if (!dst) { std::fprintf(stderr, "error: cannot write %s.bram\n", record_input.c_str()); return 1; }
        }
        input_replay.disc_sha1 = disc_sha1_from_cue(disc);
        input_replay.engine = "snatcher app";
        input_replay.cd_speed = cd_speed;
        input_replay.justifier = gun;
    } else if (!record_input.empty()) {
        std::fprintf(stderr, "--record-input needs the disc on the command line\n");
        record_input.clear();
    }
    if (!disc.empty()) boot(disc, &running_game);
    if (!record_input.empty() && running_game) { System::instance().set_replay_record(&input_replay); recording_booted = true; }
    System::instance().set_gun_connected(gun);   // the controller ID is read at boot: start with --justifier to get the Gun Adjust option
    SDL_SetWindowTitle(window, running_game ? "Snatcher" : "Snatcher - drop a .cue file on this window");

    // Demo recording (F9). Finishing a recording encodes the final video on a worker thread so play continues.
    std::unique_ptr<Recorder> rec;
    std::vector<std::thread> encoders;
    auto start_recording = [&](std::string path) {
        if (path.empty()) {
            const char* dir = SDL_GetUserFolder(SDL_FOLDER_VIDEOS);
            char stamp[32];
            std::time_t t = std::time(nullptr);
            std::strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", std::localtime(&t));
            path = std::string(dir ? dir : "") + "snatcher-" + stamp + ".mp4";
        }
        auto r = std::make_unique<Recorder>();
        std::string err;
        if (!r->start(path, System::instance().height(), codec, &err)) { std::fprintf(stderr, "recording: %s\n", err.c_str()); return; }
        std::fprintf(stderr, "recording to %s (F9 to stop)\n", path.c_str());
        rec = std::move(r);
        SDL_SetWindowTitle(window, "Snatcher - REC (F9 to stop)");
    };
    auto stop_recording = [&] {
        if (!rec) return;
        std::fprintf(stderr, "encoding %s (%llu frames)...\n", rec->path().c_str(), (unsigned long long)rec->frames());
        encoders.emplace_back([r = std::move(rec)] {
            std::string err;
            if (r->finish(&err)) std::fprintf(stderr, "saved %s\n", r->path().c_str());
            else std::fprintf(stderr, "recording: %s\n", err.c_str());
        });
        SDL_SetWindowTitle(window, "Snatcher");
    };
    if (running_game && !record_path.empty()) start_recording(record_path);

    // Frame pacing. When the display refresh is within 1% of n x 59.9227 Hz (60 Hz, 120 Hz ProMotion) the emulator is
    // locked to vsync and runs exactly one frame every n vsyncs: a wall-clock deadline at 59.92 Hz would drift across
    // vsync edges and show frames for 1 or 3 refreshes instead of 2. Elapsed vsyncs are counted on a grid that slowly
    // phase-locks to the present times, so present jitter under half a vsync (it is several ms, alternating) never moves
    // a frame boundary. Other refresh rates (75, 144 Hz, unknown) and a hidden window pace on the monotonic clock.
    // Audio drift (the emulator runs 0.13% fast on a 60 Hz lock, and the audio device has its own clock) is absorbed by
    // resampling the stream from the queue fill rather than by retiming video.
    constexpr double kEmuHz = 59.9227;
    const int audio_target = kSampleRate / 20 * 4;   // bytes: 50 ms (3 frames) of 16-bit stereo
    const bool show_fps = std::getenv("SNATCHER_FPS") != nullptr;
    double vsync_ns = 0, period_ns = 1e9 / kEmuHz, acc_ns = 0, audio_fill = 1.0, grid_ns = 0;
    bool locked = false;
    int64_t grid_k = 0;
    auto retime = [&] {
        const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
        double hz = m ? m->refresh_rate : 0.0;
        int n = hz > 0 ? int(std::lround(hz / kEmuHz)) : 0;
        locked = n >= 1 && std::fabs(hz / n / kEmuHz - 1.0) < 0.01;
        vsync_ns = hz > 0 ? 1e9 / hz : 0;
        period_ns = locked ? vsync_ns * n : 1e9 / kEmuHz;
        grid_ns = double(SDL_GetTicksNS());
        grid_k = 0;
        acc_ns = locked ? vsync_ns / 2 : 0;   // locked: acc moves in whole vsyncs, so half a vsync keeps it off the boundary
        if (show_fps) std::fprintf(stderr, "display %.2f Hz: %s, frame period %.3f ms\n", hz, locked ? "vsync-locked" : "clock-paced", period_ns / 1e6);
    };
    retime();
    uint64_t prev_ns = SDL_GetTicksNS();
    bool visible = true;

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                stop_recording();
                if (recording_booted) {   // frames restart at 0 and the disc/backup RAM change: keep what was recorded so far, stop recording
                    System::instance().set_replay_record(nullptr);
                    std::fprintf(stderr, "--record-input: a new disc was dropped, input recording stopped (saved at exit)\n");
                }
                if (running_game) System::instance().shutdown();
                running_game = false;
                if (boot(e.drop.data, &running_game)) SDL_SetWindowTitle(window, "Snatcher");
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(window, !fs);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F10 && !e.key.repeat && running_game) {
                scd::debug_log("[user] F10 state snapshot at frame %llu\n", (unsigned long long)System::instance().frame_count());
                System::instance().dump_state();
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F9 && !e.key.repeat && running_game) {
                if (rec) stop_recording(); else start_recording("");
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) quit = true;
            else if (e.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || e.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) retime();
            // A hidden window's presents can block for 100+ ms (macOS throttles occluded Metal layers): stop presenting
            // and pace on the clock so audio keeps playing.
            else if (e.type == SDL_EVENT_WINDOW_OCCLUDED || e.type == SDL_EVENT_WINDOW_HIDDEN || e.type == SDL_EVENT_WINDOW_MINIMIZED) visible = false;
            else if (e.type == SDL_EVENT_WINDOW_EXPOSED || e.type == SDL_EVENT_WINDOW_SHOWN || e.type == SDL_EVENT_WINDOW_RESTORED) visible = true;
        }
        uint64_t now = SDL_GetTicksNS();
        int64_t vsyncs = 1;   // elapsed since the last iteration (for SNATCHER_FPS)
        if (locked && visible) {
            double t = double(now) - grid_ns;
            int64_t k = std::llround(t / vsync_ns);
            grid_ns += 0.02 * (t - double(k) * vsync_ns);   // follow the real vsync phase and rate
            acc_ns += double(k - grid_k) * vsync_ns;
            vsyncs = k - grid_k;
            grid_k = k;
        } else {
            acc_ns += double(now - prev_ns);
            grid_ns = double(now);
            grid_k = 0;
        }
        prev_ns = now;
        if (acc_ns > 8 * period_ns) acc_ns = period_ns + (locked ? vsync_ns / 2 : 0);   // fell far behind (window drag, breakpoint): resync, don't fast-forward
        int ran = 0;
        if (running_game) {
            System& sys = System::instance();
            sys.set_pad(0, keyboard_buttons() | gamepad_buttons(pad));
            if (gun) {
                float mx, my;
                uint32_t mb = SDL_GetMouseState(&mx, &my);
                SDL_RenderCoordinatesFromWindow(renderer, mx, my, &mx, &my);   // letterboxed 320x240 logical space
                int gx = int(mx * sys.width() / 320.0f), gy = int(my * sys.height() / 240.0f);
                bool inside = mx >= 0 && my >= 0 && gx < sys.width() && gy < sys.height();
                sys.set_gun(gx, gy, inside, uint8_t(((mb & SDL_BUTTON_LMASK) ? 1 : 0) | ((mb & SDL_BUTTON_RMASK) ? 2 : 0)));
            }
            while (acc_ns >= period_ns && running_game) {
                sys.run_frame();
                if (rec) rec->frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
                if (audio && !sys.audio().empty()) {
                    int queued = SDL_GetAudioStreamQueued(audio);
                    audio_fill += 0.05 * (double(queued) / audio_target - audio_fill);
                    SDL_SetAudioStreamFrequencyRatio(audio, float(1.0 + std::clamp((audio_fill - 1.0) * 0.01, -0.01, 0.01)));
                    if (queued < audio_target * 4)   // after a long stall, drop rather than add latency
                        SDL_PutAudioStreamData(audio, sys.audio().data(), int(sys.audio().size() * sizeof(int16_t)));
                }
                sys.audio().clear();
                if (sys.halted()) running_game = false;
                acc_ns -= period_ns;
                ++ran;
            }
            if (ran) SDL_UpdateTexture(tex, nullptr, sys.framebuffer(), 320 * 4);
        }
        if (show_fps && running_game) {
            // hold[k]: emulated frames that stayed on screen for k vsyncs (presents when not vsync-locked); steady = one bucket
            static uint64_t t0 = now; static int frames = 0, held = 0, hold[5] = {};
            frames += ran;
            held += int(vsyncs);
            if (ran) { hold[std::min(held, 4)]++; held = 0; }
            if (now - t0 >= 1000000000ull) {
                std::fprintf(stderr, "emu fps %.2f, audio queued %d bytes (ratio %.4f), vsyncs per frame 1:%d 2:%d 3:%d 4+:%d%s\n", frames * 1e9 / double(now - t0),
                             audio ? SDL_GetAudioStreamQueued(audio) : 0, audio ? SDL_GetAudioStreamFrequencyRatio(audio) : 1.0f, hold[1], hold[2], hold[3], hold[4], visible ? "" : " (hidden)");
                t0 = now; frames = 0; std::fill(hold, hold + 5, 0);
            }
        }
        if (!visible) {
            if (acc_ns < period_ns) SDL_DelayPrecise(uint64_t(period_ns - acc_ns));
            continue;
        }
        SDL_SetRenderDrawColor(renderer, 0, 0, 0, 255);
        SDL_RenderClear(renderer);
        if (running_game) {
            SDL_FRect src = {0, 0, float(System::instance().width()), float(System::instance().height())};
            SDL_FRect dst = {0, 0, 320, 240};
            SDL_RenderTexture(renderer, tex, &src, &dst);
        }
        SDL_RenderPresent(renderer);
    }
    stop_recording();
    if (!encoders.empty()) std::fprintf(stderr, "finishing video encode...\n");
    for (auto& t : encoders) t.join();
    if (!record_input.empty() && recording_booted) {   // also after a CPU halt: a crash session is the one worth replaying
        System::instance().set_replay_record(nullptr);
        std::ofstream out(record_input, std::ios::binary);
        out << input_replay.serialize();
        out.close();
        if (out) std::fprintf(stderr, "saved %s\n", record_input.c_str());
        else std::fprintf(stderr, "error: could not write %s\n", record_input.c_str());
    }
    if (running_game) System::instance().shutdown();
    System::instance().close_log();
    SDL_Quit();
    return 0;
}
