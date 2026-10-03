"""Snatcher (Sega CD, US) scenario-script text: layout, control codes and a lossless markup.

SPxx.BIN layout (all 38 scripts with text): bytecode from 0, 0xFF padding, then a text table at 0x3800 of
0xFF-terminated strings in plain ASCII plus the control bytes below.
"""
TEXT_BASE = 0x3800

# Control bytes inside strings, as readable tags. Meanings marked "?" are inferred from context only.
CODES = {
    0xF2: "\n",          # line break inside a box
    0xF4: "{auto}",      # ? ends a line that advances without a button press (shouts, interruptions)
    0xF6: "{page}",      # ? continue in a new box
    0xF9: "{/}",         # end of highlight
    0xFA: "{hl:a}",      # highlight colour a (names of people and places, key terms)
    0xFB: "{hl:b}",      # highlight colour b (clues, numbers, instructions)
    0xFC: "{hl:c}",      # highlight colour c (narration / descriptions?)
    0xFE: "{hl:d}",      # highlight colour d (computer / Jordan output?)
}
TAGS = {v: k for k, v in CODES.items()}


def to_markup(raw: bytes) -> str:
    out = []
    for b in raw:
        if b in CODES:
            out.append(CODES[b])
        elif 0x20 <= b < 0x7F and b not in (0x7B, 0x7D):
            out.append(chr(b))
        else:
            out.append("{x%02X}" % b)   # glyphs (e.g. 0xEC/0xEE: controller buttons) and anything unexplained
    return "".join(out)


def from_markup(s: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(s):
        if s[i] == "\n":
            out.append(0xF2)
            i += 1
        elif s[i] == "{":
            j = s.index("}", i) + 1
            tag = s[i:j]
            out.append(int(tag[2:-1], 16) if tag.startswith("{x") else TAGS[tag])
            i = j
        else:
            out.append(ord(s[i]))
            i += 1
    return bytes(out)


def strings(script: bytes):
    """Yields (offset from TEXT_BASE, raw bytes) for every string in the text table, empty ones included."""
    if len(script) <= TEXT_BASE:
        return
    pos = TEXT_BASE
    while pos < len(script):
        end = script.find(b"\xff", pos)
        if end < 0:
            end = len(script)
        yield pos - TEXT_BASE, script[pos:end]
        pos = end + 1


def plain(s: str) -> str:
    """Markup with tags removed, for reading and searching."""
    import re
    return re.sub(r"\{[^}]*\}", lambda m: " " if m.group(0) == "{page}" else "", s)
