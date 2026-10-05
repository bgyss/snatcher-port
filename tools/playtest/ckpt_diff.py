"""Compare two checkpoint files (scd_headless --ckpt-out format)."""
import re
import sys
from typing import NamedTuple, Optional

LINE = re.compile(r"^@0x([0-9a-f]+) scene=(\S+) vram=([0-9a-f]+) ram=([0-9a-f]+) fb=([0-9a-f]+)$")


class Ckpt(NamedTuple):
    frame: int
    scene: str
    vram: str
    ram: str
    fb: str


def parse(text: str) -> list:
    out = []
    for n, line in enumerate(text.splitlines(), 1):
        if not line.strip():
            continue
        m = LINE.match(line)
        if not m:
            raise ValueError(f"line {n}: not a checkpoint: {line!r}")
        out.append(Ckpt(int(m[1], 16), m[2], m[3], m[4], m[5]))
    return out


ALL_FIELDS = ("vram", "ram", "fb")
VISIBLE_FIELDS = ("vram", "fb")


def first_diff(a: list, b: list, fields=ALL_FIELDS) -> Optional[dict]:
    """First checkpoint differing in one of `fields`, or None. `fb` = visible glitch; ram only = internal divergence
    (often benign: stale stack bytes below SP differ between the translated and interpreted builds)."""
    for x, y in zip(a, b):
        if x.frame != y.frame:
            return {"frame": min(x.frame, y.frame), "scene": x.scene, "fields": ["frame"]}
        diff = [f for f in fields if getattr(x, f) != getattr(y, f)]
        if diff or x.scene != y.scene:
            return {"frame": x.frame, "scene": x.scene, "fields": diff or ["scene"]}
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        return {"ended_early": "B" if len(a) > len(b) else "A", "frame": longer[min(len(a), len(b))].frame}
    return None


if __name__ == "__main__":
    d = first_diff(parse(open(sys.argv[1]).read()), parse(open(sys.argv[2]).read()))
    print(d or "identical")
    sys.exit(1 if d else 0)
