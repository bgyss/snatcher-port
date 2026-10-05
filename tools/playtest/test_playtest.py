import os, subprocess, unittest, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
HEADLESS = os.environ.get("SCD_HEADLESS", str(ROOT / "engine" / "build-pt" / "scd_headless"))
needs_disc = unittest.skipUnless(CUE.exists(), "disc not present")


def run_headless(*args, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([HEADLESS, str(CUE), *args], capture_output=True, text=True, env=e)


class TestCheckpoints(unittest.TestCase):
    @needs_disc
    def test_two_runs_identical(self):
        out = ROOT / "work" / "pt_test"
        out.mkdir(parents=True, exist_ok=True)
        a, b = out / "a.ckpt", out / "b.ckpt"
        for f in (a, b):
            r = run_headless("--frames", "400", "--out", str(out), "--ckpt-out", str(f), "--ckpt-every", "100")
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(a.read_text(), b.read_text())
        self.assertGreaterEqual(len(a.read_text().splitlines()), 4)


class TestReplayRefusal(unittest.TestCase):
    def _run(self, text, *extra):
        out = ROOT / "work" / "pt_refuse"
        out.mkdir(parents=True, exist_ok=True)
        rp = out / "r.replay"
        rp.write_text(text)
        return run_headless("--frames", "5", "--out", str(out), "--replay", str(rp), *extra)

    @needs_disc
    def test_wrong_disc_refused(self):
        r = self._run("# snatcher-replay v1\ndisc sha1=0000\nbram sha1=none\n")
        self.assertEqual(r.returncode, 4, r.stderr)
        self.assertIn("recorded for disc", r.stderr)

    @needs_disc
    def test_wrong_bram_refused(self):
        r = self._run("# snatcher-replay v1\ndisc sha1=\nbram sha1=1234\n")
        self.assertEqual(r.returncode, 4, r.stderr)
        self.assertIn("backup RAM", r.stderr)

    @needs_disc
    def test_malformed_replay_reports_line(self):
        r = self._run("# snatcher-replay v1\n@0x10 Z\n")
        self.assertEqual(r.returncode, 1, r.stderr)
        self.assertIn("line 2", r.stderr)


class TestWatch(unittest.TestCase):
    @needs_disc
    def test_hang_detected_writes_bundle(self):
        out = ROOT / "work" / "pt_hang"
        out.mkdir(parents=True, exist_ok=True)
        for f in out.glob("*"):
            f.unlink()
        # SCD_TEST_STICK_E020 pins the sampled frame counter from frame 20, a synthetic hang.
        r = run_headless("--frames", "100", "--out", str(out), "--watch", "--hang-frames", "30", env={"SCD_TEST_STICK_E020": "20"})
        self.assertEqual(r.returncode, 3, r.stderr)
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay"):
            self.assertTrue((out / name).exists(), name)
        import json
        rep = json.loads((out / "report.json").read_text())
        self.assertEqual(rep["kind"], "hang")
        self.assertIn("sub_pc", rep)

    @needs_disc
    def test_healthy_run_not_flagged(self):
        out = ROOT / "work" / "pt_ok"
        out.mkdir(parents=True, exist_ok=True)
        r = run_headless("--frames", "1200", "--out", str(out), "--watch")
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == "__main__":
    unittest.main()
