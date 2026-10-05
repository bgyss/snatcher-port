#!/usr/bin/env bash
# Run reference replays on one headless build and write a fallback census per replay (ADR-003 M1).
# usage: tools/lift/census.sh <scd_headless> <out dir> <replay>...
# Backup RAM comes from <replay>.bram when present. SCD_DUMP_OVERLAYS (if set) is passed through; CENSUS_FRAMES sets the run length.
set -euo pipefail
headless=$1; out=$2; shift 2
frames=${CENSUS_FRAMES:-60000}
mkdir -p "$out"
if [ -n "${SCD_DUMP_OVERLAYS:-}" ]; then mkdir -p "$SCD_DUMP_OVERLAYS"; fi
status=0
for replay in "$@"; do
  name=$(basename "$replay" .replay)
  args=(--headless "$headless" --frames "$frames")
  if [ -f "$replay.bram" ]; then args+=(--bram "$replay.bram"); fi
  echo "census: $name"
  if ! SCD_COVERAGE="$out/$name.tsv" .venv/bin/python tools/playtest/run.py "$replay" "${args[@]}"; then
    echo "census: $name did not finish cleanly; its census is still written" >&2
    status=1
  fi
done
exit $status
