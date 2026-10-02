#!/usr/bin/env bash
# Launch Snatcher in MAME with the debugger open.
#
#   tools/emu/mame_debug.sh scd [extra mame args]   Sega CD (USA)  - driver segacd
#   tools/emu/mame_debug.sh pce [extra mame args]   PC Engine CD   - driver pce + System Card 3
#
# BIOS images are NOT in the repo. Defaults:
#   Sega CD : $MAME_ROMPATH/segacd/mpr-15045b.bin (or segacd.zip), MAME_ROMPATH=~/mame/roms
#   PCE     : $PCE_SYSCARD, default ~/Documents/RetroArch/system/syscard3.pce
# Useful extras: -nodebug (plain run), -debugscript tools/emu/<file>.cmd, -window -nomaximize
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ROMPATH="${MAME_ROMPATH:-$HOME/mame/roms}"
SYSCARD="${PCE_SYSCARD:-$HOME/Documents/RetroArch/system/syscard3.pce}"
which="${1:-scd}"; shift || true

STATE="$ROOT/work/mame"   # gitignored: cfg, nvram (BRAM saves), snapshots, debugger state
mkdir -p "$STATE"
common=(-rompath "$ROMPATH" -window -nomaximize -debug -skip_gameinfo
        -cfg_directory "$STATE/cfg" -nvram_directory "$STATE/nvram"
        -snapshot_directory "$STATE/snap" -state_directory "$STATE/sta"
        -diff_directory "$STATE/diff" -comment_directory "$STATE/comments")
case "$which" in
  scd) exec mame segacd "${common[@]}" -cdrm "$ROOT/sega-cd-eng/Snatcher (Sega CD) (U)-redump.cue" "$@" ;;
  pce) exec mame pce "${common[@]}" -cart "$SYSCARD" -cdrm "$ROOT/pcengine-cd-jpn/Snatcher_(NTSC-J)_[KMCD2002].cue" "$@" ;;
  *) echo "usage: $0 scd|pce [mame args]" >&2; exit 1 ;;
esac
