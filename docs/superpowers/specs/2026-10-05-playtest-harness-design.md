# Playtest harness: design

Status: draft for review (2026-10-05)

## Goal

Run the whole game unattended and flag clear glitches (hangs, crashes, black or frozen screens, divergence between builds). The harness must be reusable to verify recompiled/translated content later, not only this engine.

Motivation: a live freeze at game frame `$E020 = 0x4d11` (after exiting Engineering). It could not be reproduced by replaying the logged presses headless, because `--press` counts host frames and the live app's timing differs. The Sub CPU sat at `$D648` polling `$FF8003` bit 1 (Word RAM hand-over) while the Main CPU sat in the V-blank poll at `$FF0920`; the CD was idle.

## Scope

Phase 1 (this spec): input recorder, `--replay`, watchdog, checkpoints, runner.
Phase 2 (later, same formats): autonomous explorer that reads RAM state and writes its input as a replay.
Out of scope: MAME comparison, audio checks.

## Architecture

1. **Deterministic input recorder.** The app (`--record-input FILE`) and headless record pad input keyed by boot frame count (`System::frame_count()`, the numbering `--press` and the debug log use), recorded once per emulated frame inside `run_frame()` (the app may run several frames per input poll). Headless gains `--replay FILE`.
2. **Watchdog** (`scd_headless --watch`): hang (`$E020` unchanged for N frames, or both CPUs looping on the same PCs), crash (illegal opcode other than the BIOS stub traps, address error, exception vectors), black/frozen screen (framebuffer hash unchanged while the game claims progress). On trigger it writes a failure bundle.
3. **Checkpoints.** Hashes recorded when game state (`$E022`/`$E06C`) changes, plus every N frames. Comparing two runs of one replay reports the first differing checkpoint and which hash differed.
4. **Explorer** (phase 2).
5. **Runner:** `tools/playtest/run.py <replay> --build X [--compare-build Y]`, wrapped as `mise run playtest`.

## File formats (both committable, no disc data)

`*.replay`:

```
# snatcher-replay v1
disc  sha1=<redump track-1 hash>
engine <git rev or build id>
cdspeed 1.0
justifier off
@0x0120 U
@0x0127 -
```

The loader refuses a replay whose disc hash does not match. Frames are boot frames, as `--press` uses; `$E020` is not the key because it is 0 before the game starts and not guaranteed monotonic across scenes. The header also records the SHA-1 of the starting backup RAM (the live app loads `bram.bin`, headless starts empty); headless refuses a replay when it differs. A `--log` session converts only approximately (host-frame stamps), hence the native recorder.

`*.ckpt`:

```
@0x0600 scene=E022:0003/E06C:0001 vram=<h> ram=<h> fb=<h>
```

`vram` is VRAM+CRAM, `ram` is hot main-RAM regions only (avoids uninitialised-area false positives), `fb` is the framebuffer. Hashes only. A `fb` difference means a visible glitch; a RAM-only difference means internal divergence, possibly harmless.

## Determinism gate (built first)

Run a replay twice headless; checkpoints must be identical, otherwise report the first divergence and fix the engine before building further. Suspects: host-time dependence, audio-driven timing, uninitialised RAM, CD read timing. Also check live vs headless: record in the app, replay headless, checkpoints must match. This is what would have reproduced the freeze.

## Comparison modes

- Interpreter vs translated build: same replay, two binaries, first divergent checkpoint reported (scene-level complement to `SCD_LIFT_VERIFY` and `lift_diff`).
- Engine vs MAME: later; `tools/emu/scripted_input.lua` already shares the frame numbering.

## Failure output

`work/playtest/<run>/` (gitignored): `report.json`, screenshot, last 300 frames of state, and the trimmed replay that reproduces the failure.

## Testing the harness

- Unit test: watchdog fed synthetic traces (frozen counter, illegal opcode) must trigger.
- The intro-skip scenario becomes the first committed replay + checkpoint file, run alongside `tools/test_intro_skip.sh`.
- The Engineering-exit freeze is a known-bad replay: the watchdog must flag it. The test expects failure until the bug is fixed, then flips to must-pass.

## Hard-rule compliance

No disc images, extracted data, captures or generated translated C++ enter the repo; replays and checkpoints hold only input events and hashes. Builds use Apple clang on macOS; no `rm -rf` (use fresh dirs or `mv`).
