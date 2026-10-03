# Playing the port

How to build, run and share the playable Mac build. (Windows: `mise run package:windows` builds
`dist/windows/snatcher.exe`; the controls and options below are the same.)

## You need

- **Your own disc image** of the US Sega CD release: a `.cue` plus its `.bin` file(s). Both a single `.bin` and the
  Redump one-`.bin`-per-track layout work. Keep them together. CHD and ISO+MP3 are not supported (`chdman extractcd`
  converts a CHD back to cue/bin). No BIOS file is needed: the Sega CD BIOS is emulated.
- macOS with Apple clang (Xcode command-line tools) and `cmake`, plus SDL3 from Homebrew (`brew install sdl3`) or the
  Nix shell (`nix develop`).

## Two builds

| | Personal build | Shareable build |
|---|---|---|
| Command | `mise run package:mac` | `mise run package:mac-share` (`tools/package/mac_app.sh --shareable`) |
| Output | `dist/Snatcher.app` | `dist/share/Snatcher-mac.zip` (app + player README) |
| CPU code | translated to C++ from your disc (needs `mise run extract` first; without it you get the interpreter) | interpreter only |
| Runs on | this Mac's macOS and architecture | Apple Silicon + Intel, macOS 12+ (static SDL) |
| Share it? | **Never**: it contains code generated from your disc | Yes, with players who own the game |

Both play the same: frames take ~2 ms either way, well inside the 16.7 ms budget. The translated build is the one used
for development and differential testing.

## Run

```sh
open dist/Snatcher.app --args "sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue"
# or open the app and drag a .cue onto its window
# or run the binary directly (keeps log output in the terminal):
engine/build/snatcher "$SCD_CUE"            # after `mise run build`
```

Options (any order after the cue):

| Option / variable | Effect |
|---|---|
| `--justifier` | mouse = Konami Justifier on port 2 (left click = trigger, right = start). Must be given at launch: the game detects the gun at boot |
| `--record FILE.mp4` | record from boot; `F9` starts/stops a recording any time (saved to `~/Movies/snatcher-<time>.mp4`, needs ffmpeg) |
| `--video-codec h265` | H.265 instead of H.264 for recordings |
| `SNATCHER_FPS=1` | print renderer, pacing mode and per-second fps / audio / vsync-hold stats |
| `SDL_RENDER_DRIVER=metal` | override the renderer (default on macOS: OpenGL, see `docs/MACOS_NOTES.md`) |

The first run with a fresh save opens the Options screen; select QUIT to continue into the story.

## Controls

| Keyboard | Gamepad | Sega CD pad |
|---|---|---|
| Arrow keys | D-pad / left stick | D-pad |
| Z | X / Square (west) | A |
| X | A / Cross (south) | B |
| C | B / Circle (east) | C (selects, advances text) |
| Return | Start | Start |
| F11 | | fullscreen |
| F9 | | start/stop recording |
| Esc | | quit immediately |

Connect a gamepad before launching (the first one found is used).

## Saves

Backup RAM is stored at `~/Library/Application Support/snatcher-port/snatcher/bram.bin` and written when the app
quits. Delete it to start with a fresh save.

## Sharing with a playtester

1. `mise run package:mac-share` → `dist/share/Snatcher-mac.zip`. The script refuses to package a binary that
   contains translated code.
2. Send the zip. The player supplies their own disc. Its `README.txt` (from `tools/package/PLAYTEST_README.txt`)
   covers the Gatekeeper prompt (the app is not notarized), controls and how to report problems.

## Troubleshooting

- **"Apple could not verify…"**: the app is ad-hoc signed. macOS 15+: System Settings > Privacy & Security > Open
  Anyway. Older: Control-click > Open. Or `xattr -dr com.apple.quarantine Snatcher.app`.
- **Stutter**: run with `SNATCHER_FPS=1`; on 120 Hz the healthy line is `vsyncs per frame 2:` ~60. See
  `docs/MACOS_NOTES.md` for the Metal stall and how to re-check it.
- **"cannot open image"**: the `.bin` names inside the `.cue` must match the files next to it.
