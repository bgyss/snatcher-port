// SDL3 front-end: window, audio, keyboard and gamepad input for the Sega CD core.
//   snatcher <disc.cue>      (or drop a .cue file onto the window)
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

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
    if (argc > 1) boot(argv[1], &running_game);
    SDL_SetWindowTitle(window, running_game ? "Snatcher" : "Snatcher - drop a .cue file on this window");

    bool quit = false;
    while (!quit) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) quit = true;
            else if (e.type == SDL_EVENT_DROP_FILE && e.drop.data) {
                if (running_game) System::instance().shutdown();
                running_game = false;
                if (boot(e.drop.data, &running_game)) SDL_SetWindowTitle(window, "Snatcher");
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_F11) {
                bool fs = SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(window, !fs);
            } else if (e.type == SDL_EVENT_KEY_DOWN && e.key.key == SDLK_ESCAPE) quit = true;
        }
        if (running_game) {
            System& sys = System::instance();
            sys.set_pad(0, keyboard_buttons() | gamepad_buttons(pad));
            // Audio-paced: keep roughly 3 frames of audio queued.
            const int target_bytes = kSampleRate / 20 * 4;
            if (!audio || SDL_GetAudioStreamQueued(audio) < target_bytes) {
                sys.run_frame();
                if (audio && !sys.audio().empty())
                    SDL_PutAudioStreamData(audio, sys.audio().data(), int(sys.audio().size() * sizeof(int16_t)));
                sys.audio().clear();
                if (sys.halted()) running_game = false;
            }
            SDL_UpdateTexture(tex, nullptr, sys.framebuffer(), 320 * 4);
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
    if (running_game) System::instance().shutdown();
    SDL_Quit();
    return 0;
}
