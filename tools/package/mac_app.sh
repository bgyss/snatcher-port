#!/usr/bin/env bash
# Build Snatcher.app. Needs: cmake, Apple clang, and brew sdl3 (personal build) or network access (--shareable fetches SDL).
#   tools/package/mac_app.sh              dist/Snatcher.app, translated if extracted/code exists (contains code derived
#                                         from your disc: personal use only, never share)
#   tools/package/mac_app.sh --shareable  dist/share/Snatcher-mac.zip: interpreter-only universal app, macOS 12+ (no disc-derived code)
#                                         plus README for players, who supply their own disc
set -euo pipefail
cd "$(dirname "$0")/../.."
export CC=/usr/bin/clang CXX=/usr/bin/clang++
SHARE=0; [ "${1:-}" = "--shareable" ] && SHARE=1
if [ $SHARE = 1 ]; then
  BUILD=engine/build-share; OUT=dist/share
  # -U clears a cached SNATCHER_TRANSLATE_DIR: a shareable build must never contain translated game code.
  # Static SDL (fetched), universal arm64 + x86_64, macOS 12+: Homebrew's SDL is built for the host OS only.
  cmake -S engine -B $BUILD -DCMAKE_BUILD_TYPE=Release -USNATCHER_TRANSLATE_DIR -DSNATCHER_FETCH_SDL=ON \
    -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 "-DCMAKE_OSX_ARCHITECTURES=arm64;x86_64"
else
  BUILD=engine/build-macapp; OUT=dist
  CODE=${SNATCHER_CODE_DIR:-$PWD/extracted/code}   # from tools/extract_code.py <your disc>; enables the translated build
  TR=; [ -f "$CODE/scd_subcode.bin" ] && TR=-DSNATCHER_TRANSLATE_DIR=$CODE
  cmake -S engine -B $BUILD -DCMAKE_BUILD_TYPE=Release $TR
fi
cmake --build $BUILD -j8
APP=$OUT/Snatcher.app; mkdir -p $APP/Contents/MacOS
cp $BUILD/snatcher $APP/Contents/MacOS/Snatcher
SDL=$(otool -L $BUILD/snatcher | awk '/libSDL3/{print $1;exit}')
chmod -R u+w $APP
if [ -n "$SDL" ]; then   # dynamic (Homebrew) SDL: bundle it; the shareable build links SDL statically
  mkdir -p $APP/Contents/Frameworks; cp -fL "$SDL" $APP/Contents/Frameworks/libSDL3.0.dylib
  install_name_tool -change "$SDL" @executable_path/../Frameworks/libSDL3.0.dylib $APP/Contents/MacOS/Snatcher
fi
cat > $APP/Contents/Info.plist <<P
<?xml version="1.0" encoding="UTF-8"?><plist version="1.0"><dict>
<key>CFBundleName</key><string>Snatcher</string><key>CFBundleExecutable</key><string>Snatcher</string>
<key>CFBundleIdentifier</key><string>net.snatcher-port.snatcher</string><key>CFBundlePackageType</key><string>APPL</string>
<key>NSHighResolutionCapable</key><true/></dict></plist>
P
codesign --force --deep -s - $APP
if [ $SHARE = 0 ]; then
  echo "built $APP (run: open $APP --args /path/to/disc.cue)"
  exit 0
fi
# Refuse to package anything that contains translated code (lift::sub_run, lift::main_run, lift::ovl909_run ...).
if nm $APP/Contents/MacOS/Snatcher | grep -q "lift.*_run"; then
  echo "error: $APP contains translated (disc-derived) code; not packaging" >&2
  exit 1
fi
cp tools/package/PLAYTEST_README.txt $OUT/README.txt
rm -f $OUT/Snatcher-mac.zip
(cd $OUT && ditto -c -k --norsrc --noextattr --keepParent Snatcher.app Snatcher-mac.zip && zip -q Snatcher-mac.zip README.txt)
echo "built $OUT/Snatcher-mac.zip (interpreter-only, universal, macOS 12+, no game data)"
