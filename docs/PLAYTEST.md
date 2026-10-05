# Playtest harness

Deterministic input replay, a hang/crash watchdog and per-scene checkpoints for `scd_headless`, so the whole game can be driven unattended and two builds compared (for example the interpreter against the translated build). Design: `docs/superpowers/specs/2026-10-05-playtest-harness-design.md`.

## Record a session

```sh
engine/build/snatcher "<disc>.cue" --record-input work/session.replay
```

Play, then quit (Cmd-Q or close the window). This writes `work/session.replay` (pad input per emulated frame, keyed by boot frame count) and `work/session.replay.bram` (the backup RAM the session started with). The header holds the disc SHA-1 and the starting backup-RAM SHA-1. The live app loads `bram.bin` while headless starts empty, so a replay is only valid against the backup RAM it was recorded with.

## Replay it

```sh
.venv/bin/python tools/playtest/run.py work/session.replay --headless engine/build-interp/scd_headless --bram work/session.replay.bram --frames 30000
```

`run.py` runs `scd_headless --replay --watch --ckpt-out` on a temporary copy of the backup RAM (the core writes it back on exit). Exit codes: 0 pass, 1 builds differ visibly, 3 watchdog trip, 4 replay identity mismatch (wrong disc or backup RAM).

Compare two builds with `--compare-headless OTHER`; check the engine's own determinism with `--determinism` (same build twice, must match at every checkpoint).

## Checkpoints

`--ckpt-out FILE [--ckpt-every N]` writes one line whenever `$E022`/`$E06C` change and every N frames (default 300):

```
@0x12c scene=E022:0001/E06C:0000 vram=<h> ram=<h> fb=<h>
```

All hashes are FNV-1a. `fb` is the framebuffer, `vram` is VRAM+CRAM+VSRAM+VDP registers, `ram` is Main RAM plus Sub PRG RAM `$7000..$D000`. The differ reports the first checkpoint where `vram` or `fb` differ ("visible") and, separately, the first difference of any kind. A RAM-only difference is reported but passes unless `--strict-ram`: the translated and interpreted builds differ in stale stack bytes below SP (they push different upper PC bytes), so `ram` is noisy between those builds. It is exact between two runs of the same build.

## Watchdog

`scd_headless --watch [--hang-frames N]` checks every frame: `$E020` unchanged for N frames (default 300) is a hang; a halted CPU (unexpected illegal opcode) is a crash; an identical framebuffer for 1800 frames is a warning only (static screens are legitimate). On a hang or crash it exits 3 and writes into `--out`: `report.json` (kind, frame, `$E020/$E022/$E06C`, both PCs), `final.ppm`, `mainram.bin`, `subram.bin`, `repro.replay` (the input that led there), and logs `dump_state()`.

`SCD_TEST_STICK_E020=<frame>` pins the sampled `$E020` from that frame on (a synthetic hang for tests; the emulation is untouched).

## Committing

Replays and checkpoints hold only input events and hashes and may be committed (`tools/playtest/replays/`). `.replay.bram` files are your save data, and everything under `work/` is gitignored: do not commit them.

## Tests

`mise run test-playtest` (unit tests for the replay format and watchdog, then the end-to-end tests, which skip without a disc). Convert an existing `--press` scenario with `tools/playtest/press_to_replay.py "" 2400:S:5 ...`.

## Later

An autonomous explorer (reads RAM state, picks presses) will write the same replay format, so anything it finds can be replayed deterministically.
