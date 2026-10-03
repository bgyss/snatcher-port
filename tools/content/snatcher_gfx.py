"""Snatcher DATA_*.BIN graphics: scene records -> VRAM image -> composed planes.

A scene record is `FFFF <u16 id?>` followed by entries `<u32 pointer in $28000 space> <u16 0> <u16 VRAM address>` and a
closing `FFFF`. Each pointer targets `[u16 size][Konami LZ]`. The decompressed blobs, placed at their VRAM addresses,
form the screen's VRAM: tiles from 0x0000, nametables (2,048 bytes, 32x32 cells) at 0xC000 (plane A) / 0xE000
(plane B). The engine copies this from Word RAM to VRAM.
"""
import struct

import konami_lz


def records(pack: bytes):
    """Yields (record offset, id word, [(vram address, blob offset, data)]) for every scene record in a pack."""
    i = 0
    n = len(pack)
    while i < n - 12:
        if pack[i:i + 2] == b"\xff\xff":
            ents, j = [], i + 4
            while j + 8 <= n:
                ptr, zero, vaddr = struct.unpack(">IHH", pack[j:j + 8])
                o = ptr - 0x28000
                if zero != 0 or not (0 <= o < n - 2):
                    break
                size = struct.unpack(">H", pack[o:o + 2])[0]
                try:
                    data, _ = konami_lz.decompress(pack, o + 2, limit=size)
                except (ValueError, IndexError):
                    break
                if len(data) != size or vaddr + size > 0x10000:
                    break
                ents.append((vaddr, o + 2, data))
                j += 8
            if ents and pack[j:j + 2] == b"\xff\xff":
                yield i, struct.unpack(">H", pack[i + 2:i + 4])[0], ents
                i = j
                continue
        i += 2


def commands(pack: bytes):
    """Scene command lists: yields (offset, type, words) for each `FFFF <type> ...` record, in file order. Types seen:
    0x01xx graphics load (see records()), 0x000A palette load `<u16 ?> <u16 count-1> <u32 pointer>`,
    0x02xx sprite objects `<u16 id/flags> <i16 x> <i16 y> <u32 sprite data> <u16 frame>` repeated."""
    i, n = 0, len(pack)
    while i < n - 4:
        if pack[i:i + 2] == b"\xff\xff" and pack[i + 2:i + 4] != b"\xff\xff":
            j = pack.find(b"\xff\xff", i + 4)
            while j > 0 and (j - i) % 2:
                j = pack.find(b"\xff\xff", j + 1)
            if j < 0:
                break
            body = pack[i + 4:j]
            yield i, struct.unpack(">H", pack[i + 2:i + 4])[0], struct.unpack(">%dH" % (len(body) // 2), body)
            i = j
        else:
            i += 2


def palette_at(pack, ptr, count):
    """`count` Mega Drive colours (0000BBB0GGG0RRR0) at a $28000-space pointer -> RGB tuples, padded to 64."""
    o = ptr - 0x28000
    if not (0 <= o and o + 2 * count <= len(pack)):
        return None   # points outside this pack (shared palette elsewhere in PRG RAM)
    cols = []
    for k in range(min(count, 64)):
        w = struct.unpack(">H", pack[o + 2 * k:o + 2 * k + 2])[0]
        r, g, b = (w >> 1) & 7, (w >> 5) & 7, (w >> 9) & 7
        cols.append((r * 36, g * 36, b * 36))
    return cols + [(0, 0, 0)] * (64 - len(cols))


def compose(vram, palette, plane_a=0xC000, plane_b=0xE000, has_a=True, has_b=True):
    """Plane B under plane A (colour 0 of a cell is transparent), 256x224 RGB rows. Priority bits are ignored."""
    a = render_plane(vram, plane_a, palette, raw=True) if has_a else None
    b = render_plane(vram, plane_b, palette, raw=True) if has_b else None
    rows = []
    for y in range(224):
        r = bytearray(256 * 3)
        for x in range(256):
            idx = 0
            for layer in (a, b):
                if layer and layer[y][x] & 15:
                    idx = layer[y][x]
                    break
            r[x * 3:x * 3 + 3] = bytes(palette[idx])
        rows.append(r)
    return rows


def vram_image(ents):
    vram = bytearray(0x10000)
    for vaddr, _, data in ents:
        vram[vaddr:vaddr + len(data)] = data
    return vram


def render_plane(vram, table, palette=None, w_cells=32, h_cells=28, raw=False):
    """RGB rows for a nametable at `table` (32 cells wide). palette: 64 RGB tuples (4 lines x 16); grayscale shades
    per palette line when None. Colour 0 is drawn as is (transparent on hardware). raw: rows of palette indices."""
    if raw:
        rows = [[0] * (w_cells * 8) for _ in range(h_cells * 8)]
    else:
        rows = [bytearray(w_cells * 8 * 3) for _ in range(h_cells * 8)]
    for cy in range(h_cells):
        for cx in range(w_cells):
            e = struct.unpack(">H", vram[table + (cy * 32 + cx) * 2: table + (cy * 32 + cx) * 2 + 2])[0]
            tile, hf, vf, line = e & 0x7FF, (e >> 11) & 1, (e >> 12) & 1, (e >> 13) & 3
            base = tile * 32
            for y in range(8):
                sy = 7 - y if vf else y
                rowb = vram[(base + sy * 4) & 0xFFFF:(base + sy * 4 + 4) & 0xFFFF or 0x10000]
                if len(rowb) < 4:
                    rowb = bytes(4)
                for x in range(8):
                    sx = 7 - x if hf else x
                    c = (rowb[sx >> 1] >> (4 if sx % 2 == 0 else 0)) & 15
                    if raw:
                        rows[cy * 8 + y][cx * 8 + x] = line * 16 + c
                        continue
                    rgb = palette[line * 16 + c] if palette else (c * 16, c * 16, min(255, c * 16 + line * 20))
                    p = (cx * 8 + x) * 3
                    rows[cy * 8 + y][p:p + 3] = bytes(rgb)
    return rows
