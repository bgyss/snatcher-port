import json, os, re, shutil, subprocess, sys, tempfile, unittest, pathlib

ROOT = pathlib.Path(__file__).resolve().parents[2]
CUE = ROOT / "sega-cd-eng" / "Snatcher (Sega CD) (U)-redump.cue"
HEADLESS = os.environ.get("SCD_HEADLESS", str(ROOT / "engine" / "build-pt" / "scd_headless"))
needs_disc = unittest.skipUnless(CUE.exists(), "disc not present")
needs_headless = unittest.skipUnless(pathlib.Path(HEADLESS).exists(), "scd_headless not built")
sys.path.insert(0, str(pathlib.Path(__file__).parent))


def run_headless(*args, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([HEADLESS, str(CUE), *args], capture_output=True, text=True, env=e)


def new_dir(prefix):
    (ROOT / "work").mkdir(exist_ok=True)
    return pathlib.Path(tempfile.mkdtemp(prefix=prefix, dir=ROOT / "work"))


# Synthetic play for session tests: two pad presses and Justifier moves / trigger and start pulls (frames 200..656).
SYNTH = ("--press", "300:S:6", "--press", "500:C:6", "--justifier",
         "--gun", "200:100,80,in,T", "--gun", "206:100,80,in,-", "--gun", "650:20,30,out,S", "--gun", "656:20,30,out,-")


def record_session(frames, *extra, env=None, sdir=None):
    """Record a session with scd_headless --session-out. Returns (CompletedProcess, session dir)."""
    base = new_dir("pt_session_")
    sdir = sdir or base / "s"
    out = base / "out"
    out.mkdir()
    return run_headless("--frames", str(frames), "--out", str(out), "--session-out", str(sdir), *extra, env=env), sdir


def make_bram(path):
    """A formatted backup RAM file: the core formats a missing one and writes it at exit."""
    r = run_headless("--frames", "5", "--out", str(path.parent), "--bram", str(path))
    assert r.returncode == 0, r.stderr
    return path


def ckpt_frame(line):
    return int(line.split()[0][3:], 16)


def run_session_check(sdir, *extra, env=None):
    e = dict(os.environ, **(env or {}))
    return subprocess.run([sys.executable, str(pathlib.Path(__file__).parent / "run.py"), "--session", str(sdir), "--headless", HEADLESS,
                           "--work", str(sdir.parent / "verify"), *extra], capture_output=True, text=True, env=e)


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


class TestGun(unittest.TestCase):
    @needs_headless
    def test_bad_gun_option_rejected(self):
        r = run_headless("--frames", "5", "--justifier", "--gun", "10:1,2,sideways,T")
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("bad --gun", r.stderr)

    @needs_disc
    def test_v2_justifier_replay_accepted(self):
        out = ROOT / "work" / "pt_gun"
        out.mkdir(parents=True, exist_ok=True)
        rp = out / "v2.replay"
        rp.write_text("# snatcher-replay v2\ndisc sha1=\nbram sha1=none\njustifier on\ng0x10 100,80,in,T\n")
        r = run_headless("--frames", "30", "--out", str(out), "--replay", str(rp))
        self.assertEqual(r.returncode, 0, r.stderr)

    @needs_disc
    def test_gun_replay_matches_live_gun(self):
        # --gun sets the gun before run_frame (as the app does); the replay applies it at frame start: same frames, same state.
        out = ROOT / "work" / "pt_gun"
        out.mkdir(parents=True, exist_ok=True)
        a, b, rp = out / "live.ckpt", out / "replayed.ckpt", out / "gun.replay"
        r = run_headless("--frames", "1200", "--out", str(out), "--ckpt-out", str(a), "--justifier",
                         "--gun", "300:100,80,in,T", "--gun", "306:100,80,in,-", "--gun", "700:20,30,out,S", "--gun", "706:20,30,out,-")
        self.assertEqual(r.returncode, 0, r.stderr)
        rp.write_text("# snatcher-replay v2\ndisc sha1=\nbram sha1=none\njustifier on\n"
                      "g0x12c 100,80,in,T\ng0x132 100,80,in,-\ng0x2bc 20,30,out,S\ng0x2c2 20,30,out,-\n")
        r = run_headless("--frames", "1200", "--out", str(out), "--ckpt-out", str(b), "--replay", str(rp))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(a.read_text(), b.read_text())

    @needs_disc
    def test_watch_trace_records_gun(self):
        out = ROOT / "work" / "pt_gun_hang"
        out.mkdir(parents=True, exist_ok=True)
        for f in out.glob("*"):
            f.unlink()
        r = run_headless("--frames", "100", "--out", str(out), "--watch", "--hang-frames", "30", "--justifier",
                         "--gun", "10:100,80,in,T", env={"SCD_TEST_STICK_E020": "20"})
        self.assertEqual(r.returncode, 3, r.stderr)
        text = (out / "repro.replay").read_text()
        self.assertTrue(text.startswith("# snatcher-replay v2\n"), text)
        self.assertIn("justifier on\n", text)
        self.assertIn("g0xa 100,80,in,T\n", text)


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
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay", "state.txt"):
            self.assertTrue((out / name).exists(), name)
        self.assertTrue((out / "state.txt").read_text().startswith("Main PC="))
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


class TestManifest(unittest.TestCase):
    @needs_disc
    def test_manifest_identifies_build(self):
        out = new_dir("pt_manifest_")
        mf = out / "manifest.json"
        r = run_headless("--frames", "60", "--out", str(out), "--manifest-out", str(mf), env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        m = json.loads(mf.read_text())
        self.assertEqual(m["format"], 1)
        self.assertEqual(m["app"], "scd_headless")
        self.assertEqual(m["build"], "interpreter")   # SCD_NO_TRANSLATE: interpreter on either build kind
        self.assertEqual(m["frames"], 60)
        self.assertEqual(m["exit"], "quit")
        self.assertIsNone(m["sdl"])
        self.assertEqual(m["bram_sha1"], "none")
        self.assertIsInstance(m["git_dirty"], bool)
        head = subprocess.run(["git", "-C", str(ROOT), "rev-parse", "--short", "HEAD"], capture_output=True, text=True)
        if head.returncode == 0:   # built from this checkout (and not committed since the build)
            self.assertEqual(m["git_commit"], head.stdout.strip())


class TestSession(unittest.TestCase):
    @needs_headless
    def test_session_out_rejects_watch(self):
        r = run_headless("--frames", "5", "--session-out", str(new_dir("pt_session_bad_") / "s"), "--watch")
        self.assertEqual(r.returncode, 2, r.stderr)

    @needs_disc
    def test_session_folder_layout(self):
        r, sdir = record_session(1200, *SYNTH, "--marker-at", "400", "--marker-at", "900")
        self.assertEqual(r.returncode, 0, r.stderr)
        for name in ("manifest.json", "session.replay", "session.ckpt", "session.log"):
            self.assertTrue((sdir / name).exists(), name)
        self.assertFalse((sdir / "session.replay.bram").exists())   # no --bram: bram sha1=none
        m = json.loads((sdir / "manifest.json").read_text())
        self.assertEqual((m["app"], m["frames"], m["exit"], m["markers"], m["failures"]), ("scd_headless", 1200, "quit", 2, 0))
        self.assertTrue(m["justifier"])
        self.assertEqual(m["bram_sha1"], "none")
        self.assertIsNotNone(m["ended"])
        self.assertEqual(sorted(p.name for p in (sdir / "markers").iterdir()), ["001-f400", "002-f900"])
        mk = sdir / "markers" / "001-f400"
        for name in ("marker.json", "screen.ppm", "mainram.bin", "subram.bin", "state.txt"):
            self.assertTrue((mk / name).exists(), name)
        self.assertEqual(json.loads((mk / "marker.json").read_text())["frame"], 400)
        self.assertEqual((mk / "mainram.bin").stat().st_size, 0x10000)
        self.assertEqual((mk / "subram.bin").stat().st_size, 0x6000)
        text = (sdir / "session.replay").read_text()
        self.assertTrue(text.startswith("# snatcher-replay v2\n"))
        self.assertIn("justifier on\n", text)
        for line in ("@0x12c S\n", "@0x132 -\n", "g0xc8 100,80,in,T\n", "g0x28a 20,30,out,S\n"):
            self.assertIn(line, text)
        self.assertIn("[user] marker 001 at frame 400", (sdir / "session.log").read_text())

    @needs_disc
    def test_session_replays_identically(self):
        # The session opens the log (heartbeat, cpu_pcs) and writes a marker (state_text); the replay does neither.
        r, sdir = record_session(1500, *SYNTH, "--marker-at", "700")
        self.assertEqual(r.returncode, 0, r.stderr)
        out = sdir.parent / "replay"
        out.mkdir()
        ck = out / "r.ckpt"
        rr = run_headless("--frames", "1500", "--out", str(out), "--replay", str(sdir / "session.replay"), "--ckpt-out", str(ck))
        self.assertEqual(rr.returncode, 0, rr.stderr)
        self.assertEqual((sdir / "session.ckpt").read_text(), ck.read_text())

    @needs_disc
    def test_session_copies_starting_bram(self):
        import hashlib
        base = new_dir("pt_session_bram_")
        bram = make_bram(base / "bram.bin")
        before = hashlib.sha1(bram.read_bytes()).hexdigest()
        r, sdir = record_session(600, "--bram", str(bram))
        self.assertEqual(r.returncode, 0, r.stderr)
        copy = sdir / "session.replay.bram"
        self.assertEqual(hashlib.sha1(copy.read_bytes()).hexdigest(), before)
        self.assertIn(f"bram sha1={before}\n", (sdir / "session.replay").read_text())
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["bram_sha1"], before)

    @needs_disc
    def test_session_hang_writes_one_bundle(self):
        r, sdir = record_session(200, "--hang-frames", "30", env={"SCD_TEST_STICK_E020": "20"})
        self.assertEqual(r.returncode, 0, r.stderr)   # play continues after a hang
        bundles = list((sdir / "failure").iterdir())
        self.assertEqual(len(bundles), 1, bundles)
        self.assertRegex(bundles[0].name, r"^001-hang-f\d+$")
        for name in ("report.json", "final.ppm", "mainram.bin", "subram.bin", "repro.replay", "state.txt"):
            self.assertTrue((bundles[0] / name).exists(), name)
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["failures"], 1)

    @needs_disc
    def test_unwritable_file_is_dropped(self):
        sdir = new_dir("pt_session_ro_") / "s"
        (sdir / "session.ckpt").mkdir(parents=True)   # a directory where the checkpoint file goes: fopen fails
        r, _ = record_session(120, sdir=sdir)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("session.ckpt", r.stderr)
        self.assertTrue((sdir / "session.replay").read_text().startswith("# snatcher-replay v2\n"))
        self.assertEqual(json.loads((sdir / "manifest.json").read_text())["frames"], 120)

    @needs_disc
    def test_session_survives_kill(self):
        r, sdir = record_session(2000, *SYNTH, env={"SCD_TEST_KILL_AT": "1000"})
        self.assertEqual(r.returncode, 70, r.stderr)
        m = json.loads((sdir / "manifest.json").read_text())
        self.assertIsNone(m["ended"])
        self.assertIsNone(m["frames"])
        self.assertIsNone(m["exit"])
        text = (sdir / "session.ckpt").read_text()
        last = ckpt_frame(text.splitlines()[-1])
        self.assertTrue(900 <= last <= 960, last)   # flushed every 60 emulated frames: the last flush was after frame 960
        out = sdir.parent / "replay"
        out.mkdir()
        ck = out / "r.ckpt"
        rr = run_headless("--frames", str(last), "--out", str(out), "--replay", str(sdir / "session.replay"), "--ckpt-out", str(ck))
        self.assertEqual(rr.returncode, 0, rr.stderr)
        self.assertEqual(text, ck.read_text())


