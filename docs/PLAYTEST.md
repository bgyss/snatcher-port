# Playtest harness

Deterministic input replay, a hang/crash watchdog and per-scene checkpoints for `scd_headless`, so the whole game can be driven unattended and two builds compared (for example the interpreter against the translated build). Design: `docs/superpowers/specs/2026-10-05-playtest-harness-design.md`.

## Record a session

Play in the app: every launch records a session folder (see [Sessions](#sessions)). For a plain replay file, `engine/build/snatcher "<disc>.cue" --record-input work/session.replay` also writes the first session's replay to `work/session.replay` and its starting backup RAM to `work/session.replay.bram` at exit. The live app loads `bram.bin` while headless starts empty, so a replay is only valid against the backup RAM it was recorded with.

## Sessions

Every launch of `snatcher` that boots a disc records a session: everything needed to replay it headless and check that the replay matches what was on screen. `--no-session` turns it off, and `--session-dir DIR` picks the parent directory.

**Where:**
- `work/sessions/<YYYYMMDD-HHMMSS>/` when started from the repository.
- Otherwise `sessions/` in the per-user data directory: `~/Library/Application Support/snatcher-port/snatcher/sessions/` on macOS, `%APPDATA%\snatcher-port\snatcher\sessions\` on Windows.

The path is printed at start and written to the log. Dropping a different disc on the window closes the session and starts a new one.

| File | Contents |
|---|---|
| `manifest.json` | build (`git_commit`, `git_dirty`, `build` translated/interpreter, `build_type`), OS, SDL, renderer, arguments, CD speed, Justifier, checkpoint interval, disc and backup-RAM SHA-1, start and end time, frames, exit (`quit`, `disc_changed`, `halted`; `null` if the app was killed), marker and failure counts |
| `session.replay` | replay v2 (pad and Justifier), appended about once a second |
| `session.replay.bram` | the backup RAM at boot (absent when there was none) |
| `session.ckpt` | checkpoints on scene changes, every 300 frames and at exit |
| `session.log` | the debug log (`--log` output) |
| `markers/NNN-f<frame>/` | F8 or F10: `marker.json` (frame, `$E020/$E022/$E06C`, both PCs, time), `screen.ppm`, `mainram.bin`, `subram.bin`, `state.txt` |
| `failure/NNN-<kind>-f<frame>/` | watchdog hang or halted CPU: the failure bundle (below) |

**Markers.** F8 means "this looks wrong": the app writes a marker and frames the picture in white for a moment. F10 also writes the state into the log. Note what you saw with the marker number afterwards.

**Watchdog.** A hang writes one bundle and play continues. The game may recover, and the next stall gets its own bundle. A halted CPU writes one bundle and closes the session.

**Verifying.** Verify a session against the build kind it was recorded with (the app from `mise run build` is the translated build):

```sh
.venv/bin/python tools/playtest/run.py --session work/sessions/20261016-201403 --headless engine/build/scd_headless
# or: mise run playtest:session work/sessions/20261016-201403
```

Exit codes:
- 0: reproduces at every checkpoint.
- 1: differs visibly. The first frame is printed. With `--strict-ram`, RAM-only differences also fail.
- 2: not a session folder.
- 4: identity mismatch (wrong disc or backup RAM).

A killed session (`ended: null`) is verified up to its last checkpoint. A warning is printed when the headless binary is a different build kind from the manifest's.

**Keeping a session.** To keep it as a reference replay, copy `session.replay` and `session.ckpt` to `tools/playtest/replays/<name>.replay` / `.ckpt`. Everything else stays local: the `.bram`, markers, failure bundles, log and manifest hold your save data, screenshots and RAM. Delete old session folders yourself. The log's last line gives the size.

**Headless test path.**
- `scd_headless <cue> --session-out DIR [--marker-at F ...] [--justifier --gun F:X,Y,in|out,BTNS ...]` records a session with the same writer into `DIR`. It flushes once per emulated second, so tests are deterministic.
- `SCD_TEST_KILL_AT=<frame>` exits at once after that frame, without cleanup (a killed app).
- `--manifest-out FILE` writes a manifest for an ordinary run.

## Replay it

```sh
.venv/bin/python tools/playtest/run.py work/session.replay --headless engine/build-interp/scd_headless --bram work/session.replay.bram --frames 30000
```

`run.py` runs `scd_headless --replay --watch --ckpt-out` on a temporary copy of the backup RAM (the core writes it back on exit). Exit codes: 0 pass, 1 builds differ visibly, 3 watchdog trip, 4 replay identity mismatch (wrong disc or backup RAM).

Compare two builds with `--compare-headless OTHER`; check the engine's own determinism with `--determinism` (same build twice, must match at every checkpoint).

## Replay format

Header `# snatcher-replay v2` (v1 files still load), then `disc sha1=`, `bram sha1=`, `engine`, `cdspeed`, `justifier on|off`, then events. Each event holds until the next one of its kind:

```
@0x1a2b UCS                pad buttons from this boot frame (U D L R B C A S, - for none)
g0x1a2b 143,87,in,T        Justifier from this frame: x,y in screen pixels, in|out of the screen, T trigger / S start / TS / -
```

`scd_headless --replay` connects the Justifier for a v2 replay with `justifier on`. A v1 replay with `justifier on` is refused (exit 4) because it has no gun input. `scd_headless --justifier [--gun F:X,Y,in|out,BTNS ...]` drives the gun directly, with entries in frame order.

## Checkpoints

`--ckpt-out FILE [--ckpt-every N]` writes one line whenever `$E022`/`$E06C` change and every N frames (default 300):

```
@0x12c scene=E022:0001/E06C:0000 vram=<h> ram=<h> fb=<h>
```

All hashes are FNV-1a. `fb` is the framebuffer, `vram` is VRAM+CRAM+VSRAM+VDP registers, `ram` is Main RAM plus Sub PRG RAM `$7000..$D000`. The differ reports the first checkpoint where `vram` or `fb` differ ("visible") and, separately, the first difference of any kind. A RAM-only difference is reported but passes unless `--strict-ram`: the translated and interpreted builds differ in stale stack bytes below SP (they push different upper PC bytes), so `ram` is noisy between those builds. It is exact between two runs of the same build.

## Watchdog

`scd_headless --watch [--hang-frames N]` checks every frame: `$E020` unchanged for N frames (default 300) is a hang; a halted CPU (unexpected illegal opcode) is a crash; an identical framebuffer for 1800 frames is a warning only (static screens are legitimate). On a hang or crash it exits 3 and writes into `--out`: `report.json` (kind, frame, `$E020/$E022/$E06C`, both PCs), `final.ppm`, `mainram.bin`, `subram.bin`, `repro.replay` (the input that led there), `state.txt` (both CPUs, VDP, CD and PCM state), and logs the same state.

`SCD_TEST_STICK_E020=<frame>` pins the sampled `$E020` from that frame on (a synthetic hang for tests; the emulation is untouched).

## Committing

Replays and checkpoints hold only input events and hashes and may be committed (`tools/playtest/replays/`). `.replay.bram` files are your save data, and everything under `work/` is gitignored: do not commit them.

## Tests

`mise run test-playtest` (unit tests for the replay format and watchdog, then the end-to-end tests, which skip without a disc). Convert an existing `--press` scenario with `tools/playtest/press_to_replay.py "" 2400:S:5 ...`.

## Later

An autonomous explorer (reads RAM state, picks presses) will write the same replay format, so anything it finds can be replayed deterministically.
