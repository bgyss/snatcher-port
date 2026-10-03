"""Readable screenplay from decoded script bytecode (snatcher_vm) and text (snatcher_text).

Opcode meanings used here (verified across all 39 scripts unless marked ?):
  op43(op20(speaker, text))  show a line: text = offset into this script's text table (8,808/8,808 resolve);
                             speaker = character / portrait id (? names are assigned by a human in speakers.md)
  op26 [block]               a group of choices; its op38 items are the menu options (?)
  op38(label, body)          a menu option: label = text offset (3,755/3,755 resolve), body = what it does
  op25(a, b)                 sequence: a then b
  op30(x)                    statement
  op2b(addr)                 continue at script address (goto)
Everything else is printed raw as opXX(args) so nothing is hidden.
"""
import snatcher_text

SEQ, STMT, MENU_ITEM, LINE_OP, LINE_ARGS, GOTO, BLOCK = 0x25, 0x30, 0x38, 0x43, 0x20, 0x2B, 0x26


class Screenplay:
    def __init__(self, texts, speakers=None):
        self.texts = texts            # offset -> markup
        self.speakers = speakers or {}
        self.lines = []
        self.line_count = 0

    def name(self, spk):
        return self.speakers.get(spk, f"speaker {spk}")

    def text(self, off):
        return snatcher_text.plain(self.texts.get(off, f"<missing text 0x{off:x}>")).replace("\n", " ")

    def emit(self, indent, s):
        self.lines.append("    " * indent + s)

    def node(self, n, ind):
        op, args = n["op"], n["args"]
        if n.get("end_marker"):
            self.emit(ind, "END OF SCRIPT")
        elif op == SEQ:
            for a in args:
                self.node(a, ind) if isinstance(a, dict) else self.emit(ind, f"0x{a:04x}")
        elif op == STMT and args and isinstance(args[0], dict):
            self.node(args[0], ind)
        elif op == LINE_OP and args and isinstance(args[0], dict) and args[0]["op"] == LINE_ARGS:
            spk, off = args[0]["args"]
            self.line_count += 1
            self.emit(ind, f"**{self.name(spk)}:** {self.text(off)}")
        elif op == MENU_ITEM and len(args) == 2 and not isinstance(args[0], dict):
            self.emit(ind, f"▸ **{self.text(args[0])}**")
            if isinstance(args[1], dict):
                self.node(args[1], ind + 1)
        elif op == BLOCK:
            items = [a for a in args if isinstance(a, dict)]
            self.emit(ind, f"⟨choice block, ends @{n.get('block_end', 0):04x}⟩")
            for a in items:
                self.node(a, ind + 1)
        elif op == GOTO and args and not isinstance(args[0], dict):
            self.emit(ind, f"→ @{args[0]:04x}")
        else:
            simple = [a for a in args if not isinstance(a, dict)]
            nested = [a for a in args if isinstance(a, dict)]
            blk = f" [block → @{n['block_end']:04x}]" if "block_end" in n else ""
            self.emit(ind, f"`op{op:02x}({', '.join(f'0x{a:04x}' for a in simple)})`{blk}")
            for a in nested:
                self.node(a, ind + 1)

    def script(self, insns):
        for n in insns:
            self.emit(0, "")
            self.emit(0, f"@{n['pc']:04x}")
            self.node(n, 0)
        return self.lines
