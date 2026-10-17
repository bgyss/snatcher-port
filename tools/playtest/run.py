#!/usr/bin/env python3
"""Run a replay headless, optionally against a second build, and report the first divergence or watchdog trip.
With --session DIR, replay a recorded session folder (docs/PLAYTEST.md "Sessions") and compare it with its checkpoints.

Exit codes: 0 pass, 1 checkpoints differ, 2 not a session folder, 3 watchdog trip, 4 replay identity mismatch
(wrong disc or backup RAM), other = headless failure.
"""
import argparse
import json
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


def verify_session(a):
    sdir = pathlib.Path(a.session)
    try:
        m = json.loads((sdir / "manifest.json").read_text())
    except (OSError, ValueError) as e:
        print(f"error: {sdir} is not a session folder (manifest.json: {e})")
        return 2
    replay, ckpt = sdir / "session.replay", sdir / "session.ckpt"
    if not replay.exists() or not ckpt.exists():
        print(f"error: {sdir} has no session.replay or session.ckpt")
        return 2
    try:
        rec = ckpt_diff.parse(ckpt.read_text())
    except ValueError as e:
        print(f"error: {ckpt}: {e}")
        return 2
    if not rec:
        print(f"error: {ckpt} has no checkpoints")
        return 2
    if m.get("ended") is None:
        print("note: the session did not end cleanly (crash or kill); verifying up to its last checkpoint")
    frames = m.get("frames") or rec[-1].frame
    work = pathlib.Path(a.work) / "session"
    work.mkdir(parents=True, exist_ok=True)
    out_ck, out_mf = work / "replay.ckpt", work / "replay_manifest.json"
    cmd = [a.headless, str(a.cue), "--frames", str(frames), "--out", str(work), "--replay", str(replay),
           "--ckpt-out", str(out_ck), "--ckpt-every", str(m.get("ckpt_every", 300)), "--manifest-out", str(out_mf)]
    bram = sdir / "session.replay.bram"
    if bram.exists():
        tmp = work / "bram.bin"
        shutil.copy(bram, tmp)   # the core writes BRAM back on exit, so never pass the original
        cmd += ["--bram", str(tmp)]
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        print(f"headless replay failed (exit {r.returncode}):\n{r.stderr}")
        return r.returncode
    try:
        built = json.loads(out_mf.read_text()).get("build")
    except (OSError, ValueError):
        built = None
    if built and m.get("build") and built != m["build"]:
        print(f"warning: the session was recorded on the {m['build']} build but {a.headless} is the {built} build; "
              "those builds differ in RAM and may differ on screen (ADR-003)")
    rep = ckpt_diff.parse(out_ck.read_text())
    visible = ckpt_diff.first_diff(rec, rep, ckpt_diff.VISIBLE_FIELDS)
    anything = ckpt_diff.first_diff(rec, rep)
    if visible:
        print(f"DIVERGED (visible; A = session, B = headless replay): {visible}\n  first difference of any kind: {anything}")
        return 1
    if anything:
        print(f"RAM-only difference (screen and VRAM match everywhere; A = session, B = headless replay): {anything}")
        return 1 if a.strict_ram else 0
    print(f"ok: session reproduces at all {len(rec)} checkpoints ({frames} frames)")
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("replay", nargs="?")
    ap.add_argument("--session", metavar="DIR", help="verify a recorded session folder against its checkpoints")
    ap.add_argument("--headless", required=True)
    ap.add_argument("--compare-headless")
    ap.add_argument("--cue", default=str(DEFAULT_CUE))
    ap.add_argument("--frames", type=int, default=20000)
    ap.add_argument("--bram")
    ap.add_argument("--strict-ram", action="store_true", help="fail on RAM-only differences too")
    ap.add_argument("--determinism", action="store_true", help="run the same build twice and compare")
    ap.add_argument("--work", default=str(ROOT / "work" / "playtest"))
    a = ap.parse_args()
    if a.session:
        return verify_session(a)
    if not a.replay:
        ap.error("give a replay file or --session DIR")
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