A = "@0x64 scene=E022:0003/E06C:0000 vram=1 ram=2 fb=3\n@0xc8 scene=E022:0003/E06C:0001 vram=4 ram=5 fb=6\n"


class TestSessionVerify(unittest.TestCase):
    def test_missing_manifest_is_an_error(self):
        d = new_dir("pt_nosession_")
        r = run_session_check(d)
        self.assertEqual(r.returncode, 2, r.stdout + r.stderr)
        self.assertIn("manifest.json", r.stdout)

    @needs_disc
    def test_recorded_session_reproduces(self):
        bram = make_bram(new_dir("pt_verify_bram_") / "bram.bin")
        r, sdir = record_session(1500, *SYNTH, "--marker-at", "700", "--bram", str(bram))
        self.assertEqual(r.returncode, 0, r.stderr)
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("session reproduces", v.stdout)

    @needs_disc
    def test_visible_divergence_reports_frame(self):
        r, sdir = record_session(1500, *SYNTH)
        self.assertEqual(r.returncode, 0, r.stderr)
        ck = sdir / "session.ckpt"
        lines = ck.read_text().splitlines()
        frame = ckpt_frame(lines[3])
        lines[3] = re.sub(r"fb=[0-9a-f]+", "fb=" + "0" * 16, lines[3])
        ck.write_text("\n".join(lines) + "\n")
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 1, v.stdout + v.stderr)
        self.assertIn("DIVERGED", v.stdout)
        self.assertIn(f"'frame': {frame}", v.stdout)

    @needs_disc
    def test_ram_only_difference_needs_strict(self):
        r, sdir = record_session(900)
        self.assertEqual(r.returncode, 0, r.stderr)
        ck = sdir / "session.ckpt"
        lines = ck.read_text().splitlines()
        lines[2] = re.sub(r" ram=[0-9a-f]+", " ram=" + "0" * 16, lines[2])   # leading space: not the ram= inside vram=
        ck.write_text("\n".join(lines) + "\n")
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("RAM-only", v.stdout)
        self.assertEqual(run_session_check(sdir, "--strict-ram").returncode, 1)

    @needs_disc
    def test_wrong_disc_is_identity_mismatch(self):
        r, sdir = record_session(300)
        self.assertEqual(r.returncode, 0, r.stderr)
        rp = sdir / "session.replay"
        rp.write_text(re.sub(r"^disc sha1=.*$", "disc sha1=0000", rp.read_text(), flags=re.M))
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 4, v.stdout + v.stderr)

    @needs_disc
    def test_build_kind_mismatch_warns(self):
        r, sdir = record_session(300, env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(r.returncode, 0, r.stderr)
        mf = sdir / "manifest.json"
        m = json.loads(mf.read_text())
        m["build"] = "translated"
        mf.write_text(json.dumps(m))
        v = run_session_check(sdir, env={"SCD_NO_TRANSLATE": "1"})
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("warning", v.stdout)

    @needs_disc
    def test_killed_session_verifies_to_last_checkpoint(self):
        r, sdir = record_session(2000, *SYNTH, env={"SCD_TEST_KILL_AT": "1000"})
        self.assertEqual(r.returncode, 70, r.stderr)
        v = run_session_check(sdir)
        self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
        self.assertIn("did not end cleanly", v.stdout)


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
