"""Tests for gen_program.py seeds, region windows, mainram and overlay programs (synthetic code, no disc needed)."""
import pathlib, shutil, subprocess, sys, tempfile, unittest

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


HARNESS = r"""
#include <cstdio>
#include "cpu68k.h"
extern "C" { unsigned char m68ki_cycles[3][0x10000]; }
namespace lift { Exit PROG_run(Cpu& c, uint32_t pc, int32_t& budget); }
struct Mem : lift::Bus {
    uint8_t m[0x1000000] = {};
    uint8_t r8(uint32_t a) override { return m[a & 0xFFFFFF]; }
    uint16_t r16(uint32_t a) override { return uint16_t(m[a & 0xFFFFFF] << 8 | m[(a + 1) & 0xFFFFFF]); }
    void w8(uint32_t a, uint8_t v) override { m[a & 0xFFFFFF] = v; }
    void w16(uint32_t a, uint16_t v) override { w8(a, uint8_t(v >> 8)); w8(a + 1, uint8_t(v)); }
};
static Mem mem;
int main() {
    for (auto& r : m68ki_cycles) for (auto& v : r) v = 4;
    lift::Cpu c; c.bus = &mem; c.a[7] = 0x1000; mem.w32(0x1000, 0x123456);
    int32_t budget = 1000;
    lift::Exit ex = PROG_run(c, ENTRY, budget);
    std::printf("%d %06x %08x\n", int(ex.kind), unsigned(ex.pc), unsigned(c.d[0]));
}
"""


class TestRangeExit(Base):
    """A transfer from one translated code range to another must leave an overlay program's _run (NotTranslated) so the
    dispatcher re-checks the target range's bytes (pick_entry): otherwise a branch can run stale code over a different
    overlay that shares the entry range. Sub/Main programs (not overlay-mutable) keep running across ranges."""
    ROOT = pathlib.Path(__file__).resolve().parents[2]
    # $28000: moveq #1,d0 ; bra.s $28010   |   $28010: moveq #2,d0 ; rts   (two code ranges; the gap is illegal
    # opcodes so ovl909's linear scan does not decode it)
    IMG = bytes.fromhex("7001600C") + bytes.fromhex("4AFC") * 6 + bytes.fromhex("70024E75")

    def run_prog(self, cpp_name, prog, entry):
        exe = self.out / "h"
        src = self.out / "h.cpp"
        src.write_text(HARNESS.replace("PROG", prog).replace("ENTRY", hex(entry)))
        cxx = shutil.which("clang++") or "/usr/bin/clang++"
        r = subprocess.run([cxx, "-std=c++20", "-O0", "-I", str(self.ROOT / "engine/src/lift"), str(src), str(self.out / cpp_name), "-o", str(exe)],
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        kind, pc, d0 = subprocess.run([str(exe)], capture_output=True, text=True, check=True).stdout.split()
        return int(kind), int(pc, 16), int(d0, 16)

    def test_overlay_exits_when_leaving_range(self):
        d = self.code / "overlays"
        d.mkdir()
        (d / "ovl_0000abcd.bin").write_bytes(self.IMG + bytes(0x100))
        self.seeds("ovl_0000abcd", 0x28000)
        self.gen("ovl:overlays/ovl_0000abcd.bin")
        cpp = (self.out / "ovl_0000abcd_translated.cpp").read_text()
        self.assertIn("{0x28000, 0x28004,", cpp)
        self.assertIn("{0x28010, 0x28014,", cpp)
        kind, pc, d0 = self.run_prog("ovl_0000abcd_translated.cpp", "ovl_0000abcd", 0x28000)
        self.assertEqual((kind, pc, d0), (5, 0x28010, 1))   # NotTranslated at the second range, before running it
        kind, pc, d0 = self.run_prog("ovl_0000abcd_translated.cpp", "ovl_0000abcd", 0x28010)
        self.assertEqual((kind, pc, d0), (5, 0x123456, 2))  # rts out of the program

    def test_overlapping_decodes_share_one_range(self):
        # $28000: move.w #$7001,d0 ; rts  and, seeded mid-instruction, $28002: moveq #1,d0 ; rts
        d = self.code / "overlays"
        d.mkdir()
        (d / "ovl_0000beef.bin").write_bytes(bytes.fromhex("303C70014E75") + bytes(0x100))
        self.seeds("ovl_0000beef", 0x28000, 0x28002)
        self.gen("ovl:overlays/ovl_0000beef.bin")
        cpp = (self.out / "ovl_0000beef_translated.cpp").read_text()
        self.assertIn("{0x28000, 0x28006,", cpp)
        self.assertIn("kOvl_0000beefRangeCount = 1;", cpp)

    def test_ovl909_exits_when_leaving_range(self):
        (self.code / "ovl_909.bin").write_bytes(self.IMG + bytes(0x100))
        self.seeds("ovl909", 0x28000)
        self.gen("ovl909")
        kind, pc, d0 = self.run_prog("ovl909_translated.cpp", "ovl909", 0x28000)
        self.assertEqual((kind, pc, d0), (5, 0x28010, 1))

    def test_mainram_exits_when_leaving_range(self):
        sp = bytearray(0x1800)
        sp[0x1300:0x1300 + len(self.IMG)] = self.IMG
        (self.code / "scd_sub_sp.bin").write_bytes(bytes(sp))
        self.gen("mainram")
        kind, pc, d0 = self.run_prog("mainram_translated.cpp", "mainram", 0xFFE100)
        self.assertEqual((kind, pc, d0), (5, 0xFFE110, 1))

    def test_sub_runs_across_ranges(self):
        sp = bytearray(0x200)
        sp[0x2A:0x2A + len(self.IMG)] = self.IMG     # entry $602A
        (self.code / "scd_sub_sp.bin").write_bytes(bytes(sp))
        (self.code / "scd_subcode.bin").write_bytes(bytes(0x100))
        self.gen("sub")
        kind, pc, d0 = self.run_prog("sub_translated.cpp", "sub", 0x602A)
        self.assertEqual((kind, pc, d0), (5, 0x123456, 2))

    def test_trap_is_translated_and_descent_continues(self):
        # $28000: trap #0 ; moveq #1,d0 ; rts. The handler returns to $28002, so it is decoded too.
        d = self.code / "overlays"
        d.mkdir()
        (d / "ovl_0000c0de.bin").write_bytes(bytes.fromhex("4E4070014E75") + bytes(0x100))
        self.seeds("ovl_0000c0de", 0x28000)
        self.gen("ovl:overlays/ovl_0000c0de.bin")
        cpp = (self.out / "ovl_0000c0de_translated.cpp").read_text()
        self.assertIn("case 0x28002:", cpp)
        self.assertIn("budget -= int32_t(c.exc_cycles) - m68ki_cycles[0][0x4E40];", cpp)
        kind, pc, d0 = self.run_prog("ovl_0000c0de_translated.cpp", "ovl_0000c0de", 0x28000)
        self.assertEqual((kind, pc, d0), (5, 0x0, 0))   # vector 32 is 0 in the harness: NotTranslated at the handler


if __name__ == "__main__":
    unittest.main()
