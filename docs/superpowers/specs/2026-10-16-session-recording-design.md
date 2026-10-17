# Session recording: debug data from every play session

Date: 2026-10-16. Status: proposed. Builds on the playtest harness (`docs/superpowers/specs/2026-10-05-playtest-harness-design.md`, `docs/PLAYTEST.md`) and ADR-003 (static recompilation), whose milestones need replays of the whole game.

## Goal

Each time the user plays Snatcher in the SDL app (`snatcher`), the session leaves enough data behind that a developer or agent can, without asking the user what happened:

1. replay it exactly in `scd_headless`, light-gun sections included;
2. show that the headless replay reproduces what the user saw, and find the first frame where it does not;
3. know which build and settings produced it;
4. find the moments the user flagged as wrong, with a screenshot and RAM for each;
5. examine any hang or crash that happened during play.

Success: a session recorded on the live app, handed over as one folder, passes `tools/playtest/run.py --session DIR` (reproduces at every checkpoint), and each of the five points above can be answered from the folder alone.

## Principle

The core is deterministic given the disc, the starting backup RAM and the per-frame input. So the live app records only what cannot be rebuilt: the input (pad **and** Justifier), the starting backup RAM, the build identity, checkpoint hashes, the user's markers and evidence of failures. Anything derivable from a replay (video, the ADR-003 coverage census, per-frame CPU traces, `SCD_LIFT_VERIFY`, more checkpoints) is produced headless afterwards and is **not** recorded live.

## What exists today (2026-10-16)

- `snatcher --record-input FILE` writes a v1 replay (`# snatcher-replay v1`, header lines `disc sha1=`, `bram sha1=`, `engine`, `cdspeed`, `justifier on|off`, then `@0x<frame> <buttons>` events) and `FILE.bram`. The replay is written only at exit, so a crash of the app process loses it.
- Justifier input is not recorded; `scd_headless --replay` refuses a replay with `justifier on` (exit 4).
- `--log` / `SNATCHER_LOG=1` writes a log; F10 calls `System::dump_state()` into it; F9 records video.
- `scd_headless` has checkpoints (`--ckpt-out`, `System::state_hash()`), a watchdog (`engine/src/scd/watchdog.h`) and a failure bundle (`write_failure_bundle` in `headless.cpp`). The live app has none of these.

## Design

### Always on

Every launch of `snatcher` that boots a disc records a session. Opt out with `--no-session`. `--session-dir DIR` sets the parent directory. `--record-input FILE` stays as a compatibility alias: it records a session as usual and also writes the replay to `FILE` and `FILE.bram` at exit.

Default parent directory: `work/sessions/` if the current directory is the repository (it contains `engine/` and `work/` or can create `work/`), otherwise the per-user data directory from `SDL_GetPrefPath("snatcher", "snatcher")` + `sessions/` (for the `.app` bundle and the Windows build). The path is printed to stderr and written to the log at start.

A session directory is `<parent>/<YYYYMMDD-HHMMSS>[-N]/` (local time; `-N` added if it exists). Dropping a different disc on the window closes the current session and opens a new one. A session that never boots a disc is not created.

If the directory cannot be created, the app prints an error and continues without a session (it does not refuse to start; recording must never block play).

### Session folder layout

| File | Written | Contents |
|---|---|---|
| `manifest.json` | at start, rewritten at exit | build and session identity (below) |
| `session.replay` | appended as events happen | replay v2 (below) |
| `session.replay.bram` | at start | backup RAM at boot (copy of `bram.bin`), or absent when there is none (`bram sha1=none`) |
| `session.ckpt` | appended | checkpoints, same line format as `scd_headless --ckpt-out` |
| `session.log` | appended | the existing debug log (`--log` output), always on in a session |
| `markers/NNN-f<frame>/` | on F8 / F10 | `marker.json`, `screen.ppm`, `mainram.bin`, `subram.bin`, `state.txt` |
| `failure/NNN-<kind>-f<frame>/` | on a watchdog trip | the headless failure bundle: `report.json`, `final.ppm`, `mainram.bin`, `subram.bin`, `repro.replay`, `state.txt` |

All files hold the user's own input, hashes, screenshots and RAM: they are disc-derived or personal and stay out of git (`work/` is gitignored). Only `session.replay` and `session.ckpt` may be copied into `tools/playtest/replays/` (as today with `.replay`/`.ckpt`). The matching `.bram` is the user's save data: it stays local next to the replay and is never committed.

### manifest.json

```json
{
  "format": 1,
  "app": "snatcher",
  "git_commit": "dd7aa26", "git_dirty": false,
  "build": "translated" | "interpreter",
  "build_type": "Release",
  "os": "macOS 27.2", "sdl": "3.4.10", "renderer": "opengl",
  "args": ["--justifier"],
  "cd_speed": 1.0, "justifier": true,
  "disc_sha1": "...", "bram_sha1": "..." | "none",
  "started": "2026-10-16T20:14:03-07:00",
  "ended": "..." | null, "frames": 123456 | null,
  "exit": "quit" | "disc_changed" | "halted" | null,
  "markers": 2, "failures": 0
}
```

`git_commit`/`git_dirty` are stamped at configure/build time by CMake into a generated header (`git describe --always --dirty`, `unknown` outside a git checkout). `build` comes from `System::translated()`. `ended`, `frames` and `exit` stay `null` if the process dies; the other files still hold everything recorded up to then.

### Replay v2

Header `# snatcher-replay v2`. Same header keys as v1, plus `gun` event lines when `justifier on`:

```
@0x1a2b UCS            pad events, unchanged
g0x1a2b 143,87,in,T    gun state from this frame on: x,y, in|out, buttons T (trigger) / S (start) / - (none)
```

