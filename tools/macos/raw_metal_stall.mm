// Present-stall probe for CAMetalLayer without SDL's renderer (see docs/MACOS_NOTES.md): if this stalls too, the
// problem is in macOS, not SDL. SDL only provides the window and the layer.
//   raw_metal_stall [default|two|nosync] [seconds]
//     two: maximumDrawableCount = 2;  nosync: displaySyncEnabled = NO, paced with an 8.3 ms sleep
#include <SDL3/SDL.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    const std::string mode = argc > 1 ? argv[1] : "default";
    const double secs = argc > 2 ? std::atof(argv[2]) : 8.0;
    SDL_Init(SDL_INIT_VIDEO);
    SDL_Window* w = SDL_CreateWindow("raw metal stall probe", 960, 720, SDL_WINDOW_METAL);
    SDL_MetalView view = SDL_Metal_CreateView(w);
    CAMetalLayer* layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    layer.device = dev;
    layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly = NO;
    if (mode == "nosync") layer.displaySyncEnabled = NO;
    if (mode == "two") layer.maximumDrawableCount = 2;
    id<MTLCommandQueue> queue = [dev newCommandQueue];
    id<MTLTexture> tex = [dev newTextureWithDescriptor:[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                                         width:320 height:240 mipmapped:NO]];
    static uint32_t fb[320 * 240];
    const uint64_t start = SDL_GetTicksNS();
    uint64_t t = start;
    int loops = 0, stalls = 0;
    double worst = 0;
    while (SDL_GetTicksNS() - start < uint64_t(secs * 1e9)) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {}
        @autoreleasepool {
            for (auto& p : fb) p = uint32_t(loops) * 0x10101u;
            [tex replaceRegion:MTLRegionMake2D(0, 0, 320, 240) mipmapLevel:0 withBytes:fb bytesPerRow:320 * 4];
            id<CAMetalDrawable> d = [layer nextDrawable];   // the stalls are here: a ~1 s semaphore timeout
            if (!d) continue;
            id<MTLCommandBuffer> cb = [queue commandBuffer];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = d.texture;
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(320, 240, 1)
                      toTexture:d.texture destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [bl endEncoding];
            [cb presentDrawable:d];
            [cb commit];
        }
        ++loops;
        if (mode == "nosync") SDL_DelayPrecise(8333333);
        uint64_t n = SDL_GetTicksNS();
        double ms = (n - t) / 1e6;
        if (ms > 50) ++stalls;
        if (ms > worst) worst = ms;
        t = n;
    }
    std::printf("raw-metal %-7s loops/s %6.1f  stalls>50ms %3d  worst %5.0f ms\n", mode.c_str(), loops / secs, stalls, worst);
    SDL_Metal_DestroyView(view);
    SDL_Quit();
    return 0;
}
