# macOS platform notes (revisit after OS / SDL updates)

Open issues tied to specific macOS and SDL versions, with the workaround currently in the code and how to check
whether it is still needed. Re-run the check after every macOS point release and SDL bump, and update this file.

## 1. Metal presents stall for up to 1 s (workaround: prefer the OpenGL renderer)

**Found:** 2026-10-02, while tracking down frame stutter in the SDL front-end.
**Seen on:** macOS 27.2 (build 26B5091g), Apple M1 Max, built-in 120 Hz ProMotion (Liquid Retina XDR), SDL 3.4.10
(Homebrew). Windows and external displays were not tested.

**Symptom.** With SDL's `metal` renderer (the macOS default), `SDL_RenderPresent` sometimes blocks for 50 ms to
1000 ms. In the game that showed up as heavy stutter: the loop managed 20–40 presents/s instead of 120, and
emulation fell to 2–27 fps because the pacer dropped the lost time.

**What it is.** A stack sample during a stall shows the time in `-[CAMetalLayer nextDrawable]` →
`CAMetalLayerPrivateNextDrawableLocked` → `semaphore_timedwait_trap`, i.e. the layer has no free drawable and
waits for the ~1 s drawable timeout. The present path also goes through Apple's `FramePacing` framework
(`FPCAMetalLayerState commandBufferCommit:commitTime:`), which is new in recent macOS.

- It isn't SDL's fault: a raw CAMetalLayer loop with no SDL renderer (`tools/macos/raw_metal_stall.mm`) stalls the
  same way in some runs, with vsync on (`displaySyncEnabled`) or off, and with 2 or 3 drawables.
- It's intermittent: some 8-second runs are clean, and the next one stalls a dozen times. Window focus,
  `SDL_RaiseWindow`, and the window being frontmost made no difference. No `SDL_EVENT_WINDOW_OCCLUDED` arrives, and
  the window server reports the window on screen.
- `gpu` (SDL_GPU, which also runs on Metal) stalls the same way.
- `opengl` never stalled in any run (more than 15 runs of 6–8 s): a steady 120 presents/s, worst interval ≈ 20–40 ms.

**Workaround in the code.** `engine/src/frontend/sdl_main.cpp` sets `SDL_HINT_RENDER_DRIVER` to `"opengl,metal"`
on Apple builds. SDL falls back to Metal if OpenGL can't be created, and the `SDL_RENDER_DRIVER` environment
variable overrides the hint (`SDL_RENDER_DRIVER=metal` to test). OpenGL has been deprecated on macOS since 10.14,
so the long-term goal is to return to Metal.

**How to re-check.**

```sh
tools/macos/check_present.sh      # ~2 min, opens test windows; keep them uncovered
```

It prints one line per renderer per round (3 rounds). Metal is healthy again when `metal`, `gpu` and `raw-metal`
show about 120 loops/s (about 60 on a 60 Hz display) and `stalls>50ms 0` in **every** round, since a single bad round means
the bug is still there. Then remove the hint, run the game with `SNATCHER_FPS=1`, and confirm that
`vsyncs per frame` stays in the `2:` bucket (on 120 Hz) for a minute or more.

Results on 2026-10-02 (macOS 27.2, SDL 3.4.10), one `check_present.sh` run:

| renderer | round 1 | round 2 | round 3 |
|---|---|---|---|
| metal | 117 loops/s, 1 stall | 44 loops/s, 14 stalls (1004 ms) | 26 loops/s, 16 stalls (1002 ms) |
| gpu | 6.5 loops/s, 13 stalls (1120 ms) | 7.2 loops/s, 12 stalls | 118 loops/s, 0 |
| opengl | 118 loops/s, 0 | 120 loops/s, 0 | 120 loops/s, 0 |
| raw-metal | 119 loops/s, 0 | 120 loops/s, 0 | 119 loops/s, 0 |

(Raw Metal was clean in that run but stalled in earlier ones: 45 loops/s with 16 stalls of up to 1002 ms, and 11 loops/s
with 22 stalls with `maximumDrawableCount = 2`.)

**If it's still broken on a later version,** consider reporting it to Apple (Feedback Assistant) with
`raw_metal_stall.mm` as the reproducer, and to SDL (libsdl-org/SDL) with `present_stall.cpp`.

## 2. The emulator's main thread is slower in the GUI than headless (no workaround needed)

`run_frame` takes about 1.7–2 ms in `scd_headless` but averages 3.6 ms (p90 6 ms, max 11 ms) inside the SDL app on the
same machine. Raising the main thread to `QOS_CLASS_USER_INTERACTIVE` made no difference, so the likely cause is
the CPU clocking down for a bursty load (a few ms of work every 16.7 ms) rather than core placement. It still fits well
inside the frame budget. Recheck if frames start missing vsyncs: `SNATCHER_FPS=1` shows `3:`/`4+:` buckets.

## Related front-end behaviour (not version-specific)

- On a 120 Hz ProMotion panel the front-end locks emulation to vsync (one frame every 2 vsyncs) on a phase-locked vsync
  grid, and corrects audio drift by resampling (`SDL_SetAudioStreamFrequencyRatio`). See the comment above the
  pacing code in `sdl_main.cpp`.
- `SNATCHER_FPS=1` prints the renderer, the pacing mode, and every second the emulator fps, audio queue,
  resample ratio and a histogram of how many vsyncs each frame stayed on screen.
