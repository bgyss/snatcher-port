#!/usr/bin/env bash
# CI player packages: interpreter-only, static SDL, no game data. Usage: tools/package/ci_package.sh mac|windows|linux
#   mac      dist/ci/Snatcher-mac.zip (universal arm64 + x86_64, macOS 12+; delegates to mac_app.sh --shareable)
#   linux    dist/ci/Snatcher-linux-x86_64.tar.gz
#   windows  dist/ci/Snatcher-windows-x64/ (snatcher.exe + README.txt; the workflow zips it). Run from an MSYS2 MinGW-w64 shell.
# Also runs the disc-free tests and refuses to package a binary containing translated (disc-derived) code.
set -euo pipefail
cd "$(dirname "$0")/../.."
PLATFORM=${1:?usage: ci_package.sh mac|windows|linux}
OUT=dist/ci
mkdir -p "$OUT"

if [ "$PLATFORM" = mac ]; then
  tools/package/mac_app.sh --shareable
  BUILD=engine/build-share
  cp dist/share/Snatcher-mac.zip "$OUT/"
else
  BUILD=engine/build-ci
  EXTRA=()
  [ "$PLATFORM" = windows ] && EXTRA+=(-G Ninja)
  [ "$PLATFORM" = linux ] && EXTRA+=(-DCMAKE_EXE_LINKER_FLAGS=-static-libstdc++\ -static-libgcc)
  # -U clears a cached SNATCHER_TRANSLATE_DIR: a shareable build must never contain translated game code.
  cmake -S engine -B "$BUILD" -DCMAKE_BUILD_TYPE=Release -USNATCHER_TRANSLATE_DIR -DSNATCHER_FETCH_SDL=ON "${EXTRA[@]}"
  cmake --build "$BUILD" -j
fi

# Disc-free tests (the end-to-end ones skip without a disc).
"$BUILD/replay_test"
"$BUILD/watchdog_test"
SCD_HEADLESS="$PWD/$BUILD/scd_headless" "${PYTHON:-python3}" tools/playtest/test_playtest.py

[ "$PLATFORM" = mac ] && { echo "packaged $OUT/Snatcher-mac.zip"; exit 0; }   # mac_app.sh already ran its translated-code guard

EXE="$BUILD/snatcher"; [ "$PLATFORM" = windows ] && EXE="$BUILD/snatcher.exe"
if nm "$EXE" | grep -Eq 'lift.*_run'; then
  echo "error: $EXE contains translated (disc-derived) code; not packaging" >&2
  exit 1
fi

if [ "$PLATFORM" = linux ]; then
  DIR="$OUT/Snatcher-linux-x86_64"
  mkdir -p "$DIR"
  cp "$EXE" "$DIR/snatcher"
  cp tools/package/PLAYTEST_README_PC.txt "$DIR/README.txt"
  tar -czf "$OUT/Snatcher-linux-x86_64.tar.gz" -C "$OUT" Snatcher-linux-x86_64
  echo "packaged $OUT/Snatcher-linux-x86_64.tar.gz"
else
  DIR="$OUT/Snatcher-windows-x64"
  mkdir -p "$DIR"
  cp "$EXE" "$DIR/snatcher.exe"
  cp tools/package/PLAYTEST_README_PC.txt "$DIR/README.txt"
  echo "staged $DIR"
fi
