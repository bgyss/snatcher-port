#!/usr/bin/env bash
# Re-check the macOS Metal present stalls (docs/MACOS_NOTES.md) after an OS or SDL update. Opens test windows for
# about 2 minutes. Keep them uncovered. Builds into work/macos (gitignored).
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT=work/macos; mkdir -p "$OUT"
SDL_PREFIX=${SDL_PREFIX:-$(brew --prefix sdl3 2>/dev/null || echo /opt/homebrew)}
/usr/bin/clang++ -std=c++20 -O2 -I"$SDL_PREFIX/include" tools/macos/present_stall.cpp -L"$SDL_PREFIX/lib" -lSDL3 -o "$OUT/present_stall"
/usr/bin/clang++ -std=c++20 -O2 -fobjc-arc -I"$SDL_PREFIX/include" tools/macos/raw_metal_stall.mm -L"$SDL_PREFIX/lib" -lSDL3 \
  -framework Metal -framework QuartzCore -o "$OUT/raw_metal_stall"
echo "macOS $(sw_vers -productVersion) ($(sw_vers -buildVersion)), SDL $(grep -m1 'Version:' "$SDL_PREFIX/lib/pkgconfig/sdl3.pc" | cut -d' ' -f2)"
for rep in 1 2 3; do
  for d in metal gpu opengl; do SDL_RENDER_DRIVER=$d "$OUT/present_stall" 8; done
  "$OUT/raw_metal_stall" default 8
done
echo "Healthy: ~120 loops/s (or ~60 on a 60 Hz display) and 0 stalls in every run. A single stalling run means it is not fixed."
