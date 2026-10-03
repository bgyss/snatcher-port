"""Extraction stages. Each stage takes a Context and returns a manifest dict:
    {"stage", "outputs": [...], "verified": bool, "checks": [str], "notes": [str], "review": [paths for a human]}
Outputs go under ctx.out/<stage>/. Stages are idempotent and depend only on earlier stages' outputs.
"""
import json
import os
import struct
import subprocess

import konami_lz
import media
import snatcher_text


class Context:
    def __init__(self, cue, out):
        self.disc = media.Disc(cue)
        self.out = out

    def dir(self, stage):
        d = os.path.join(self.out, stage)
        os.makedirs(d, exist_ok=True)
        return d

    def iso_file(self, name):
        return open(os.path.join(self.out, "iso", name), "rb").read()


def stage_iso(ctx):
    d = ctx.dir("iso")
    files = sorted(ctx.disc.iso_files(), key=lambda f: f[1])
    outs = []
    for name, lba, size in files:
        data = ctx.disc.read_data(lba, size)
        p = os.path.join(d, name)
        with open(p, "wb") as f:
            f.write(data)
        outs.append({"file": name, "lba": lba, "size": size, "sha1": media.sha1(data)})
    boot = ctx.disc.read_data(0, 0x8000)
    for name, lo, hi in (("_boot_ip.bin", 0x200, 0x6800), ("_boot_sp.bin", 0x6800, 0x8000)):
        with open(os.path.join(d, name), "wb") as f:
            f.write(boot[lo:hi])
    ok = len(outs) == 100 and all(o["size"] == len(open(os.path.join(d, o["file"]), "rb").read()) for o in outs)
    return {"stage": "iso", "outputs": outs, "verified": ok,
            "checks": [f"{len(outs)} files from the ISO9660 directory (expected 100), sizes match the directory"],
            "notes": ["_boot_ip.bin / _boot_sp.bin are the Main/Sub boot programs from sector 0 (not in the filesystem)."],
            "review": []}


def stage_cdda(ctx):
    d = ctx.dir("cdda")
    outs, checks = [], []
    tracks = ctx.disc.tracks
    for i, t in enumerate(tracks):
        if t["type"] != "AUDIO":
            continue
        end = t["end"]
        sectors = end - t["start"]
        pcm = ctx.disc.raw(t["start"], sectors)
        name = f"track{t['number']:02d}.flac"
        p = os.path.join(d, name)
        r = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-y", "-f", "s16le", "-ar", "44100", "-ac", "2",
                            "-i", "-", "-compression_level", "8", p], input=pcm)
        secs = sectors / 75
        outs.append({"file": name, "track": t["number"], "start_lba": t["start"], "sectors": sectors,
                     "seconds": round(secs, 2), "ok": r.returncode == 0})
    checks.append(f"{sum(o['ok'] for o in outs)}/{len(outs)} audio tracks encoded; lengths are sector counts from the cue sheet")
    lines = ["# CD-DA tracks", "", "| Track | Length | File |", "|---|---|---|"]
    lines += [f"| {o['track']} | {int(o['seconds'] // 60)}:{o['seconds'] % 60:05.2f} | [{o['file']}]({o['file']}) |" for o in outs]
    lines += ["", "Each file runs from INDEX 01 to the next track's INDEX 01, so it includes the next track's pregap."]
    open(os.path.join(d, "INDEX.md"), "w").write("\n".join(lines) + "\n")
    return {"stage": "cdda", "outputs": outs, "verified": all(o["ok"] for o in outs), "checks": checks,
            "notes": ["Which scene plays which track comes from the script VM stage (not yet decoded)."],
            "review": ["cdda/INDEX.md"]}


