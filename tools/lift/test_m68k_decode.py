"""Decoder output for exceptions and SR writes (ADR-003 M2). Synthetic encodings, no disc needed."""
import pathlib, sys, unittest

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from m68k_decode import decode
from emit import flow_code

PC = 0x1000


def dec(hexs):
    raw = bytes.fromhex(hexs) + bytes(16)
    r16 = lambda a: (raw[a - PC] << 8 | raw[a - PC + 1]) if 0 <= a - PC < len(raw) - 1 else 0
    ins = decode(r16, PC)
    return ins, '\n'.join(ins.code + flow_code(ins))


class TestExceptions(unittest.TestCase):
    def test_trap_uses_vector_32_plus_n_and_continues(self):
        for n in (0, 7, 15):
            ins, code = dec('%04X' % (0x4E40 + n))
            self.assertEqual(ins.flow, ('exc',))
            self.assertIn('npc = lift::exception_entry(c, %d, 0x1002);' % (32 + n), code)
            self.assertNotIn('Exit::Trap', code)

    def test_chk(self):
        ins, code = dec('4181')                                   # chk.w d1,d0
        self.assertEqual(ins.flow, ('exc',))
        self.assertIn('int16_t(c.d[0])', code)
        self.assertIn('npc = lift::exception_entry(c, 6, 0x1002);', code)
        ins, code = dec('45BC0010')                               # chk.w #$10,d2
        self.assertEqual(ins.size, 4)
        self.assertIn('int16_t(c.d[2])', code)
        self.assertIn('npc = lift::exception_entry(c, 6, 0x1004);', code)

    def test_divide_by_zero_raises_vector_5(self):
        for hexs in ('80FC0000', '83FC0000'):                    # divu #0,d0 ; divs #0,d1
            ins, code = dec(hexs)
            self.assertEqual(ins.flow, ('exc',))
            self.assertIn('npc = lift::exception_entry(c, 5, 0x1004);', code)
            self.assertNotIn('DivZero', code)

    def test_stop(self):
        ins, code = dec('4E722000')
        self.assertEqual((ins.flow, ins.size, ins.sr_write), (('exc',), 4, True))
        self.assertIn('lift::Exit::Unsupported', code.splitlines()[0])   # user mode: Musashi takes the privilege violation
        self.assertIn('if (lift::set_sr(c, 0x2000)) return lift::Exit{0x1004, lift::Exit::Irq};', code)
        self.assertIn('return lift::Exit{0x1004, lift::Exit::Stop};', code)

    def test_plain_instruction_is_not_an_sr_write(self):
        ins, _ = dec('7001')                                      # moveq #1,d0
        self.assertFalse(ins.sr_write)


if __name__ == '__main__':
    unittest.main()
