"""Tests for gen_program.py seeds, region windows, mainram and overlay programs (synthetic code, no disc needed)."""
import pathlib, subprocess, sys, tempfile, unittest

GEN = pathlib.Path(__file__).with_name("gen_program.py")
MOVEQ1_RTS = bytes.fromhex("70014E75")
MOVEQ2_RTS = bytes.fromhex("70024E75")


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.code = pathlib.Path(self.tmp.name) / "code"
        self.out = pathlib.Path(self.tmp.name) / "out"
        self.code.mkdir()

    def tearDown(self):
        self.tmp.cleanup()

    def gen(self, prog):
        r = subprocess.run([sys.executable, str(GEN), str(self.out), str(self.code), prog], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        return r.stdout

    def seeds(self, prog, *pcs):
        d = self.code / "seeds"
        d.mkdir(exist_ok=True)
        (d / (prog + ".txt")).write_text("# test\n" + "".join("%06x\n" % pc for pc in pcs))


class TestMainRam(Base):
    def setUp(self):
        super().setUp()
        sp = bytearray(0x1800)
        sp[0x1300:0x1304] = MOVEQ1_RTS          # $FFE100: entry
        sp[0x1310:0x1314] = MOVEQ2_RTS          # $FFE110: reachable only through a seed
        (self.code / "scd_sub_sp.bin").write_bytes(bytes(sp))

    def test_window_is_based_at_ffe100(self):
        self.gen("mainram")
        cpp = (self.out / "mainram_translated.cpp").read_text()
        self.assertIn("bool mainram_has(uint32_t pc)", cpp)
        self.assertIn("case 0xFFE100:", cpp)
        self.assertNotIn("case 0xFFE110:", cpp)
        self.assertIn("kMainramRanges", cpp)

    def test_seed_adds_entry(self):
        self.seeds("mainram", 0xFFE110)
        self.gen("mainram")
        self.assertIn("case 0xFFE110:", (self.out / "mainram_translated.cpp").read_text())

    def test_out_of_range_seed_is_ignored(self):
        self.seeds("mainram", 0x500000, 0xFFE3D0)
        self.gen("mainram")
        cpp = (self.out / "mainram_translated.cpp").read_text()
        self.assertNotIn("case 0x500000:", cpp)
        self.assertNotIn("case 0xFFE3D0:", cpp)


class TestOverlays(Base):
    def write_overlay(self, name, body):
        d = self.code / "overlays"
        d.mkdir(exist_ok=True)
        img = bytearray(0x1000)
        img[0:len(body)] = body
        (d / (name + ".bin")).write_bytes(bytes(img))

    def test_overlay_program_from_seeds_only(self):
        self.write_overlay("ovl_deadbeef", MOVEQ1_RTS)
        self.seeds("ovl_deadbeef", 0x28000)
        self.gen("ovl:overlays/ovl_deadbeef.bin")
        cpp = (self.out / "ovl_deadbeef_translated.cpp").read_text()
        self.assertIn("bool ovl_deadbeef_has(uint32_t pc)", cpp)
        self.assertIn("case 0x28000:", cpp)
        self.assertIn("kOvl_deadbeefRanges", cpp)

    def test_same_pc_different_bytes_gets_different_range_crc(self):
        self.write_overlay("ovl_00000001", MOVEQ1_RTS)
        self.write_overlay("ovl_00000002", MOVEQ2_RTS)
        self.seeds("ovl_00000001", 0x28000)
        self.seeds("ovl_00000002", 0x28000)
        self.gen("ovl:overlays/ovl_00000001.bin")
        self.gen("ovl:overlays/ovl_00000002.bin")
        # the instruction-range line ({lo, hi, crc}); the span line for the whole file starts {0x28000, 0x29000, ...}
        crc = lambda n: [l for l in (self.out / (n + "_translated.cpp")).read_text().splitlines() if l.strip().startswith("{0x28000, 0x28004,")]
        self.assertEqual(len(crc("ovl_00000001")), 1)
        self.assertNotEqual(crc("ovl_00000001"), crc("ovl_00000002"))

    def test_index_lists_every_overlay(self):
        self.write_overlay("ovl_deadbeef", MOVEQ1_RTS)
        self.gen("ovl_index")
        cpp = (self.out / "overlays_index.cpp").read_text()
        self.assertIn("{ovl_deadbeef_has, ovl_deadbeef_run, kOvl_deadbeefRanges, &kOvl_deadbeefRangeCount, &kOvl_deadbeefCrc}", cpp)
        self.assertIn("kOverlayCount = 1;", cpp)

    def test_index_without_overlays(self):
        self.gen("ovl_index")
        self.assertIn("kOverlayCount = 0;", (self.out / "overlays_index.cpp").read_text())


if __name__ == "__main__":
    unittest.main()
