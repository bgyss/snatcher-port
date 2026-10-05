#!/bin/bash
# Regression test for the intro skip and the Act 1 hang (docs/JOURNAL.md, 2026-10-04).
# Boots the disc headless, opens Options with Start, selects QUIT, lets the intro run, presses Start to skip it and
# checks that the game reaches the first playable scene (Junker HQ front desk) instead of a black screen or the ACT 1 card.
#   tools/test_intro_skip.sh [path/to/scd_headless] [cue]     (set SCD_NO_TRANSLATE=1 to test the interpreter)
set -e
cd "$(dirname "$0")/.."
HEADLESS=${1:-engine/build/scd_headless}
CUE=${2:-${SCD_CUE:-sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue}}
OUT=$(mktemp -d "${TMPDIR:-/tmp}/snatcher-skip-test.XXXXXX")
"$HEADLESS" "$CUE" --frames 12000 --out "$OUT" \
    --press 2400:S:5 --press 2700:D:3 --press 2760:D:3 --press 2820:D:3 --press 3000:C:3 --press 7000:S:4 >/dev/null 2>&1
python3 - "$OUT/final.ppm" <<'EOF'
import sys
d = open(sys.argv[1], 'rb').read()
# P6 header: "P6\n320 240\n255\n"
parts = d.split(b'\n', 3)
w, h = map(int, parts[1].split())
px = parts[3]
n = w * h
lit = sum(1 for i in range(0, n * 3, 3) if px[i] or px[i + 1] or px[i + 2])
cyan = sum(1 for i in range(0, n * 3, 3) if px[i] < 80 and px[i + 1] > 150 and px[i + 2] > 150)   # command menu text
frac = lit / n
print(f"lit pixels {frac:.1%}, menu-colour pixels {cyan}")
# Junker HQ: a large lit scene plus the cyan LOOK/INVESTIGATE/TALK menu or Mika's text box. A black screen is ~0%, the ACT 1 card ~2%.
if frac < 0.15:
    print("FAIL: screen is (nearly) black or still on the ACT 1 card"); sys.exit(1)
print("PASS")
EOF
echo "output kept in $OUT"
