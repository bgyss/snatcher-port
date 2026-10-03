// Present-stall probe for SDL renderers (see docs/MACOS_NOTES.md). Mimics the front-end: a 320x240 streaming texture
// updated every other present on a vsynced 120/60 Hz window. Renderer from SDL_RENDER_DRIVER (metal, opengl, gpu).
//   present_stall [seconds]   -> "<renderer> loops/s N  stalls>50ms N  worst N ms"
#include <SDL3/SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

int main(int argc, char** argv) {
    const double secs = argc > 1 ? std::atof(argv[1]) : 8.0;
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* w = SDL_CreateWindow("present stall probe", 960, 720, SDL_WINDOW_RESIZABLE);
    SDL_Renderer* r = SDL_CreateRenderer(w, nullptr);
    SDL_SetRenderVSync(r, 1);
    SDL_Texture* tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_XRGB8888, SDL_TEXTUREACCESS_STREAMING, 320, 240);
    std::vector<uint32_t> fb(320 * 240);
    const uint64_t start = SDL_GetTicksNS();
    uint64_t t = start;
    int loops = 0, stalls = 0;
    double worst = 0;
    while (SDL_GetTicksNS() - start < uint64_t(secs * 1e9)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        if (loops & 1) {
            for (auto& p : fb) p = uint32_t(loops) * 0x10101u;
            SDL_UpdateTexture(tex, nullptr, fb.data(), 320 * 4);
        }
        SDL_RenderClear(r);
        SDL_FRect d = {0, 0, 960, 720};
        SDL_RenderTexture(r, tex, nullptr, &d);
        SDL_RenderPresent(r);
        ++loops;
        uint64_t n = SDL_GetTicksNS();
        double ms = (n - t) / 1e6;
        if (ms > 50) ++stalls;
        if (ms > worst) worst = ms;
        t = n;
    }
    std::printf("%-7s loops/s %6.1f  stalls>50ms %3d  worst %5.0f ms\n", SDL_GetRendererName(r), loops / secs, stalls, worst);
    SDL_Quit();
    return 0;
}