def stage_text(ctx):
    d = ctx.dir("text")
    outs, total, bad = [], 0, []
    for n in range(39):
        name = f"SP{n:02d}.BIN"
        data = ctx.iso_file(name)
        rows = []
        for off, raw in snatcher_text.strings(data):
            m = snatcher_text.to_markup(raw)
            if snatcher_text.from_markup(m) != raw:
                bad.append((name, off))
            rows.append({"index": len(rows), "offset": off, "text": m})
        total += len(rows)
        base = f"SP{n:02d}"
        json.dump({"script": name, "text_base": snatcher_text.TEXT_BASE, "bytecode_bytes": bytecode_len(data),
                   "strings": rows}, open(os.path.join(d, base + ".json"), "w"), indent=1)
        md = [f"# {name}", "", f"{len(rows)} strings, text table at 0x{snatcher_text.TEXT_BASE:X}. "
              "Order is file order, not play order (that needs the script VM). Tags: `{hl:x}`..`{/}` highlight, "
              "`{page}` new box, `{auto}` advances by itself, `{xNN}` glyph.", ""]
        for r in rows:
            if not r["text"]:
                continue
            body = r["text"].replace("\n", "  \n> ")
            md.append(f"**{r['index']}** `+0x{r['offset']:04X}`")
            md.append("> " + body)
            md.append("")
        open(os.path.join(d, base + ".md"), "w").write("\n".join(md) + "\n")
        outs.append({"script": name, "strings": len(rows), "nonempty": sum(1 for r in rows if r["text"])})
    lines = ["# Dialogue and text corpus", "", f"{total} strings in 39 scripts.", "", "| Script | Strings | Readable |", "|---|---|---|"]
    lines += [f"| {o['script']} | {o['nonempty']} | [SP{o['script'][2:4]}.md](SP{o['script'][2:4]}.md) |" for o in outs]
    open(os.path.join(d, "INDEX.md"), "w").write("\n".join(lines) + "\n")
    return {"stage": "text", "outputs": outs, "verified": not bad,
            "checks": [f"round trip: markup -> bytes identical for all {total} strings ({len(bad)} failures)"],
            "notes": ["File order, not play order: speakers, branching and menus come from the bytecode (see the script stage)."],
            "review": ["text/INDEX.md"]}


def bytecode_len(data):
    i = min(snatcher_text.TEXT_BASE, len(data))
    while i > 0 and data[i - 1] == 0xFF:
        i -= 1
    return i


def pack_blobs(data):
    """Every [u16 size][Konami LZ] blob referenced by a $28000-space pointer in a DATA pack, whose header matches."""
    found = {}
    for i in range(0, len(data) - 4, 2):
        o = struct.unpack(">I", data[i:i + 4])[0] - 0x28000
        if not (0 <= o < len(data) - 4) or o + 2 in found:
            continue
        size = struct.unpack(">H", data[o:o + 2])[0]
        if size < 32:
            continue
        try:
            out, end = konami_lz.decompress(data, o + 2, limit=size)
        except (ValueError, IndexError):
            continue
        if len(out) == size:
            found[o + 2] = (out, end - o - 2, i)
    return found


