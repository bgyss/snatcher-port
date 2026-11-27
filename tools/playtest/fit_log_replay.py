#!/usr/bin/env python3
"""Rebuild a replay from an app debug log (--log / debug.log) that has no session folder.

The app logs `[pad]` changes once per host iteration, so their frames are approximate, and a replay built from
them drifts off the recorded route at the first menu press that lands differently. This tool fits the replay to
the log: it replays headless, compares the `[game]` state changes and `[cd]` reads with the log, and at the first
mismatch tries small edits to the presses just before it (shift by a few frames, drop one) until the headless run
matches again. Then it continues from there.

    .venv/bin/python tools/playtest/fit_log_replay.py debug.log --bram bram.bin --headless engine/build/scd_headless \
        --out work/fit

Writes <out>/fitted.replay (and keeps every trial run under <out>/trials/). The backup RAM must be the one the app
booted with; the app's bram.bin is only right if the game did not save during that run.
"""
import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
LINE = re.compile(r"^\[(game|cd|pad)\] f=(\d+) (.*)$")


def parse_log(path):
    """-> (tokens, pads). tokens: [(frame, kind, text)] for game/cd lines; pads: [(frame, buttons)]."""
    toks, pads = [], []
    for ln in Path(path).read_text(errors="replace").splitlines():
        m = LINE.match(ln)
        if not m:
            continue
        kind, f, rest = m.group(1), int(m.group(2)), m.group(3)
        if kind == "pad":
            if rest.startswith("port1 "):
                pads.append((f, rest[6:].strip()))
        else:
            toks.append((f, kind, rest.strip()))
    return toks, pads


def first_mismatch(target, got, upto):
    """Index into target of the first token (frame <= upto) that `got` does not reproduce, or None.
    game and cd streams are compared separately (their relative order can jitter by a frame)."""
    worst = None
    for kind in ("game", "cd"):
        t = [(i, x) for i, x in enumerate(target) if x[1] == kind and x[0] <= upto]
        g = [x for x in got if x[1] == kind]
        for j, (i, x) in enumerate(t):
            if j >= len(g) or g[j][2] != x[2]:
                worst = i if worst is None else min(worst, i)
                break
    return worst


def write_replay(path, pads, bram_sha):
    lines = ["# snatcher-replay v1", "disc sha1=", f"bram sha1={bram_sha}", "engine fit_log_replay", "cdspeed 1.0",
             "justifier off"]
    last = "-"
    for f, b in sorted(pads):
        if b != last:
            lines.append(f"@0x{f:x} {b}")
            last = b
    Path(path).write_text("\n".join(lines) + "\n")


