#!/usr/bin/env bash
# Cross-build snatcher.exe (x64, static) on macOS/Linux with mingw-w64. SDL3 is fetched and linked statically.
set -euo pipefail
cd "$(dirname "$0")/../.."
cat > /tmp/mingw-toolchain.cmake <<T
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)
T
export CC=/usr/bin/clang CXX=/usr/bin/clang++
cmake -S engine -B engine/build-hostgen >/dev/null && cmake --build engine/build-hostgen --target musashi >/dev/null
unset CC CXX
CODE=${SNATCHER_CODE_DIR:-$PWD/extracted/code}
TR=; [ -f "$CODE/scd_subcode.bin" ] && TR=-DSNATCHER_TRANSLATE_DIR=$CODE
cmake -S engine -B engine/build-win2 $TR -DMUSASHI_PREGEN=$PWD/engine/build-hostgen/musashi_gen -DCMAKE_TOOLCHAIN_FILE=/tmp/mingw-toolchain.cmake -DSNATCHER_FETCH_SDL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build-win2 -j8
mkdir -p dist/windows && cp engine/build-win2/snatcher.exe dist/windows/
echo "built dist/windows/snatcher.exe"
