#!/usr/bin/env python3
"""
trace.py -- symbolic dataflow over the IR produced by extract.py.

Shows the *arithmetic* of SoftDrv's MMX kernels -- lane widths, shift amounts, bias constants, the
order light and fog are folded together -- so the portable C path can reproduce it exactly.  It is
a reading aid, not a translator: the output is not compilable.

Each instruction is rewritten as an assignment whose right-hand side names a portable helper
(`add_i16x4`, `mulhi_i16x4`, `shr64`, ...).  Long expressions spill into numbered temporaries so
blocks stay readable.  Registers entering a block from elsewhere print as `eax@in`.

    python3 trace.py <file.cpp> <function> [...]      # annotated basic blocks
    python3 trace.py --summary <file.cpp> <function>  # constants / symbols / opcode mix only
    python3 trace.py --blocks  <file.cpp> <function>  # control-flow skeleton only

`--summary` is the usual first step: it groups every immediate by the opcode that consumed it,
which exposes constants such as the fog scale (13-bit + MMXFlashOffset13) and the knight-dither
offsets.
"""

import collections
import sys

import extract
from extract import BRANCHES, GPR32, MMX

# ---------------------------------------------------------------------------
# Instruction semantics
#
# name -> template.  Template placeholders: {0} = dest value, {1}, {2} = sources.
# `None` means "no modelled value" (flags-only or control flow).
# ---------------------------------------------------------------------------

MMX_OPS = {
    "movq":       "{1}",
    "movd":       "movd({1})",
    "paddb":      "add_i8x8({0}, {1})",
    "paddw":      "add_i16x4({0}, {1})",
    "paddd":      "add_i32x2({0}, {1})",
    "paddsw":     "adds_i16x4({0}, {1})",
    "paddusb":    "addus_u8x8({0}, {1})",
    "paddusw":    "addus_u16x4({0}, {1})",
    "psubb":      "sub_i8x8({0}, {1})",
    "psubw":      "sub_i16x4({0}, {1})",
    "psubd":      "sub_i32x2({0}, {1})",
    "psubsw":     "subs_i16x4({0}, {1})",
    "psubusb":    "subus_u8x8({0}, {1})",
    "pmulhw":     "mulhi_i16x4({0}, {1})",
    "pmullw":     "mullo_i16x4({0}, {1})",
    "pmaddwd":    "madd_i16x4({0}, {1})",
    "pand":       "and64({0}, {1})",
    "pandn":      "andnot64({0}, {1})",
    "por":        "or64({0}, {1})",
    "pxor":       "xor64({0}, {1})",
    "psllq":      "shl64({0}, {1})",
    "psrlq":      "shr64({0}, {1})",
    "psllw":      "shl_i16x4({0}, {1})",
    "psrlw":      "shr_u16x4({0}, {1})",
    "psraw":      "shr_i16x4({0}, {1})",
    "pslld":      "shl_i32x2({0}, {1})",
    "psrld":      "shr_u32x2({0}, {1})",
    "psrad":      "shr_i32x2({0}, {1})",
    "punpcklbw":  "unpacklo_u8x8({0}, {1})",
    "punpckhbw":  "unpackhi_u8x8({0}, {1})",
    "punpcklwd":  "unpacklo_i16x4({0}, {1})",
    "punpckhwd":  "unpackhi_i16x4({0}, {1})",
    "punpckldq":  "unpacklo_i32x2({0}, {1})",
    "punpckhdq":  "unpackhi_i32x2({0}, {1})",
    "packuswb":   "packus_i16_to_u8({0}, {1})",
    "packsswb":   "packs_i16_to_i8({0}, {1})",
    "packssdw":   "packs_i32_to_i16({0}, {1})",
    "pcmpeqb":    "cmpeq_i8x8({0}, {1})",
    "pcmpeqw":    "cmpeq_i16x4({0}, {1})",
    "pcmpeqd":    "cmpeq_i32x2({0}, {1})",
    "pcmpgtw":    "cmpgt_i16x4({0}, {1})",
    "pcmpgtd":    "cmpgt_i32x2({0}, {1})",
}

