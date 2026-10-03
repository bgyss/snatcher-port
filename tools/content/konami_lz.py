"""Konami LZ decompressor used by Snatcher's Main CPU (IP routine at $FF1996, called from the DMA queue at $FF1902).

Stream format (a5 = source, a6 = destination), transcribed from the 68000 routine:
  - A flag byte supplies 8 flags, least-significant bit first.
  - flag 0: copy one literal byte.
  - flag 1: read byte b:
      b == 0x1F            end of stream
      b <  0x80            back-reference: offset = ((b >> 5) & 3) << 8 | next byte, length = (b & 0x1F) + 3
      0x80 <= b < 0xC0     short back-reference: offset = b & 0x0F, length = (b >> 4) - 6
      b >= 0xC0            raw run: copy b - 0xB8 literal bytes
Back-references copy byte by byte, so overlapping copies repeat data as on the original hardware.
"""


def decompress(src: bytes, pos: int = 0, limit: int = 1 << 20):
    """Returns (data, end_pos). Raises ValueError on a malformed stream or past `limit` output bytes."""
    out = bytearray()
    flags = 0
    nflags = 0
    while True:
        if nflags == 0:
            flags = src[pos]
            pos += 1
            nflags = 8
        bit = flags & 1
        flags >>= 1
        nflags -= 1
        if not bit:
            out.append(src[pos])
            pos += 1
        else:
            b = src[pos]
            pos += 1
            if b < 0x80:
                if b == 0x1F:
                    return bytes(out), pos
                off = ((b >> 5) & 3) << 8 | src[pos]
                pos += 1
                n = (b & 0x1F) + 3
            elif b < 0xC0:
                off = b & 0x0F
                n = (b >> 4) - 6
            else:
                n = b - 0xB8
                out += src[pos:pos + n]
                pos += n
                if len(out) > limit:
                    raise ValueError("output limit")
                continue
            if off == 0 or off > len(out):
                raise ValueError(f"bad back-reference offset {off} at output {len(out)}")
            for _ in range(n):
                out.append(out[-off])
        if len(out) > limit:
            raise ValueError("output limit")
