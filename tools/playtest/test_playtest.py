import os, subprocess, sys, unittest, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
HEADLESS = os.environ.get("SCD_HEADLESS", str(ROOT / "engine" / "build-pt" / "scd_headless"))
needs_disc = unittest.skipUnless(CUE.exists(), "disc not present")
sys.path.insert(0, str(pathlib.Path(__file__).parent))


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

    @needs_disc
    def test_checkpoint_frames_strictly_increase(self):
        out = ROOT / "work" / "pt_test"
        out.mkdir(parents=True, exist_ok=True)
        f = out / "mono.ckpt"
        # 400 is a multiple of --ckpt-every, so the last frame is emitted by the backstop and again at exit
        r = run_headless("--frames", "400", "--out", str(out), "--ckpt-out", str(f), "--ckpt-every", "100")
        self.assertEqual(r.returncode, 0, r.stderr)
        frames = [int(l.split()[0][3:], 16) for l in f.read_text().splitlines()]
        self.assertEqual(frames, sorted(set(frames)))


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
    def test_justifier_replay_refused(self):
        r = self._run("# snatcher-replay v1\ndisc sha1=\nbram sha1=none\njustifier on\n")
        self.assertEqual(r.returncode, 4, r.stderr)
        self.assertIn("gun", r.stderr)

    @needs_disc
    def test_cdspeed_in_replay_is_applied(self):
        out = ROOT / "work" / "pt_refuse"
        out.mkdir(parents=True, exist_ok=True)
        ck = {}
        for speed in ("1.0", "4.0"):
            rp = out / f"s{speed}.replay"
            rp.write_text(f"# snatcher-replay v1\ndisc sha1=\nbram sha1=none\ncdspeed {speed}\n")
            ck[speed] = out / f"s{speed}.ckpt"
            r = run_headless("--frames", "1500", "--out", str(out), "--replay", str(rp), "--ckpt-out", str(ck[speed]))
            self.assertEqual(r.returncode, 0, r.stderr)
        self.assertNotEqual(ck["1.0"].read_text(), ck["4.0"].read_text())

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


A = "@0x64 scene=E022:0003/E06C:0000 vram=1 ram=2 fb=3\n@0xc8 scene=E022:0003/E06C:0001 vram=4 ram=5 fb=6\n"


class TestDiff(unittest.TestCase):
    def test_identical(self):
        import ckpt_diff
        self.assertIsNone(ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(A)))

    def test_fb_only(self):
        import ckpt_diff
        b = A.replace("fb=6", "fb=7")
        d = ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(b))
        self.assertEqual(d["frame"], 0xC8)
        self.assertEqual(d["fields"], ["fb"])

    def test_ended_early(self):
        import ckpt_diff
        short = A.splitlines()[0] + "\n"
        d = ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(short))
        self.assertEqual(d, {"ended_early": "B", "frame": 0xC8, "ended_at": 0x64})

    def test_visible_only_skips_ram_noise(self):
        import ckpt_diff
        b = A.replace("ram=2", "ram=9").replace("fb=6", "fb=7")   # ram differs at the first checkpoint, fb at the second
        d = ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(b), fields=("vram", "fb"))
        self.assertEqual(d["frame"], 0xC8)
        self.assertEqual(d["fields"], ["fb"])
        self.assertEqual(ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(b))["frame"], 0x64)

    def test_ram_only_is_not_visible(self):
        import ckpt_diff
        b = A.replace("ram=2", "ram=9")
        self.assertIsNone(ckpt_diff.first_diff(ckpt_diff.parse(A), ckpt_diff.parse(b), fields=("vram", "fb")))

    def test_ended_early_off_grid(self):
        import ckpt_diff
        long_run = A + "@0x12c scene=E022:0003/E06C:0001 vram=4 ram=5 fb=6\n"
        short = A + "@0x100 scene=E022:0003/E06C:0001 vram=4 ram=5 fb=6\n"   # final line at the frame the short run stopped on
        d = ckpt_diff.first_diff(ckpt_diff.parse(long_run), ckpt_diff.parse(short))
        self.assertEqual(d.get("ended_early"), "B")
        self.assertEqual(d.get("ended_at"), 0x100)

    def test_bad_line_reports_line_number(self):
        import ckpt_diff
        with self.assertRaisesRegex(ValueError, "line 2"):
            ckpt_diff.parse(A.splitlines()[0] + "\ngarbage\n")


class TestConvert(unittest.TestCase):
    def test_press_list(self):
        import press_to_replay
        text = press_to_replay.convert(["10:S:5", "30:UD:2"], "deadbeef")
        self.assertIn("disc sha1=deadbeef", text)
        self.assertIn("@0xa S", text)
        self.assertIn("@0xf -", text)
        self.assertIn("@0x1e UD", text)
        self.assertIn("@0x20 -", text)

    def test_back_to_back_presses(self):
        import press_to_replay
        text = press_to_replay.convert(["10:S:5", "15:C:3"], "")
        self.assertIn("@0xf C", text)      # the second press starts where the first releases: no release event between
        self.assertNotIn("@0xf -", text)


class TestRunner(unittest.TestCase):
    @needs_disc
    def test_replay_matches_press(self):
        import press_to_replay
        out = ROOT / "work" / "pt_press"
        out.mkdir(parents=True, exist_ok=True)
        rp = out / "p.replay"
        rp.write_text(press_to_replay.convert(["300:S:6", "500:C:6"], ""))
        a, b = out / "a.ckpt", out / "b.ckpt"
        run_headless("--frames", "800", "--out", str(out), "--ckpt-out", str(a), "--press", "300:S:6", "--press", "500:C:6")
        run_headless("--frames", "800", "--out", str(out), "--ckpt-out", str(b), "--replay", str(rp))
        self.assertEqual(a.read_text(), b.read_text())

    @needs_disc
    def test_runner_determinism_mode(self):
        out = ROOT / "work" / "pt_run"
        rp = ROOT / "work" / "pt_run.replay"
        import press_to_replay
        rp.write_text(press_to_replay.convert(["300:S:6"], ""))
        r = subprocess.run([sys.executable, str(pathlib.Path(__file__).parent / "run.py"), str(rp), "--headless", HEADLESS,
                            "--determinism", "--frames", "600", "--work", str(out)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertIn("builds match", r.stdout)


if __name__ == "__main__":
    unittest.main()