GPR_OPS = {
    "mov":   "{1}",
    "movzx": "zext({1})",
    "movsx": "sext({1})",
    "lea":   "&{1}",
    "add":   "({0} + {1})",
    "adc":   "({0} + {1} + CF)",
    "sub":   "({0} - {1})",
    "sbb":   "({0} - {1} - CF)",
    "and":   "({0} & {1})",
    "or":    "({0} | {1})",
    "xor":   "({0} ^ {1})",
    "shl":   "({0} << {1})",
    "sal":   "({0} << {1})",
    "shr":   "({0} >>u {1})",
    "sar":   "({0} >>s {1})",
    "rol":   "rotl({0}, {1})",
    "ror":   "rotr({0}, {1})",
    "imul":  "({0} * {1})",
    "neg":   "(-{0})",
    "not":   "(~{0})",
    "inc":   "({0} + 1)",
    "dec":   "({0} - 1)",
}

FLAG_OPS = {"cmp", "test"}
NOVALUE = {"nop", "emms", "push", "pop", "call", "ret", "retn", "fld", "fmul",
           "fadd", "fstp", "fxch", "fsub", "fdiv", "fild", "fistp"}

MAX_EXPR = 58          # spill to a temporary beyond this many characters


# ---------------------------------------------------------------------------
# Basic blocks
# ---------------------------------------------------------------------------

class Block:
    def __init__(self, index, labels):
        self.index = index
        self.labels = list(labels)
        self.insns = []
        self.succs = []

    @property
    def name(self):
        return self.labels[0] if self.labels else "B%d" % self.index


def build_blocks(insns):
    blocks, cur = [], None
    for ins in insns:
        if cur is None or ins.labels:
            cur = Block(len(blocks), ins.labels)
            blocks.append(cur)
        cur.insns.append(ins)
        if ins.op in BRANCHES:
            target = ins.operands[0].name if ins.operands else "?"
            cur.succs.append(target)
            if not BRANCHES[ins.op]:
                cur.succs.append("<fallthrough>")
            cur = None
    return blocks


# ---------------------------------------------------------------------------
# Symbolic evaluation
# ---------------------------------------------------------------------------

class State:
    def __init__(self):
        self.regs = {}
        self.temps = []        # (name, expr) emitted before the current instruction
        self.ntemp = 0         # monotonic across the block, so names never collide

    def read(self, op):
        if op is None:
            return "?"
        if op.kind == "reg":
            return self.regs.get(op.name, op.name + "@in")
        if op.kind == "imm":
            if op.value is not None:
                return hex(op.value) if abs(op.value) >= 256 else str(op.value)
            return op.text
        if op.kind == "sym":
            return "&" + op.name
        if op.kind == "label":
            return op.name
        # memory
        return "*" + str(op)

    def bind(self, op, expr):
        if op is None or op.kind != "reg":
            return
        if len(expr) > MAX_EXPR:
            name = "t%d" % self.ntemp
            self.ntemp += 1
            self.temps.append((name, expr))
            expr = name
        self.regs[op.name] = expr


def annotate(block, state):
    """Yield (insn, rendered_lines) for one block."""
    out = []
    for ins in block.insns:
        state.temps = []
        dst = ins.operands[0] if ins.operands else None
        src = ins.operands[1] if len(ins.operands) > 1 else None

        table = MMX_OPS if ins.op in MMX_OPS else (GPR_OPS if ins.op in GPR_OPS else None)

        text = None
        if table is not None:
            a = state.read(dst)
            b = state.read(src) if src is not None else ""
            expr = table[ins.op].format(a, b)
            if dst is not None and dst.kind == "reg":
                state.bind(dst, expr)
                text = "%s = %s" % (dst.name, state.regs[dst.name])
            else:
                # Store to memory: report the value written.
                text = "%s = %s" % (str(dst), expr if ins.op in ("mov", "movq", "movd") else expr)
        elif ins.op in FLAG_OPS:
            a = state.read(dst)
            b = state.read(src) if src is not None else ""
            text = "flags = %s %s %s" % (a, "-" if ins.op == "cmp" else "&", b)
        elif ins.op in BRANCHES:
            text = "-> %s" % (ins.operands[0].name if ins.operands else "?")
        elif ins.op == "call":
            text = "call %s" % (ins.operands[0].name if ins.operands else "?")
            state.regs.clear()
        elif ins.op in NOVALUE:
            text = ins.op
        else:
            text = str(ins)
            if dst is not None and dst.kind == "reg":
                state.regs.pop(dst.name, None)

        out.append((ins, list(state.temps), text))
    return out


