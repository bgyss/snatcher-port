// SDL3 front-end: window, audio, keyboard and gamepad input for the Sega CD core.
//   snatcher <disc.cue> [--justifier] [--record FILE.mp4] [--video-codec h264|h265]   (or drop a .cue file onto the window)
//   Every boot records a session folder (docs/PLAYTEST.md "Sessions"): work/sessions/ when run from the repository, else
//   <per-user data dir>/sessions/. --no-session: do not record; --session-dir DIR: parent directory for session folders.
//   --record-input FILE: also copy the first session's replay to FILE and its starting backup RAM to FILE.bram at exit
//   F8: mark "this looks wrong" (markers/ in the session folder); F10: the same plus a state snapshot in the log
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
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "manifest.h"
#include "recorder.h"
#include "replay.h"
#include "session.h"
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

std::string pref_dir() {   // per-user data directory (ends in a separator), "" when SDL cannot provide one
    char* pref = SDL_GetPrefPath("snatcher-port", "snatcher");
    std::string s = pref ? pref : "";
    SDL_free(pref);
    return s;
}

std::string bram_path() {
    const std::string p = pref_dir();
    return p.empty() ? p : p + "bram.bin";
}

bool boot(const std::string& cue, bool* running_game) {
    std::string err;
    auto disc = Disc::open(cue, &err);
    if (!disc) {
        std::fprintf(stderr, "error: %s\n", err.c_str());
        return false;
    }
    if (!System::instance().init(std::move(disc), bram_path(), &err)) {
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
    bool want_session = true;
    std::string session_parent;
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
        else if (a == "--no-session") want_session = false;
        else if (a == "--session-dir" && i + 1 < argc) session_parent = argv[++i];
        else disc = a;
    }
    System::instance().set_cd_speed(cd_speed);
    if (!record_input.empty() && !want_session) {
        std::fprintf(stderr, "--record-input saves the session's replay: ignoring --no-session\n");
        want_session = true;
    }
    if (!record_input.empty()) {   // an hour-long freeze hunt must not be lost to a missing directory: fail now, not at exit
        std::ofstream probe(record_input, std::ios::binary);
        if (!probe) { std::fprintf(stderr, "error: cannot write %s (does the directory exist?)\n", record_input.c_str()); return 1; }
    }
    std::vector<std::string> session_args;   // manifest "args": the command line without the disc path
    for (int i = 1; i < argc; ++i)
        if (disc.empty() || disc != argv[i]) session_args.push_back(argv[i]);
    const int sdl_v = SDL_GetVersion();
    char sdl_version[32];
    std::snprintf(sdl_version, sizeof sdl_version, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(sdl_v), SDL_VERSIONNUM_MINOR(sdl_v), SDL_VERSIONNUM_MICRO(sdl_v));
    const char* renderer_name = SDL_GetRendererName(renderer);
    auto log_header = [&](const std::string& log_path, const std::string& cue) {
        scd::debug_log("platform %s, SDL %s, renderer %s\n", SDL_GetPlatform(), sdl_version, renderer_name ? renderer_name : "?");
        const char* adev = SDL_GetAudioDeviceName(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK);
        scd::debug_log("audio: %s (%s); gamepad: %s; CD speed %.1fx; justifier %s\n", adev ? adev : "?", audio ? "stream ok" : "STREAM FAILED, no sound",
                       pad ? SDL_GetGamepadName(pad) : "none", cd_speed, gun ? "on" : "off");
        scd::debug_log("log file: %s\ndisc: %s\n", log_path.c_str(), cue.empty() ? "(none yet, drop a .cue on the window)" : cue.c_str());
    };
    auto open_debug_log = [&] {   // --log without a session: the per-user debug.log, as before sessions existed
        const std::string path = pref_dir() + "debug.log";
        if (System::instance().open_log(path)) log_header(path, disc);
        else std::fprintf(stderr, "could not open debug log %s\n", path.c_str());
    };
    if (want_log && !want_session) open_debug_log();

    // Session recording: one folder per booted disc. It never blocks play: on any failure the game runs without one.
    std::unique_ptr<Session> session;
    bool record_input_written = false;
    auto open_session = [&](const std::string& cue) {
        if (!want_session) return;
        std::error_code ec;
        const std::string parent = !session_parent.empty() ? session_parent
                                                           : default_session_parent(std::filesystem::current_path(ec).string(), pref_dir());
        std::string err;
        const std::string dir = create_session_dir(parent, session_stamp(std::time(nullptr)), &err);
        auto s = std::make_unique<Session>();
        SessionConfig c;
        c.cue = cue;
        c.bram_path = bram_path();
        c.engine = "snatcher app";
        c.cd_speed = cd_speed;
        c.justifier = gun;
        c.manifest.app = "snatcher";
        c.manifest.os = host_os();
        c.manifest.sdl = sdl_version;
        c.manifest.renderer = renderer_name ? renderer_name : "";
        c.manifest.args = session_args;
        if (dir.empty() || !s->begin(System::instance(), c, dir, &err)) {
            std::fprintf(stderr, "session: %s; playing without a session\n", err.c_str());
            if (want_log) open_debug_log();
            if (!record_input.empty() && !record_input_written) {
                std::fprintf(stderr, "warning: --record-input %s will not be written (no session)\n", record_input.c_str());
                scd::debug_log("--record-input %s will not be written (no session)\n", record_input.c_str());
            }
            return;
        }
        log_header(dir + "/session.log", cue);   // begin() already printed and logged the folder
        session = std::move(s);
    };
    auto close_session = [&](const char* why) {
        if (!session) return;
        if (!record_input.empty() && !record_input_written) {   // --record-input: the first session's replay
            if (session->export_replay(record_input)) std::fprintf(stderr, "saved %s\n", record_input.c_str());
            else std::fprintf(stderr, "error: could not write %s\n", record_input.c_str());
            record_input_written = true;
        }
        session->end(System::instance(), why);
        session.reset();
    };

    if (!disc.empty() && boot(disc, &running_game)) open_session(disc);
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

    uint64_t flash_until_ns = 0;   // F8/F10: a marker was written, frame the picture in white briefly
    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                stop_recording();
                close_session("disc_changed");   // frames restart at 0 and the disc/backup RAM change: one session per disc
                if (running_game) System::instance().shutdown();
                running_game = false;
                if (boot(e.drop.data, &running_game)) {
                    SDL_SetWindowTitle(window, "Snatcher");
                    open_session(e.drop.data);
                }
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(window, !fs);
            } else if (e.type == SDL_EVENT_KEY_DOWN && (e.key.key == SDLK_F8 || e.key.key == SDLK_F10) && !e.key.repeat && running_game) {
                if (e.key.key == SDLK_F10) {
                    scd::debug_log("[user] F10 state snapshot at frame %llu\n", (unsigned long long)System::instance().frame_count());
                    System::instance().dump_state();
                }
                if (session && session->marker(System::instance())) flash_until_ns = SDL_GetTicksNS() + 300000000ull;   // 0.3 s
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
                if (session) session->after_frame(sys, sys.e020(), SDL_GetTicks());
                if (rec) rec->frame(sys.framebuffer(), sys.width(), sys.height(), sys.audio());
                if (audio && !sys.audio().empty()) {
                    int queued = SDL_GetAudioStreamQueued(audio);
                    audio_fill += 0.05 * (double(queued) / audio_target - audio_fill);
                    SDL_SetAudioStreamFrequencyRatio(audio, float(1.0 + std::clamp((audio_fill - 1.0) * 0.01, -0.01, 0.01)));
                    if (queued < audio_target * 4)   // after a long stall, drop rather than add latency
                        SDL_PutAudioStreamData(audio, sys.audio().data(), int(sys.audio().size() * sizeof(int16_t)));
                }
                sys.audio().clear();
                if (sys.halted()) {   // the session's watchdog wrote the failure bundle in after_frame
                    running_game = false;
                    close_session("halted");
                }
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
            if (SDL_GetTicksNS() < flash_until_ns) {
                SDL_SetRenderDrawColor(renderer, 255, 255, 255, 255);
                for (int i = 0; i < 3; ++i) {
                    SDL_FRect r = {float(i), float(i), 320.0f - 2 * i, 240.0f - 2 * i};
                    SDL_RenderRect(renderer, &r);
                }
            }
        }
        SDL_RenderPresent(renderer);
    }
    stop_recording();
    if (!encoders.empty()) std::fprintf(stderr, "finishing video encode...\n");
    for (auto& t : encoders) t.join();
    close_session("quit");   // after a CPU halt the session was already closed as "halted"
    if (!record_input.empty() && !record_input_written) {
        std::fprintf(stderr, "warning: --record-input %s was never written (no session ran); the file is empty\n", record_input.c_str());
        scd::debug_log("--record-input %s was never written (no session ran)\n", record_input.c_str());
    }
    if (running_game) System::instance().shutdown();
    System::instance().close_log();
    SDL_Quit();
    return 0;
}
