#!/usr/bin/env python3
"""Run a replay headless, optionally against a second build, and report the first divergence or watchdog trip.

Exit codes: 0 pass, 1 checkpoints differ, 3 watchdog trip, other = headless failure.
"""
import argparse
import pathlib
import shutil
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ckpt_diff

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEFAULT_CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"


def run_once(headless, cue, replay, frames, bram, out):
    out.mkdir(parents=True, exist_ok=True)
    ckpt = out / "run.ckpt"
    cmd = [headless, str(cue), "--frames", str(frames), "--out", str(out), "--replay", str(replay),
           "--ckpt-out", str(ckpt), "--watch"]
    if bram:
        tmp = out / "bram.bin"
        shutil.copy(bram, tmp)   # the core writes BRAM back on exit, so never pass the original
        cmd += ["--bram", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    return r.returncode, ckpt, r.stderr


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay")
    ap.add_argument("--headless", required=True)
    ap.add_argument("--compare-headless")
    ap.add_argument("--cue", default=str(DEFAULT_CUE))
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--bram")
    ap.add_argument("--strict-ram", action="store_true", help="fail on RAM-only differences too")
    ap.add_argument("--determinism", action="store_true", help="run the same build twice and compare")
    ap.add_argument("--work", default=str(ROOT / "work" / "playtest"))
    a = ap.parse_args()
    work = pathlib.Path(a.work)
    other = a.compare_headless or (a.headless if a.determinism else None)

    rc, ck_a, err = run_once(a.headless, a.cue, a.replay, a.frames, a.bram, work / "A")
    if rc == 3:
        print(f"WATCHDOG TRIP in A; bundle in {work / 'A'}\n{err}")
        return 3
    if rc != 0:
        print(f"A failed (exit {rc}):\n{err}")
        return rc
    if other is None:
        print(f"ok: {len(ckpt_diff.parse(ck_a.read_text()))} checkpoints in {ck_a}")
        return 0
    rc, ck_b, err = run_once(other, a.cue, a.replay, a.frames, a.bram, work / "B")
    if rc == 3:
        print(f"WATCHDOG TRIP in B; bundle in {work / 'B'}\n{err}")
        return 3
    if rc != 0:
        print(f"B failed (exit {rc}):\n{err}")
        return rc
    pa, pb = ckpt_diff.parse(ck_a.read_text()), ckpt_diff.parse(ck_b.read_text())
    visible = ckpt_diff.first_diff(pa, pb, ckpt_diff.VISIBLE_FIELDS)
    anything = ckpt_diff.first_diff(pa, pb)
    if visible:
        print(f"DIVERGED (visible): {visible}\n  first difference of any kind: {anything}")
        return 1
    if anything:
        print(f"RAM-only difference (screen and VRAM match everywhere): {anything}")
        if a.strict_ram:
            return 1
        return 0
    print("ok: builds match at every checkpoint")
    return 0


if __name__ == "__main__":
    sys.exit(main())
