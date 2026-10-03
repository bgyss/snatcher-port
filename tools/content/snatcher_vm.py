"""Snatcher scenario-script VM (Sub CPU, SUBCODE.BIN at $D400): instruction decoding.

Recovered from the VM's own instruction-skip routine at $1699A (used to step over untaken blocks):
  - Scripts run from $1A800; the dispatcher at $16974 reads the opcode byte at (a2 + a3).
  - Opcode table at $16CDC: 4 bytes per opcode (0x00..0x4A valid): byte 0 = type of operand 1, byte 1 = types of
    operands 2 (high nibble) and 3 (low nibble), word 2 = handler offset from $15400.
  - Operand types: 0 and 7 = no bytes; 4 = a nested instruction (expression / argument instruction);
    any other = a 16-bit big-endian value.
  - If any operand type has bit 3 set, the instruction is a block: the 16-bit word after the opcode is the address
    (script offset) where the block ends, and the skip routine jumps there.
Operand meanings per opcode are not decoded yet; see OPCODES for what is known.
"""
import struct
import sys

sys.setrecursionlimit(20000)   # sequences are right-nested (op25 a, op25 b, ...) and get thousands deep

SUBCODE_BASE = 0xD400
TABLE = 0x16CDC
HANDLER_BASE = 0x15400
NUM_OPS = 0x4B


def load_table(subcode: bytes):
    o = TABLE - SUBCODE_BASE
    table = []
    for op in range(NUM_OPS):
        b0, b1, h = struct.unpack(">BBH", subcode[o + op * 4:o + op * 4 + 4])
        table.append({"types": (b0, b1 >> 4, b1 & 15), "handler": HANDLER_BASE + h})
    return table


class Decoder:
    def __init__(self, subcode: bytes, script: bytes, end: int):
        self.t = load_table(subcode)
        self.s = script
        self.end = min(end, len(script))

    def insn(self, pc, depth=0):
        """Decodes one instruction at pc -> dict(pc, op, args, size, block_end?, nested?)."""
        if pc >= len(self.s):
            raise ValueError(f"pc {pc:#x} past end")
        op = self.s[pc]
        if op >= NUM_OPS:
            raise ValueError(f"bad opcode {op:#04x} at {pc:#x}")
        if depth > 2000:
            raise ValueError(f"nesting too deep at {pc:#x}")
        types = self.t[op]["types"]
        node = {"pc": pc, "op": op, "args": []}
        p = pc + 1
        if any(t & 8 for t in types):
            node["block_end"] = struct.unpack(">H", self.s[p:p + 2])[0]
            p += 2
            types = tuple(t for t in types if not t & 8)
        for t in types:
            if t in (0, 7):
                continue
            if t == 4:
                sub = self.insn(p, depth + 1)
                node["args"].append(sub)
                p += sub["size"]
            else:
                node["args"].append(struct.unpack(">H", self.s[p:p + 2])[0])
                p += 2
        node["size"] = p - pc
        return node

    def linear(self, start=0):
        """Top-level instructions from start up to the 0xFF padding (0xFF is never an opcode, but may be operand data,
        so the end is found by decoding rather than by trimming padding), in address order."""
        pc, out = start, []
        while pc < self.end and self.s[pc] != 0xFF:
            if self.s[pc] == 0x00 and (pc + 1 >= len(self.s) or self.s[pc + 1] == 0xFF):   # a bare 00 before the padding ends the script
                out.append({"pc": pc, "op": 0, "args": [], "size": 1, "end_marker": True})
                pc += 1
                break
            n = self.insn(pc)
            out.append(n)
            pc += n["size"]
        return out, pc


def fmt(n):
    args = ", ".join(fmt(a) if isinstance(a, dict) else f"0x{a:04x}" for a in n["args"])
    blk = f" [block -> 0x{n['block_end']:04x}]" if "block_end" in n else ""
    return f"op{n['op']:02x}({args}){blk}"
