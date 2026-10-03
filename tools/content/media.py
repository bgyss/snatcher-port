"""Game-agnostic media helpers: cue/bin discs (MODE1/2352 + CD-DA), ISO9660, and PNG/WAV writers with no dependencies."""
import hashlib
import os
import struct
import wave
import zlib

RAW = 2352
DATA = 2048


class Disc:
    """A single-FILE cue/bin image. tracks: list of dicts {number, type, start (LBA of INDEX 01), end}."""

    def __init__(self, cue_path):
        self.cue = cue_path
        self.tracks = []
        bin_name = None
        cur = None
        for line in open(cue_path, encoding="latin-1"):
            w = line.split()
            if not w:
                continue
            if w[0] == "FILE":
                bin_name = line.split('"')[1]
            elif w[0] == "TRACK":
                cur = {"number": int(w[1]), "type": w[2]}
                self.tracks.append(cur)
            elif w[0] == "INDEX" and w[1] == "01" and cur is not None:
                m, s, f = (int(x) for x in w[2].split(":"))
                cur["start"] = (m * 60 + s) * 75 + f
        self.bin = os.path.join(os.path.dirname(cue_path), bin_name)
        self.f = open(self.bin, "rb")
        total = os.path.getsize(self.bin) // RAW
        for i, t in enumerate(self.tracks):
            t["end"] = self.tracks[i + 1]["start"] if i + 1 < len(self.tracks) else total

    def raw(self, lba, count=1):
        self.f.seek(lba * RAW)
        return self.f.read(RAW * count)

    def data_sector(self, lba):
        return self.raw(lba)[16:16 + DATA]

    def read_data(self, lba, length):
        out = bytearray()
        for i in range((length + DATA - 1) // DATA):
            out += self.data_sector(lba + i)
        return bytes(out[:length])

    def iso_files(self):
        """Yields (path, lba, size) for every file in the ISO9660 filesystem of track 1."""
        pvd = self.data_sector(16)
        if pvd[1:6] != b"CD001":
            return
        root = pvd[156:190]
        yield from self._walk(struct.unpack("<I", root[2:6])[0], struct.unpack("<I", root[10:14])[0], "")

    def _walk(self, lba, size, path):
        data = self.read_data(lba, size)
        pos = 0
        while pos < len(data):
            rlen = data[pos]
            if rlen == 0:
                pos = (pos // DATA + 1) * DATA
                continue
            rec = data[pos:pos + rlen]
            pos += rlen
            name = rec[33:33 + rec[32]]
            if name in (b"\x00", b"\x01"):
                continue
            name = name.decode("ascii", "replace").split(";")[0]
            ext, sz = struct.unpack("<I", rec[2:6])[0], struct.unpack("<I", rec[10:14])[0]
            if rec[25] & 2:
                yield from self._walk(ext, sz, f"{path}/{name}")
            else:
                yield f"{path}/{name}".lstrip("/"), ext, sz


def sha1(data):
    return hashlib.sha1(data).hexdigest()


def write_png(path, width, height, rows_rgb):
    """rows_rgb: iterable of `height` bytes objects, each width*3 bytes (RGB)."""
    raw = b"".join(b"\x00" + bytes(r) for r in rows_rgb)

    def chunk(tag, body):
        c = struct.pack(">I", len(body)) + tag + body
        return c + struct.pack(">I", zlib.crc32(tag + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def write_wav(path, pcm16, rate, channels):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm16)