def run(headless, cue, pads, bram, bram_sha, frames, d):
    d = Path(d)
    if d.exists():
        shutil.rmtree(d)
    d.mkdir(parents=True)
    write_replay(d / "r.replay", pads, bram_sha)
    shutil.copy(bram, d / "bram.bin")
    env = dict(os.environ, SCD_LOG=str(d / "log.txt"))
    subprocess.run([headless, str(cue), "--frames", str(frames), "--out", str(d), "--replay", str(d / "r.replay"),
                    "--bram", str(d / "bram.bin")], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return parse_log(d / "log.txt")[0]


def presses(pads):
    """Group pad events into presses: (index of the press event, index of its release or None)."""
    out = []
    for i, (f, b) in enumerate(pads):
        if b != "-":
            rel = i + 1 if i + 1 < len(pads) else None
            out.append((i, rel))
    return out


def candidates(pads, lo, hi):
    """Edits of presses whose start lies in [lo, hi], smallest first."""
    cand = []
    ps = [(i, r) for i, r in presses(pads) if lo <= pads[i][0] <= hi]
    for d in (1, -1, 2, -2, 3, -3, 4, -4, 6, -6, 8, -8):
        for i, r in reversed(ps):     # the press nearest the mismatch first
            p = list(pads)
            f0 = p[i][0] + d
            prev = p[i - 1][0] if i > 0 else 0
            nxt = p[r + 1][0] if r is not None and r + 1 < len(p) else 1 << 30
            if f0 <= prev:
                continue
            p[i] = (f0, p[i][1])
            if r is not None:
                fr = p[r][0] + d
                if fr >= nxt:
                    continue
                p[r] = (fr, p[r][1])
            cand.append((f"shift press@{pads[i][0]} by {d:+d}", p))
    for i, r in reversed(ps):
        p = [x for k, x in enumerate(pads) if k != i and k != r]
        cand.append((f"drop press@{pads[i][0]} {pads[i][1]}", p))
    for i, r in reversed(ps):         # lengthen / shorten the hold
        for dh in (2, -2, 4):
            if r is None:
                continue
            p = list(pads)
            fr = p[r][0] + dh
            nxt = p[r + 1][0] if r + 1 < len(p) else 1 << 30
            if fr <= p[i][0] or fr >= nxt:
                continue
            p[r] = (fr, p[r][1])
            cand.append((f"hold press@{pads[i][0]} {dh:+d}", p))
    return cand


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--bram", required=True)
    ap.add_argument("--headless", required=True)
    ap.add_argument("--cue", default=str(DEFAULT_CUE))
    ap.add_argument("--out", default=str(ROOT / "work" / "fit"))
    ap.add_argument("--until", type=int, help="stop fitting after this app frame (default: the last logged token)")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 2) - 1))
    ap.add_argument("--window", type=int, default=400, help="frames before a mismatch whose presses may be edited")
    ap.add_argument("--lookahead", type=int, default=3, help="extra target tokens a fix must also match")
    ap.add_argument("--pair-presses", type=int, default=6, help="pair edits use the last N presses before a mismatch")
    ap.add_argument("--max-pairs", type=int, default=150)
    ap.add_argument("--max-cd-gap", type=int, default=3000, help="also require the next CD read if it is this close (frames)")
    a = ap.parse_args()

    target, pads = parse_log(a.log)
    until = a.until or target[-1][0]
    out = Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    bram_sha = hashlib.sha1(Path(a.bram).read_bytes()).hexdigest()
    edits = []
    floor = 0                         # presses before this frame are fixed
    while True:
        got = run(a.headless, a.cue, pads, a.bram, bram_sha, until + 120, out / "base")
        k = first_mismatch(target, got, until)
        if k is None:
            print(f"fitted: every logged state change and CD read up to f={until} reproduces")
            break
        tf = target[k][0]
        print(f"mismatch at app f={tf}: expected {target[k][1]} '{target[k][2]}'", flush=True)
        # a fix must reproduce the next few tokens and everything up to the next CD read: menu states (7/8/0/9)
        # repeat for every choice, a CD read is specific to the scene the choice leads to
        goal = min(k + a.lookahead, len(target) - 1)
        nxt_cd = next((i for i in range(k + 1, len(target)) if target[i][1] == "cd"), None)
        if nxt_cd is not None and target[nxt_cd][0] - tf <= a.max_cd_gap:
            goal = max(goal, nxt_cd)
        need = min(target[goal][0] + 120, until + 120)
        def attempt(cand, tag):
            """Run every candidate; the first in list order (the smallest edit) that reaches `goal` wins."""
            if not cand:
                return None
            with cf.ThreadPoolExecutor(a.jobs) as ex:
                futs = {ex.submit(run, a.headless, a.cue, p, a.bram, bram_sha, need, out / "trials" / f"{tf}_{tag}{n}"): n
                        for n, (_, p) in enumerate(cand)}
                res = {futs[fu]: first_mismatch(target, fu.result(), until) for fu in cf.as_completed(futs)}
            ok = [n for n in sorted(res) if res[n] is None or res[n] > goal]
            return cand[ok[0]] if ok else None

        fixed = None
        # widen step by step: single edits after the last fix, pairs of edits, then single edits reaching back past
        # the last fix (it may have matched the generic menu states by luck)
        for lo, pairs in ((max(floor, tf - a.window), False), (max(floor, tf - a.window), True), (tf - 2 * a.window, False)):
            if pairs:
                ps = [i for i, _ in presses(pads) if lo <= pads[i][0] <= tf][-a.pair_presses:]
                cand = []
                for desc1, p1 in candidates(pads, lo, tf):
                    if not any(desc1.startswith(x) for x in ("shift", "drop")):
                        continue
                    for desc2, p2 in candidates(p1, lo, tf):
                        if desc2 != desc1 and desc2 > desc1 and any(f"@{pads[i][0]}" in desc2 for i in ps) \
                                and any(f"@{pads[i][0]}" in desc1 for i in ps) and ("+2" in desc2 or "drop" in desc2 or "+4" in desc2):
                            cand.append((desc1 + " + " + desc2, p2))
                cand = cand[:a.max_pairs]
            else:
                cand = candidates(pads, lo, tf)
            print(f"  trying {len(cand)} {'pair' if pairs else 'single'} edits from f={lo}", flush=True)
            fixed = attempt(cand, "p" if pairs else "s")
            if fixed:
                break
        if not fixed:
            write_replay(out / "partial.replay", pads, bram_sha)
            print(f"no edit within {a.window} frames fixes the mismatch at f={tf}; wrote {out / 'partial.replay'}")
            return 1
        print(f"  fix: {fixed[0]}", flush=True)
        edits.append((tf, fixed[0]))
        pads = fixed[1]
        floor = max(0, tf - a.window)
    write_replay(out / "fitted.replay", pads, bram_sha)
    (out / "edits.txt").write_text("".join(f"f={tf}: {d}\n" for tf, d in edits))
    print(f"wrote {out / 'fitted.replay'} ({len(edits)} edits, listed in edits.txt)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
