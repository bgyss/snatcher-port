#!/usr/bin/env bash
# Build dist/Snatcher.app (arm64) from engine/build-macapp. Bundles libSDL3. Needs: cmake, clang, brew sdl3.
set -euo pipefail
cd "$(dirname "$0")/../.."
export CC=/usr/bin/clang CXX=/usr/bin/clang++; cmake -S engine -B engine/build-macapp -DCMAKE_BUILD_TYPE=Release
cmake --build engine/build-macapp -j8
APP=dist/Snatcher.app; mkdir -p $APP/Contents/MacOS $APP/Contents/Frameworks
cp engine/build-macapp/snatcher $APP/Contents/MacOS/Snatcher
SDL=$(otool -L engine/build-macapp/snatcher | awk '/libSDL3/{print $1;exit}')
cp -L "$SDL" $APP/Contents/Frameworks/libSDL3.0.dylib
install_name_tool -change "$SDL" @executable_path/../Frameworks/libSDL3.0.dylib $APP/Contents/MacOS/Snatcher
cat > $APP/Contents/Info.plist <<P
<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>
<key>CFBundleName</key><string>Snatcher</string><key>CFBundleExecutable</key><string>Snatcher</string>
<key>CFBundleIdentifier</key><string>net.snatcher-port.snatcher</string><key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/></dict></plist>
P
codesign --force --deep -s - $APP
echo "built $APP (run: open $APP --args /path/to/disc.cue)"
