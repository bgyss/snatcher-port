"""Convert scd_headless --press lists (FRAME:BUTTONS:HOLD) into a .replay."""
import sys


def convert(presses, disc_sha1, bram_sha1="none"):
    ev = {}
    for p in presses:
        f, b, h = p.split(":")
        f, h = int(f), int(h)
        ev[f] = b
        ev.setdefault(f + h, "-")   # a later press starting at f+h wins
    lines = ["# snatcher-replay v1", f"disc sha1={disc_sha1}", f"bram sha1={bram_sha1}", "engine press_to_replay",
             "cdspeed 1.0", "justifier off"]
    last = "-"
    for f in sorted(ev):
        if ev[f] != last:
            lines.append(f"@0x{f:x} {ev[f]}")
            last = ev[f]
    return "\n".join(lines) + "\n"


if __name__ == "__main__":
    print(convert(sys.argv[2:], sys.argv[1]), end="")
