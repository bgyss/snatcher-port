"""Unit tests for coverage_report.py (no disc needed)."""
import io, os, pathlib, sys, tempfile, unittest
from contextlib import redirect_stdout

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import coverage_report as cr

HDR = "# snatcher-coverage v1 frames=100 translated=1\n"


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = pathlib.Path(self.tmp.name)
        code = self.dir / "code"
        code.mkdir()
        for name, size in (("scd_main_ip.bin", 0x6000), ("scd_sub_sp.bin", 0x1800), ("scd_subcode.bin", 0xD000)):
            (code / name).write_bytes(bytes(size))
        self.code = str(code)
        self.sizes = cr.code_sizes(self.code)

    def tearDown(self):
        self.tmp.cleanup()

    def census(self, name, body):
        p = self.dir / name
        p.write_text(HDR + body)
        return str(p)


class TestProgramFor(Base):
    def test_mapping(self):
        f = lambda cpu, image, pc: cr.program_for(cpu, image, pc, self.sizes)
        self.assertEqual(f("main", 0, 0xFF0584), "main")
        self.assertEqual(f("main", 0, 0xFFE100), "mainram")
        self.assertEqual(f("main", 0, 0xFFE3CE), "mainram")
        self.assertIsNone(f("main", 0, 0xFFE3D0))
        self.assertIsNone(f("main", 0, 0x00036C))
        self.assertEqual(f("sub", 0, 0x602A), "sub")
        self.assertEqual(f("sub", 0, 0xD5CC), "sub")
        self.assertIsNone(f("sub", 0, 0x5F22))
        self.assertEqual(f("sub", 0x1A2B3C4D, 0x28010), "ovl_1a2b3c4d")


class TestMergeAndSeeds(Base):
    def test_merge_sums_counts(self):
        a = self.census("a.tsv", "sub\t00000000\tnot_translated\t00d5cc\t2\n")
        b = self.census("b.tsv", "sub\t00000000\tnot_translated\t00d5cc\t3\n")
        self.assertEqual(cr.merge([a, b])[("sub", 0, "not_translated", 0xD5CC)], 5)

    def test_seeds_only_from_interpreter_and_not_translated(self):
        a = self.census("a.tsv",
                        "main\t00000000\tirq_entry\tff0584\t1\n"
                        "main\t00000000\tinterpreter\tff0600\t1\n"
                        "main\t00000000\thle_stub\t00036c\t1\n"
                        "sub\t00000000\tnot_translated\t00d5cc\t1\n"
                        "sub\t1a2b3c4d\tnot_translated\t028010\t1\n"
                        "sub\t00000000\tnot_translated\t005f22\t1\n")
        seeds = self.dir / "seeds"
        cr.write_seeds(cr.merge([a]), self.sizes, str(seeds))
        self.assertEqual(sorted(p.name for p in seeds.iterdir()), ["main.txt", "ovl_1a2b3c4d.txt", "sub.txt"])
        lines = lambda n: [l for l in (seeds / n).read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(lines("main.txt"), ["ff0600"])
        self.assertEqual(lines("sub.txt"), ["00d5cc"])
        self.assertEqual(lines("ovl_1a2b3c4d.txt"), ["028010"])

    def test_seeds_accumulate_across_runs(self):
        seeds = self.dir / "seeds"
        cr.write_seeds(cr.merge([self.census("a.tsv", "sub\t00000000\tinterpreter\t00d5cc\t1\n")]), self.sizes, str(seeds))
        cr.write_seeds(cr.merge([self.census("b.tsv", "sub\t00000000\tinterpreter\t00d400\t1\n")]), self.sizes, str(seeds))
        body = [l for l in (seeds / "sub.txt").read_text().splitlines() if not l.startswith("#")]
        self.assertEqual(body, ["00d400", "00d5cc"])


class TestCheck(Base):
    def run_main(self, *args):
        buf = io.StringIO()
        with redirect_stdout(buf):
            rc = cr.main(list(args))
        return rc, buf.getvalue()

    def test_check_passes_with_only_allowed_causes(self):
        a = self.census("a.tsv",
                        "main\t00000000\tirq_entry\tff0584\t9\n"
                        "main\t00000000\thle_stub\t00036c\t9\n"
                        "sub\t00000000\tunsupported\t00d600\t9\n")
        rc, out = self.run_main(a, "--code-dir", self.code, "--check")
        self.assertEqual(rc, 0, out)

    def test_check_fails_on_not_translated_and_names_unmapped(self):
        a = self.census("a.tsv", "sub\t00000000\tnot_translated\t01b000\t4\n"
                                 "main\t00000000\tcode_mismatch\tffe100\t1\n")
        rc, out = self.run_main(a, "--code-dir", self.code, "--check")
        self.assertEqual(rc, 1)
        self.assertIn("unmapped:sub", out)
        self.assertIn("mainram", out)
        self.assertIn("FAIL", out)


if __name__ == "__main__":
    unittest.main()