# ---------------------------------------------------------------------------
# Reports
# ---------------------------------------------------------------------------

def summarize(name, insns):
    consts = collections.defaultdict(collections.Counter)
    reads, writes = collections.Counter(), collections.Counter()
    ops = collections.Counter()
    structs = collections.Counter()

    for ins in insns:
        ops[ins.op] += 1
        for i, op in enumerate(ins.operands):
            if op.kind == "imm":
                key = op.text if op.value is None else (
                    hex(op.value) if abs(op.value) >= 256 else str(op.value))
                consts[ins.op][key] += 1
            elif op.kind == "sym":
                reads[op.name] += 1
            elif op.kind == "mem":
                if op.sym:
                    (writes if i == 0 else reads)[op.sym] += 1
                if op.struct or op.field:
                    structs["%s.%s" % (op.struct or "?", op.field)] += 1

    print("=" * 78)
    print("%s -- %d instructions" % (name, len(insns)))
    print("=" * 78)

    print("\n-- immediates by opcode (shift amounts and biases live here) --")
    for op in sorted(consts):
        items = ", ".join("%s x%d" % (k, v) for k, v in consts[op].most_common())
        print("   %-11s %s" % (op, items))

    print("\n-- memory symbols written --")
    for s, c in writes.most_common():
        print("   %-28s x%d" % (s, c))
    print("\n-- memory symbols read --")
    for s, c in reads.most_common():
        print("   %-28s x%d" % (s, c))
    if structs:
        print("\n-- struct fields touched --")
        for s, c in structs.most_common():
            print("   %-28s x%d" % (s, c))

    mmx = {k: v for k, v in ops.items() if k in MMX_OPS}
    print("\n-- opcode mix (%d MMX of %d total) --" % (sum(mmx.values()), sum(ops.values())))
    print("   " + ", ".join("%s:%d" % (k, v) for k, v in ops.most_common()))
    print()


def show_blocks(name, insns):
    blocks = build_blocks(insns)
    print("=" * 78)
    print("%s -- %d blocks" % (name, len(blocks)))
    print("=" * 78)
    for b in blocks:
        lbl = ", ".join(b.labels) or "(fallthrough)"
        print("  %-4s %-38s %3d insns -> %s"
              % (b.name, lbl, len(b.insns), ", ".join(b.succs) or "fallthrough"))
    print()


def show_trace(name, insns):
    blocks = build_blocks(insns)
    print("=" * 78)
    print("%s -- symbolic trace (%d blocks, %d instructions)"
          % (name, len(blocks), len(insns)))
    print("=" * 78)
    for b in blocks:
        for lbl in b.labels:
            print("\n%s:" % lbl)
        if not b.labels:
            print("\n%s:" % b.name)
        state = State()
        for ins, temps, text in annotate(b, state):
            for tname, texpr in temps:
                print("        %-12s %s" % (tname, "= " + texpr))
            note = ("        ; " + ins.comment) if ins.comment else ""
            print("  %5d  %-58s %s" % (ins.line, text, note.strip()))
        if b.succs:
            print("         -> %s" % ", ".join(b.succs))
    print()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv):
    mode = "trace"
    if "--summary" in argv:
        mode = "summary"
    if "--blocks" in argv:
        mode = "blocks"
    argv = [a for a in argv if not a.startswith("--")]
    if len(argv) < 2:
        print(__doc__)
        return 2

    path, names = argv[0], argv[1:]
    funcs = extract.extract(path, set(names))
    missing = [n for n in names if n not in funcs]
    for n in missing:
        print("!! %s: not found or contains no asm" % n, file=sys.stderr)

    for n in names:
        if n not in funcs:
            continue
        if mode == "summary":
            summarize(n, funcs[n])
        elif mode == "blocks":
            show_blocks(n, funcs[n])
        else:
            show_trace(n, funcs[n])
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