def tiles_png(path, data, palette=None, cols=16):
    """4bpp Mega Drive tiles (8x8, 32 bytes each, high nibble = left pixel) as a sheet. Grayscale unless palette given."""
    n = len(data) // 32
    rows_t = (n + cols - 1) // cols
    pal = palette or [(i * 17, i * 17, i * 17) for i in range(16)]
    w, h = cols * 8, rows_t * 8
    img = [bytearray(w * 3) for _ in range(h)]
    for t in range(n):
        tx, ty = (t % cols) * 8, (t // cols) * 8
        for y in range(8):
            row = data[t * 32 + y * 4: t * 32 + y * 4 + 4]
            for x in range(8):
                c = (row[x >> 1] >> (4 if x % 2 == 0 else 0)) & 15
                p = (tx + x) * 3
                img[ty + y][p:p + 3] = bytes(pal[c])
    media.write_png(path, w, h, img)


def stage_gfx_raw(ctx):
    d = ctx.dir("gfx-raw")
    outs, total = [], 0
    names = sorted(f for f in os.listdir(os.path.join(ctx.out, "iso")) if f.startswith("DATA_"))
    gallery = ["<!doctype html><meta charset=utf-8><title>Snatcher graphics blobs</title>",
               "<style>body{font:13px system-ui;background:#222;color:#ddd}img{image-rendering:pixelated;width:256px;"
               "background:#000;margin:2px}figure{display:inline-block;margin:4px}figcaption{font-size:11px}</style>",
               "<h1>Graphics blobs (4bpp tile sheets, grayscale)</h1><p>Palettes and tilemaps are applied in the gfx-scenes stage."
               " 2,048-byte blobs are usually tilemaps, not tiles, and look like noise here.</p>"]
    for name in names:
        data = ctx.iso_file(name)
        blobs = pack_blobs(data)
        pd = os.path.join(d, name[:-4])
        os.makedirs(pd, exist_ok=True)
        gallery.append(f"<h2>{name} ({len(blobs)} blobs)</h2>")
        for off, (out, clen, ref) in sorted(blobs.items()):
            base = f"{off:06x}_{len(out)}"
            open(os.path.join(pd, base + ".bin"), "wb").write(out)
            tiles_png(os.path.join(pd, base + ".png"), out)
            outs.append({"pack": name, "offset": off, "size": len(out), "compressed": clen, "pointer_at": ref})
            gallery.append(f"<figure><img src='{name[:-4]}/{base}.png' loading=lazy><figcaption>+0x{off:x} · {len(out)} B</figcaption></figure>")
        total += len(blobs)
    open(os.path.join(d, "gallery.html"), "w").write("\n".join(gallery))
    return {"stage": "gfx-raw", "outputs": outs, "verified": True,
            "checks": [f"{total} blobs in {len(names)} packs; every blob's u16 header equals its decompressed size",
                       "decoder verified against the game: 171/171 blobs decompressed during the intro are byte-identical "
                       "(tools/content/lz_locate.py on an SCD_PROBE log)"],
            "notes": ["Grayscale tile sheets: the palette and tilemap that compose each image are not yet decoded."],
            "review": ["gfx-raw/gallery.html"]}


def stage_gfx_scenes(ctx):
    import snatcher_gfx
    d = ctx.dir("gfx-scenes")
    outs = []
    names = sorted(f for f in os.listdir(os.path.join(ctx.out, "iso")) if f.startswith("DATA_"))
    gallery = ["<!doctype html><meta charset=utf-8><title>Snatcher scenes</title>",
               "<style>body{font:13px system-ui;background:#111;color:#ddd}img{image-rendering:pixelated;width:512px;margin:2px}"
               "figure{display:inline-block;margin:6px}figcaption{font-size:11px}</style>",
               "<h1>Composed scene backgrounds</h1><p>Each image is one graphics-load command: its blobs placed at their VRAM "
               "addresses, planes A and B drawn through their nametables with the palette command that follows. Sprites, "
               "scrolling and palette animation are not applied. ⚠ = no palette command found (grayscale).</p>"]
    for name in names:
        pack = ctx.iso_file(name)
        recs = {off: ents for off, _, ents in snatcher_gfx.records(pack)}
        pending, k = None, 0
        gallery.append(f"<h2>{name}</h2>")

        def emit(roff, ents, pal, pal_ptr):
            nonlocal k
            vram = snatcher_gfx.vram_image(ents)
            tabs = {v for v, _, dd in ents if len(dd) == 2048}
            if not tabs & {0xC000, 0xE000}:
                return
            rows = snatcher_gfx.compose(vram, pal or [(i * 4, i * 4, i * 4) for i in range(64)],
                                        has_a=0xC000 in tabs, has_b=0xE000 in tabs)
            fn = f"{name[:-4]}_{k:02d}_{roff:05x}.png"
            media.write_png(os.path.join(d, fn), 256, 224, rows)
            outs.append({"pack": name, "record": roff, "file": fn, "palette": pal_ptr,
                         "planes": sorted(hex(t) for t in tabs & {0xC000, 0xE000})})
            gallery.append(f"<figure><img src='{fn}' loading=lazy><figcaption>{'' if pal else '⚠ '}{name} record "
                           f"+0x{roff:x}</figcaption></figure>")
            k += 1

        for off, typ, w in snatcher_gfx.commands(pack):
            if off in recs:
                if pending:
                    emit(*pending, None, None)
                pending = (off, recs[off])
            elif typ == 0x000A and pending and len(w) >= 4:
                ptr = w[2] << 16 | w[3]
                emit(*pending, snatcher_gfx.palette_at(pack, ptr, 64), ptr)
                pending = None
        if pending:
            emit(*pending, None, None)
    open(os.path.join(d, "gallery.html"), "w").write("\n".join(gallery))
    with_pal = sum(1 for o in outs if o["palette"])
    return {"stage": "gfx-scenes", "outputs": outs, "verified": False,
            "checks": [f"{len(outs)} scene backgrounds composed, {with_pal} with a palette",
                       "spot check: the Neo Kobe City background (DATA_D0) matches the engine's frame 13800 in layout and "
                       "colour (scroll and sprites aside)"],
            "notes": ["Unverified as a whole: only one scene was compared against the engine. The palette command's "
                      "count field is not yet understood, so 64 colours are always loaded.",
                      "Sprites (0x02xx commands: character portraits, animations) are not composed yet."],
            "review": ["gfx-scenes/gallery.html"]}


def sm8_to_pcm16(data):
    """RF5C164 8-bit sign-magnitude (bit 7 set = positive, as in engine/src/scd/pcm.cpp) -> 16-bit little-endian.
    0xFF is the chip's loop/end marker and is written as silence."""
    out = bytearray(len(data) * 2)
    for i, s in enumerate(data):
        v = 0 if s == 0xFF else ((s & 0x7F) if s & 0x80 else -(s & 0x7F)) * 256
        struct.pack_into("<h", out, i * 2, v)
    return bytes(out)


STREAM_HZ = round(32552 * 0x600 / 0x800)   # FD = 0x0600, the only rate the game programs for streamed audio (SCD_TRACE_AUDIO)


def stage_pcm(ctx):
    d = ctx.dir("pcm")
    outs = []
    for bank in ("PCMLD_01.BIN", "PCMDRMDT.BIN"):
        data = ctx.iso_file(bank)
        for s in range(len(data) // 2048):
            h = data[s * 2048:s * 2048 + 16]
            if h[4:8] == b"\xff\xff\xff\xff" and h[10:16] == bytes(6):
                n, rate = struct.unpack(">I", h[:4])[0], struct.unpack(">H", h[8:10])[0]
                fn = f"{bank[:-4]}_clip_s{s:05d}_{rate}hz.wav"
                media.write_wav(os.path.join(d, fn), sm8_to_pcm16(data[s * 2048 + 16:s * 2048 + 16 + n]), rate, 1)
                outs.append({"bank": bank, "kind": "clip", "sector": s, "bytes": n, "rate": rate, "file": fn,
                             "seconds": round(n / rate, 2)})
    lt = ctx.iso_file("PCMLT_01.BIN")
    ld = ctx.iso_file("PCMLD_01.BIN")
    headered = {o["sector"] for o in outs if o["bank"] == "PCMLD_01.BIN"}
    seen = set()
    for i in range(8, len(lt) - 7, 8):
        kind, param, start, length = struct.unpack(">HHHH", lt[i:i + 8])
        if length == 0 or (start + length) * 2048 > len(ld) or start in headered or (start, length) in seen:
            continue
        if kind >> 8 not in (3, 4, 7, 8):
            continue
        seen.add((start, length))
        fn = f"PCMLD_01_stream_{len(seen) - 1:04d}_s{start:05d}.wav"
        media.write_wav(os.path.join(d, fn), sm8_to_pcm16(ld[start * 2048:(start + length) * 2048]), STREAM_HZ, 1)
        outs.append({"bank": "PCMLD_01.BIN", "kind": "stream", "index_record": (i - 8) // 8, "type": hex(kind),
                     "param": hex(param), "sector": start, "sectors": length, "rate": STREAM_HZ, "file": fn,
                     "seconds": round(length * 2048 / STREAM_HZ, 2)})
    covered = sum(o.get("sectors", 0) for o in outs if o["kind"] == "stream")
    lines = ["# PCM audio (voices and sound effects)", "",
             f"{sum(1 for o in outs if o['kind'] == 'clip')} headered clips (rate from their header), "
             f"{sum(1 for o in outs if o['kind'] == 'stream')} streamed records from PCMLT_01 at {STREAM_HZ} Hz "
             "(**unverified rate**). Which line of dialogue each voice belongs to comes from the script VM stage.", "",
             "| File | Kind | Seconds | Index type / param |", "|---|---|---|---|"]
    lines += [f"| [{o['file']}]({o['file']}) | {o['kind']} | {o['seconds']} | {o.get('type', '')} {o.get('param', '')} |" for o in outs]
    open(os.path.join(d, "INDEX.md"), "w").write("\n".join(lines) + "\n")
    total = sum(o["seconds"] for o in outs)
    return {"stage": "pcm", "outputs": outs, "verified": False,
            "checks": [f"{len(outs)} WAVs, {total / 60:.1f} minutes; index covers {covered} of {len(ld) // 2048} bank sectors",
                       "sample format (sign-magnitude) matches the engine's RF5C164; streamed rate from the only FD value "
                       "the game writes in the intro (0x0600)"],
            "notes": ["Listen-test needed: streamed voices are decoded at one fixed rate; some records may be stereo pairs "
                      "or use other rates later in the game. PCMLT_01 has a second table of 8-word records (SFX?) not yet decoded."],
            "review": ["pcm/INDEX.md"]}


def stage_script(ctx):
    """Decodes every scenario script's bytecode and writes a screenplay per script plus a speaker sheet."""
    import collections
    import snatcher_story
    import snatcher_vm
    d = ctx.dir("script")
    sub = ctx.iso_file("SUBCODE.BIN")
    speakers_file = os.path.join(d, "speakers.json")   # human-maintained: {"4": "Mika (receptionist)", ...}
    names = {int(k): v for k, v in json.load(open(speakers_file)).items()} if os.path.exists(speakers_file) else {}
    outs, failures, sample = [], [], collections.defaultdict(list)
    counts = collections.Counter()
    for n in range(39):
        name = f"SP{n:02d}.BIN"
        s = ctx.iso_file(name)
        texts = {o: snatcher_text.to_markup(r) for o, r in snatcher_text.strings(s)}
        dec = snatcher_vm.Decoder(sub, s, snatcher_text.TEXT_BASE)
        try:
            insns, end = dec.linear()
        except ValueError as e:
            failures.append(f"{name}: {e}")
            continue
        clean = all(b == 0xFF for b in s[end:min(len(s), snatcher_text.TEXT_BASE)])
        if not clean:
            failures.append(f"{name}: non-padding bytes after the decoded end 0x{end:x}")

        def collect(node):
            if node["op"] == 0x43 and node["args"] and isinstance(node["args"][0], dict) and node["args"][0]["op"] == 0x20:
                spk, off = node["args"][0]["args"]
                counts[spk] += 1
                if len(sample[spk]) < 6 and texts.get(off):
                    sample[spk].append(f"{name}: {snatcher_text.plain(texts[off]).replace(chr(10), ' ')[:100]}")
            for a in node["args"]:
                if isinstance(a, dict):
                    collect(a)

        for i in insns:
            collect(i)
        sp = snatcher_story.Screenplay(texts, names)
        lines = sp.script(insns)
        md = [f"# {name}: screenplay (decoded bytecode)", "",
              "Statements in address order, with `@addr` labels so gotos (→) can be followed. **Bold** = dialogue; ▸ = "
              "menu option. Raw `opXX(...)` lines are instructions not yet named (flags, graphics, sound, waits).", ""]
        open(os.path.join(d, f"SP{n:02d}.md"), "w").write("\n".join(md + lines) + "\n")
        json.dump(insns, open(os.path.join(d, f"SP{n:02d}.ast.json"), "w"))
        outs.append({"script": name, "statements": len(insns), "lines": sp.line_count, "decoded_to": end, "clean": clean})
    sheet = ["# Speakers", "", "Speaker ids from `op43(op20(speaker, text))`, with sample lines. Name them in "
             "`speakers.json` (`{\"3\": \"Gillian\"}`) and rerun the stage to put names in every screenplay.", "",
             "⚠ Not yet confirmed that the id is a fixed character: id 3 is consistently Gillian and id 2 looks like "
             "Metal Gear, but id 4 covers several people (the Junker receptionist, \"Gillian, it's a trap!\"), so some "
             "ids may be a portrait or text-window slot. Check samples across several scripts before naming one.", ""]
    for spk, c in counts.most_common():
        sheet.append(f"## {spk}: {names.get(spk, '?')} ({c} lines)")
        sheet += [f"- {x}" for x in sample[spk]] + [""]
    open(os.path.join(d, "SPEAKERS.md"), "w").write("\n".join(sheet) + "\n")
    total = sum(o["lines"] for o in outs)
    return {"stage": "script", "outputs": outs, "verified": not failures,
            "checks": [f"{len(outs)}/39 scripts decode exactly from 0 to the padding (instruction lengths from the "
                       "VM's own skip routine at $1699A); the opcodes the VM dispatched in a live SP06 run all fall on "
                       "decoded instruction boundaries",
                       f"{total} dialogue lines and every menu label resolve to a string start in their script"]
                      + failures,
            "notes": ["Speaker ids need names (SPEAKERS.md). Flags, conditions, scene/graphics/sound commands are still "
                      "raw opcodes; naming them is the next step (probe each handler in the engine)."],
            "review": ["script/SP06.md", "script/SPEAKERS.md"]}


STAGES = {"iso": stage_iso, "cdda": stage_cdda, "text": stage_text, "gfx-raw": stage_gfx_raw, "gfx-scenes": stage_gfx_scenes, "pcm": stage_pcm, "script": stage_script}
