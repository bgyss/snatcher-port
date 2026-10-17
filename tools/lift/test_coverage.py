"""End-to-end tests for the fallback census (ADR-003 M1). Skips without the disc or the builds."""
import os, pathlib, subprocess, unittest, zlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import coverage_report as cr

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
INTERP = pathlib.Path(os.environ.get("SCD_HEADLESS_INTERP", ROOT / "engine" / "build-interp" / "scd_headless"))
TRANS = pathlib.Path(os.environ.get("SCD_HEADLESS_TR", ROOT / "engine" / "build" / "scd_headless"))
OUT = ROOT / "work" / "cov_test"


def run(binary, frames, **env):
    OUT.mkdir(parents=True, exist_ok=True)
    e = dict(os.environ)
    for k in ("SCD_NO_TRANSLATE", "SCD_LIFT_VERIFY", "SCD_COVERAGE", "SCD_DUMP_OVERLAYS"):
        e.pop(k, None)
    e.update(env)
    r = subprocess.run([str(binary), str(CUE), "--frames", str(frames), "--out", str(OUT)], capture_output=True, text=True, env=e)
    assert r.returncode == 0, r.stderr[-2000:]
    return r


def rows(path):
    return [l.split("\t") for l in path.read_text().splitlines() if l and not l.startswith("#")]


@unittest.skipUnless(CUE.exists() and INTERP.exists(), "disc or interpreter build missing")
class TestInterpreterCensus(unittest.TestCase):
    def test_every_step_is_interpreter(self):
        f = OUT / "interp.tsv"
        run(INTERP, 400, SCD_COVERAGE=str(f))
        self.assertTrue(f.read_text().startswith("# snatcher-coverage v1 frames=400 translated=0\n"))
        r = rows(f)
        self.assertEqual({x[2] for x in r}, {"interpreter"})
        self.assertEqual({x[0] for x in r}, {"main", "sub"})

    def test_overlay_dump_is_keyed_by_its_first_2k(self):
        d = OUT / "ovl"
        d.mkdir(parents=True, exist_ok=True)
        for p in d.glob("ovl_*.bin"):
            p.unlink()
        f = OUT / "ovl.tsv"
        run(INTERP, 1500, SCD_COVERAGE=str(f), SCD_DUMP_OVERLAYS=str(d))   # the intro overlay runs after the Konami logo
        dumps = sorted(d.glob("ovl_*.bin"))
        self.assertTrue(dumps, "no overlay dumped")
        for p in dumps:
            data = p.read_bytes()
            self.assertEqual(len(data), 0x58000)
            self.assertEqual(p.stem, "ovl_%08x" % zlib.crc32(data[:0x800]))
        images = {x[1] for x in rows(f) if x[0] == "sub" and int(x[3], 16) >= 0x28000}
        self.assertTrue(images, "no census rows tagged with an overlay image")
        self.assertLessEqual(images, {p.stem[4:] for p in dumps})
        self.assertNotIn("00000000", images)


@unittest.skipUnless(CUE.exists() and TRANS.exists(), "disc or translated build missing")
class TestTranslatedCensus(unittest.TestCase):
    def test_no_interpreter_or_irq_entry_rows(self):
        # ADR-003 M2: interrupts are taken in host code, so no Musashi step is an interrupt entry.
        f = OUT / "tr.tsv"
        run(TRANS, 400, SCD_COVERAGE=str(f))
        self.assertIn("translated=1", f.read_text().splitlines()[0])
        reasons = {x[2] for x in rows(f)}
        self.assertNotIn("interpreter", reasons)
        self.assertNotIn("irq_entry", reasons)

    def test_interrupts_enter_translated_handlers(self):
        # Main's H-INT handlers are vectored through the Gate Array ($70 reads $A12006) into the IP, its V-INT through the RAM
        # jump slot $FFFD06. After an entry in host code the handler runs as translated code, checked per range like any other:
        # the only Main code stepped in Musashi is the BIOS RAM jump slots at $FFFD00 and up.
        f = OUT / "tr_irq.tsv"
        run(TRANS, 1500, SCD_COVERAGE=str(f))
        main = [x for x in rows(f) if x[0] == "main"]
        self.assertNotIn("irq_entry", {x[2] for x in main})
        self.assertNotIn("code_mismatch", {x[2] for x in main})
        stepped = {int(x[3], 16) for x in main if x[2] == "not_translated"}
        self.assertTrue(all(pc >= 0xFFFD00 for pc in stepped), sorted(hex(pc) for pc in stepped))

    @unittest.skipUnless(INTERP.exists(), "interpreter build missing")
    def test_main_ram_code_is_translated(self):
        # The SP routine the Main CPU copies to $FFE100 runs through the mainram program (ADR-003):
        # $FFE106 and $FFE140 run every frame from about frame 1020, and nothing on Main fails its per-range CRC check.
        # Liveness comes from the interpreter build, where every step is a census row. On the translated build,
        # nothing in $FFE100..$FFE3D0 may run as not_translated once the mainram seed file (`mise run census`, which
        # seeds $FFE106) exists. Since M2 no irq_entry rows appear (interrupts are taken in host code).
        fi = OUT / "interp_main.tsv"
        run(INTERP, 1100, SCD_COVERAGE=str(fi))
        ran = {int(x[3], 16) for x in rows(fi) if x[0] == "main" and 0xFFE100 <= int(x[3], 16) < 0xFFE3D0}
        self.assertIn(0xFFE106, ran, "the $FFE100 code did not run")
        self.assertIn(0xFFE140, ran, "the $FFE100 code did not run")
        f = OUT / "tr_main.tsv"
        run(TRANS, 1100, SCD_COVERAGE=str(f))
        main = [x for x in rows(f) if x[0] == "main"]
        self.assertNotIn("code_mismatch", {x[2] for x in main})
        seeds = ROOT / "extracted" / "code" / "seeds" / "mainram.txt"
        if not seeds.exists():
            self.skipTest("extracted/code/seeds/mainram.txt missing: run `mise run census` and rebuild the translated build")
        window = {int(x[3], 16) for x in main if x[2] == "not_translated" and 0xFFE100 <= int(x[3], 16) < 0xFFE3D0}
        self.assertEqual(window, set(), "$FFE100 code ran in the interpreter (not_translated)")

    def test_only_hle_steps_remain(self):
        # ADR-003 M2 exit criterion on a short run: with host-built BIOS glue counted as HLE, every Musashi step is hle_stub.
        f = OUT / "tr_m2.tsv"
        run(TRANS, 1500, SCD_COVERAGE=str(f))
        self.assertEqual({k[2] for k in cr.merge([str(f)])}, {"hle_stub"})

    def test_lift_verify_disables_census(self):
        f = OUT / "verify.tsv"
        f.unlink(missing_ok=True)
        r = run(TRANS, 60, SCD_COVERAGE=str(f), SCD_LIFT_VERIFY="1")
        self.assertFalse(f.exists())
        self.assertIn("census disabled", r.stderr)


if __name__ == "__main__":
    unittest.main()
