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


def first_diff(a: list, b: list) -> Optional[dict]:
    """First differing checkpoint, or None. `fb` in fields = visible glitch; ram/vram only = internal divergence."""
    for x, y in zip(a, b):
        if x.frame != y.frame:
            return {"frame": min(x.frame, y.frame), "scene": x.scene, "fields": ["frame"]}
        fields = [f for f in ("vram", "ram", "fb") if getattr(x, f) != getattr(y, f)]
        if fields or x.scene != y.scene:
            return {"frame": x.frame, "scene": x.scene, "fields": fields or ["scene"]}
    if len(a) != len(b):
        longer = a if len(a) > len(b) else b
        return {"ended_early": "B" if len(a) > len(b) else "A", "frame": longer[min(len(a), len(b))].frame}
    return None


if __name__ == "__main__":
    d = first_diff(parse(open(sys.argv[1]).read()), parse(open(sys.argv[2]).read()))
    print(d or "identical")
    sys.exit(1 if d else 0)