Gun events are recorded like pad events: only on change, held until the next event. The core applies them through `System::set_gun()` at the same point in `run_frame` where `replay_play_` applies pad input today (`system.cpp`, start of the frame), so live and replayed runs see identical gun state per frame.

`Replay::parse` accepts v1 and v2. v1 files parse exactly as today. `scd_headless --replay` stops rejecting `justifier on` for v2 replays (it calls `set_gun_connected(true)` before boot); a v1 replay with `justifier on` is still rejected (exit 4), since it has no gun data.

Recording into the session replay happens in `System` (as `replay_rec_` does now, extended with gun state), and the app flushes new events to `session.replay` at most once per second and at exit, so a crash loses at most a second of input. The header is written at session start.

### Checkpoints

The app emits a checkpoint line (`System::state_hash()`, the `--ckpt-out` format) whenever `$E022`/`$E06C` change and every 300 frames, plus one at exit. The writer moves from a lambda in `headless.cpp` into a small shared unit (`engine/src/scd/checkpoint.h/.cpp`) used by both front ends, so the format cannot drift. Lines are flushed with the replay.

Cost: one `state_hash()` per 300 frames plus scene changes. If profiling shows a visible stall, the interval becomes configurable; the default stays 300 to match `scd_headless`.

### Markers (F8, F10)

- **F8** "this looks wrong": writes `markers/NNN-f<frame>/` with `marker.json` (`{"frame", "e020", "e022", "e06c", "main_pc", "sub_pc", "time"}`), `screen.ppm`, `mainram.bin` (Main RAM `$FF0000`, 64 KB), `subram.bin` (Sub PRG RAM `$7000..$D000`, the same window `write_failure_bundle` uses), and `state.txt` (the `System::dump_state()` text). Logs `[user] marker NNN at frame F` and shows a brief on-screen flash so the user knows it registered.
- **F10** keeps its current behaviour (state into the log) and also writes a marker.

No text entry in the app: markers are timestamps the user can describe later. The writers (`write_ppm`, RAM dump, state text) move out of `headless.cpp` into a shared `engine/src/scd/debug_dump.h/.cpp` used by both markers and failure bundles.

### Watchdog in the live app

The app feeds `Watchdog` one sample per emulated frame, as `scd_headless --watch` does (`hang_frames` 300). On `kHang` or `kHalted` it writes `failure/NNN-<kind>-f<frame>/` with the existing bundle contents plus `state.txt`, logs it, and:

- `kHalted`: the game has stopped (as today); the session closes with `exit: "halted"`.
- `kHang`: play continues (the game may recover, e.g. a long CD seek). The watchdog re-arms only after `$E020` changes again, so one hang writes one bundle.

`kStillScreen` is logged only (static screens are legitimate).

`dump_state()` today prints to the log; the shared dump unit gains a variant that returns the text so it can be written to `state.txt` as well.

### Verifying a session

`tools/playtest/run.py` gains `--session DIR`: it reads `manifest.json`, replays `session.replay` with `session.replay.bram` in `scd_headless` (the given `--headless` binary), writes headless checkpoints and compares them with `session.ckpt` using the existing differ (`ckpt_diff.py`). Exit codes as today: 0 reproduces at every checkpoint, 1 differs visibly (first frame reported), 4 identity mismatch. With `--strict-ram` RAM-only differences fail too. The run warns when the headless binary's build kind (translated/interpreter) differs from the manifest's, because those builds already differ in RAM (ADR-003).

`docs/PLAYTEST.md` gets a "Sessions" section: where they are, what is in them, how to verify and how to promote a session's replay to the reference set.

## Error handling

- Recording never stops play: any write failure is logged once per file and that file is dropped; the session continues with the rest.
- Low disk: before each flush the app checks that the write succeeded; on failure it stops writing that file and logs why.
- A process crash (signal) leaves `manifest.json` without `ended`; everything else is valid up to the last flush (at most one second).
- Determinism hazards the design relies on: input must be applied at frame start from recorded state only (pad and gun are already sampled once per host iteration before `run_frame`); the replay frame counter is `System::frame_count()`. Anything that makes the live run depend on host timing beyond input (for example CD read timing tied to wall clock) breaks reproduction and shows up as a `--session` mismatch, which is the point of recording checkpoints.

## Testing

- `replay` unit tests (existing test target): v2 round-trip with gun events; v1 files parse unchanged; malformed gun lines rejected with a line number; `buttons_at`/`gun_at` boundaries.
- Headless end-to-end (skips without a disc): a test-only path in `scd_headless` (`--session-out DIR`) drives the same session writer the app uses with synthetic pad and gun input for N frames; then `run.py --session DIR` must report a match at every checkpoint. This covers recording, gun replay and checkpoint compatibility without a window.
- Watchdog bundle: `SCD_TEST_STICK_E020=<frame>` with `--session-out` must produce exactly one `failure/*-hang-*` directory with all files.
- Manifest: `git_commit` matches `git rev-parse --short HEAD` in a build from this checkout; `build` matches the binary.
- Manual (documented in the plan): launch the app, play briefly, press F8 twice, quit; the folder has two markers, a valid manifest with `ended`, and `run.py --session` passes. Repeat with `--justifier` on a shooting scene.

## Out of scope

- Savestates or rewinding (no savestate support exists; the replay plus determinism covers reproduction).
- Recording video, audio, the coverage census or CPU traces live (regenerate headless).
- Uploading or sharing sessions; pruning old sessions (the size is logged; the user deletes folders).
- Text annotations typed into the app.
- Making the translated build reproduce the interpreter build (ADR-003 M3+); a session is verified against the same build kind it was recorded with.
