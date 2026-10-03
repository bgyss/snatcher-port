#!/usr/bin/env python3
"""Locate every compressed blob the game decompressed during a run, and verify the Python decompressor against it.

Input: an SCD_PROBE log of the Sub CPU LZ routine (entry $F346/$F348, exit $F3AE), e.g.
    SCD_NO_TRANSLATE=1 SCD_PROBE=S:f346,S:f348,S:f3ae scd_headless <cue> ... 2> probe.txt
Each entry line carries a5 (source), a6 (destination) and 16 bytes at a5; the exit line carries the final a5/a6.
The 16-byte signature is searched in the extracted disc files. The blob is then decompressed offline from that file
offset, and the consumed and produced lengths must equal what the game's own routine did.

Usage: lz_locate.py probe.txt <dir with disc files> [--json out.json]
"""
import argparse
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(__file__))
from konami_lz import decompress  # noqa: E402

LINE = re.compile(r"PROBE (\d+) S (\w+) .* a5=([0-9a-f]+) a6=([0-9a-f]+) a7=[0-9a-f]+ @a5=([0-9a-f]+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("probe")
    ap.add_argument("files")
    ap.add_argument("--json")
    args = ap.parse_args()

    blobs = {}
    for name in sorted(os.listdir(args.files)):
        p = os.path.join(args.files, name)
        if os.path.isfile(p) and os.path.getsize(p) < 16 << 20:
            blobs[name] = open(p, "rb").read()

    calls, cur = [], None
    for line in open(args.probe):
        m = LINE.match(line)
        if not m:
            continue
        frame, pc, a5, a6, sig = int(m[1]), int(m[2], 16), int(m[3], 16), int(m[4], 16), bytes.fromhex(m[5])
        if pc in (0xF346, 0xF348):
            if cur and cur["pc"] == 0xF346 and pc == 0xF348:
                continue   # $F346 falls into $F348 after skipping its 2-byte header
            cur = {"frame": frame, "pc": pc, "src": a5 + (2 if pc == 0xF346 else 0), "dst": a6,
                   "sig": sig[2:] if pc == 0xF346 else sig}
        elif pc == 0xF3AE and cur:
            cur["src_len"] = a5 - cur["src"]
            cur["dst_len"] = a6 - cur["dst"]
            calls.append(cur)
            cur = None

    results, seen = [], {}
    ok = bad = unknown = 0
    for c in calls:
        key = (c["sig"], c["src_len"], c["dst_len"])
        if key in seen:
            results.append(dict(c, **seen[key], repeat=True))
            continue
        hits = [(n, i) for n, d in blobs.items() for i in find_all(d, c["sig"])]
        found = None
        for n, i in hits:
            try:
                out, end = decompress(blobs[n], i)
            except (ValueError, IndexError):
                continue
            if end - i == c["src_len"] and len(out) == c["dst_len"]:
                found = {"file": n, "offset": i, "verified": True}
                break
        if found is None:
            found = {"file": hits[0][0] if hits else None, "offset": hits[0][1] if hits else None, "verified": False}
        seen[key] = found
        results.append(dict(c, **found, repeat=False))
        if found["verified"]:
            ok += 1
        elif found["file"]:
            bad += 1
        else:
            unknown += 1

    print(f"{len(calls)} decompress calls, {len(seen)} distinct blobs: {ok} verified, {bad} found but not matching, {unknown} not found on disc")
    by_file = {}
    for r in results:
        if not r["repeat"] and r["verified"]:
            by_file.setdefault(r["file"], []).append(r)
    for f, rs in sorted(by_file.items()):
        print(f"  {f}: {len(rs)} blobs, {sum(r['dst_len'] for r in rs)} bytes out")
    if args.json:
        with open(args.json, "w") as o:
            json.dump([{k: (v.hex() if isinstance(v, bytes) else v) for k, v in r.items()} for r in results], o, indent=1)


def find_all(data, sig):
    i = data.find(sig)
    while i >= 0:
        yield i
        i = data.find(sig, i + 1)


if __name__ == "__main__":
    main()
