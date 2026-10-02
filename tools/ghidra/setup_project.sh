#!/usr/bin/env bash
# Build the Snatcher Ghidra project (ghidra/snatcher.gpr) from the user's Sega CD image.
# Project files and exports are gitignored; only scripts live in git.
#
# Usage: tools/ghidra/setup_project.sh [path/to/sega-cd.bin]
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${1:-$ROOT/sega-cd-eng/Snatcher (Sega CD) (U)-redump.bin}"
CODE="$ROOT/extracted/code"
EXPORTS="$ROOT/extracted/ghidra-exports"
PROJ_DIR="$ROOT/ghidra"
PROJ=snatcher
LANG_ID="68000:BE:32:default"

if [[ -z "${GHIDRA_HOME:-}" ]]; then
  GHIDRA_HOME="$(dirname "$(readlink -f "$(command -v ghidra)")")"
fi
HEADLESS="$GHIDRA_HOME/support/analyzeHeadless"
[[ -x "$HEADLESS" ]] || { echo "analyzeHeadless not found; set GHIDRA_HOME" >&2; exit 1; }

# Export scripts from the ghidra-headless skill (trailofbits), if installed.
SKILL_SCRIPTS="$HOME/.agents/skills/ghidra-headless/scripts/ghidra_scripts"
SCRIPT_PATH="$ROOT/tools/ghidra"
[[ -d "$SKILL_SCRIPTS" ]] && SCRIPT_PATH="$SCRIPT_PATH;$SKILL_SCRIPTS"

python3 "$ROOT/tools/extract_code.py" "$IMAGE" "$CODE"
mkdir -p "$PROJ_DIR" "$EXPORTS"

run() {  # run <blob> <base> <setup args...>
  local blob="$1" base="$2"; shift 2
  local post=()
  [[ -d "$SKILL_SCRIPTS" ]] && post=(-postScript ExportAll.java)
  GHIDRA_OUTPUT_DIR="$EXPORTS" "$HEADLESS" "$PROJ_DIR" "$PROJ" \
    -import "$CODE/$blob" -overwrite \
    -loader BinaryLoader -loader-baseAddr "$base" -processor "$LANG_ID" \
    -scriptPath "$SCRIPT_PATH" \
    -preScript SnatcherSetup.java "$@" \
    "${post[@]}" \
    -log "$EXPORTS/${blob%.bin}.log"
}

run scd_main_ip.bin 0xFF0000 main
run scd_sub_sp.bin 0x6000 sub "$CODE/scd_subcode.bin"

echo "Project: $PROJ_DIR/$PROJ.gpr   Exports: $EXPORTS"
