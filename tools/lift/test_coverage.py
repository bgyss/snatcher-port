"""End-to-end tests for the fallback census (ADR-003 M1). Skips without the disc or the builds."""
import os, pathlib, subprocess, unittest, zlib

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
    def test_no_interpreter_rows_and_irq_entries_present(self):
        f = OUT / "tr.tsv"
        run(TRANS, 400, SCD_COVERAGE=str(f))
        self.assertIn("translated=1", f.read_text().splitlines()[0])
        reasons = {x[2] for x in rows(f)}
        self.assertNotIn("interpreter", reasons)
        self.assertIn("irq_entry", reasons)

    def test_main_ram_code_is_translated(self):
        # The SP routine the Main CPU copies to $FFE100 runs through the mainram program (Task 6, ADR-003):
        # $FFE140 runs every frame from about frame 1020, and nothing on Main fails its per-range CRC check.
        f = OUT / "tr_main.tsv"
        run(TRANS, 1100, SCD_COVERAGE=str(f))
        main = [x for x in rows(f) if x[0] == "main"]
        window = {int(x[3], 16): x[2] for x in main if 0xFFE100 <= int(x[3], 16) < 0xFFE3D0}
        self.assertIn(0xFFE106, window, "the $FFE100 code did not run")   # not reached by the mainram program's recursive descent
        self.assertNotIn(0xFFE140, window)
        self.assertNotIn("code_mismatch", {x[2] for x in main})

    def test_lift_verify_disables_census(self):
        f = OUT / "verify.tsv"
        f.unlink(missing_ok=True)
        r = run(TRANS, 60, SCD_COVERAGE=str(f), SCD_LIFT_VERIFY="1")
        self.assertFalse(f.exists())
        self.assertIn("census disabled", r.stderr)


if __name__ == "__main__":
    unittest.main()
