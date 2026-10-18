#!/usr/bin/env python3
"""
extract.py -- pull MSVC `__asm` blocks out of UE1's SoftDrv sources into a normalized IR.

Locates a function in Source/SoftDrv/Src/{DrawSurf,DrawPoly,DrawTile}.cpp, gathers its `__asm` text
and returns a structured instruction list, which trace.py and emit.py consume.  It is the
mechanical half of reading the x86/MMX kernels' arithmetic (shift amounts, bias constants, lane
packing) so the portable C path can reproduce it.

Handles the MSVC inline-asm dialect this codebase uses:

    mov     esi,[eax]FTexSetup.X        struct-relative:  base + offsetof(FTexSetup,X)
    mov     ecx,[eax+ 8*3]FTexSetup.X   ... with an extra displacement
    mov     ebp,offset Photon           address-of a C symbol
    add     ebx,[DWORD PTR LightOS+12]  sized symbol reference with displacement
    mov     eax,DWORD PTR FastSqrtTbl [eax]
    paddw   mm3, MMXFlashOffset13       bare symbol = memory operand
    movq    [ebp+8], mm3
    call    SampleFogLight ;// comment   `;` and `//` both start comments
    __asm   emms                        one-instruction form (used inside macros)

Usage:
    python3 extract.py <file.cpp> [function ...]      # human-readable dump
    python3 extract.py --json <file.cpp> [function]   # IR as JSON
    python3 extract.py --list <file.cpp>              # functions that contain asm
"""

import json
import re
import sys

# ---------------------------------------------------------------------------
# Register sets
# ---------------------------------------------------------------------------

GPR32 = {"eax", "ebx", "ecx", "edx", "esi", "edi", "ebp", "esp"}
GPR16 = {"ax", "bx", "cx", "dx", "si", "di", "bp", "sp"}
GPR8 = {"al", "ah", "bl", "bh", "cl", "ch", "dl", "dh"}
MMX = {"mm%d" % i for i in range(8)}
FPU = {"st", "st(0)", "st(1)", "st(2)", "st(3)", "st(4)", "st(5)", "st(6)", "st(7)"}
REGS = GPR32 | GPR16 | GPR8 | MMX | FPU

SIZE_PREFIXES = ("qword ptr", "dword ptr", "word ptr", "byte ptr", "mmword ptr")

# Directives that are not instructions.
DIRECTIVES = {"align", "doalign16", "_emit", "even"}

# Branch opcodes -> whether the branch is unconditional.
BRANCHES = {
    "jmp": True, "je": False, "jz": False, "jne": False, "jnz": False,
    "jb": False, "jnb": False, "jbe": False, "jnbe": False,
    "ja": False, "jna": False, "jae": False, "jnae": False,
    "jl": False, "jnl": False, "jle": False, "jnle": False,
    "jg": False, "jng": False, "jge": False, "jnge": False,
    "js": False, "jns": False, "jc": False, "jnc": False,
    "jo": False, "jno": False, "jp": False, "jnp": False,
    "jecxz": False, "loop": False, "loope": False, "loopne": False,
}


# ---------------------------------------------------------------------------
# Operand model
# ---------------------------------------------------------------------------

class Operand:
    """One decoded operand.

    kind is one of:
      reg   -- name
      imm   -- value (int or None if unevaluated), text
      mem   -- base, index, scale, disp, sym, struct, field, size
      sym   -- name (bare `offset foo`, i.e. address-of)
      label -- name (branch/call target)
    """

    __slots__ = ("kind", "name", "value", "text", "base", "index", "scale",
                 "disp", "sym", "struct", "field", "size")

    def __init__(self, kind, **kw):
        self.kind = kind
        for slot in self.__slots__:
            if slot != "kind":
                setattr(self, slot, kw.get(slot))
        if self.scale is None:
            self.scale = 1

    def to_dict(self):
        d = {"kind": self.kind}
        for slot in self.__slots__:
            if slot == "kind":
                continue
            v = getattr(self, slot)
            if v is not None and not (slot == "scale" and v == 1):
                d[slot] = v
        return d

    def __str__(self):
        if self.kind == "reg":
            return self.name
        if self.kind == "imm":
            return self.text
        if self.kind == "sym":
            return "&%s" % self.name
        if self.kind == "label":
            return self.name
        # mem
        parts = []
        if self.sym:
            parts.append(self.sym)
        if self.struct:
            parts.append("%s.%s" % (self.struct, self.field))
        if self.base:
            parts.append(self.base)
        if self.index:
            parts.append("%s*%d" % (self.index, self.scale))
        if self.disp:
            parts.append(str(self.disp))
        inner = " + ".join(p for p in parts if p)
        return "[%s]" % inner


class Insn:
    __slots__ = ("op", "operands", "labels", "line", "raw", "comment")

    def __init__(self, op, operands, labels, line, raw, comment):
        self.op = op
        self.operands = operands
        self.labels = labels          # labels attached to this instruction
        self.line = line              # 1-based source line
        self.raw = raw
        self.comment = comment

    def to_dict(self):
        return {
            "op": self.op,
            "operands": [o.to_dict() for o in self.operands],
            "labels": self.labels,
            "line": self.line,
            "raw": self.raw,
            "comment": self.comment,
        }

    def __str__(self):
        s = "%-11s %s" % (self.op, ", ".join(str(o) for o in self.operands))
        return s.rstrip()


# ---------------------------------------------------------------------------
# Numeric literals
# ---------------------------------------------------------------------------

def _eval_int(text):
    """Best-effort evaluation of an MSVC asm integer expression.

    Accepts C hex (0x1f), MASM hex (1Fh), decimal, and simple arithmetic such as
    `1* (3*8)` or `(1 << (FASTAPPROX_MAN_BITS+1))`.  Returns None when symbols
    are involved -- the raw text is kept either way.
    """
    t = text.strip()
    if not t:
        return None
    # MASM-style trailing-h hex, e.g. 0Fh / 3FFFFFFFh
    t = re.sub(r"\b([0-9][0-9A-Fa-f]*)h\b", lambda m: "0x" + m.group(1), t)
    if not re.fullmatch(r"[0-9a-fA-Fx()+\-*/ <>~&|]+", t):
        return None
    try:
        return int(eval(t, {"__builtins__": {}}, {}))  # noqa: S307 - arithmetic only
    except Exception:
        return None


# ---------------------------------------------------------------------------
# Operand parsing
# ---------------------------------------------------------------------------

_STRUCT_SUFFIX = re.compile(r"^\s*([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)")


def parse_operand(text, is_branch_target=False):
    t = text.strip()
    if not t:
        return None

    if is_branch_target:
        return Operand("label", name=t)

    low = t.lower()

    # Plain register.
    if low in REGS:
        return Operand("reg", name=low)

    # `offset foo` -- address of a C symbol.
    m = re.match(r"^offset\s+([A-Za-z_]\w*)$", t, re.I)
    if m:
        return Operand("sym", name=m.group(1))

    # Strip and remember a size prefix.
    size = None
    for pfx in SIZE_PREFIXES:
        if low.startswith(pfx):
            size = pfx.split()[0]
            t = t[len(pfx):].strip()
            low = t.lower()
            break

    # Bracketed form, possibly with a leading symbol and/or trailing struct field:
    #   [eax]FTexSetup.X      [eax+ 8*3]FTexSetup.X
    #   [ebp+8]               [MMXFlashOffset13]
    #   LightOS[eax*4]        FastSqrtTbl [eax]
    #   [DWORD PTR LightOS + 12]
    lead_sym = None
    m = re.match(r"^([A-Za-z_]\w*)\s*\[", t)
    if m:
        lead_sym = m.group(1)
        t = t[m.end() - 1:]
        low = t.lower()

    if t.startswith("["):
        depth = 0
        for i, ch in enumerate(t):
            if ch == "[":
                depth += 1
            elif ch == "]":
                depth -= 1
                if depth == 0:
                    inner = t[1:i]
                    tail = t[i + 1:]
                    break
        else:
            inner, tail = t[1:], ""

        # A size prefix can also live *inside* the brackets.
        inner_low = inner.lower().strip()
        for pfx in SIZE_PREFIXES:
            if inner_low.startswith(pfx):
                size = size or pfx.split()[0]
                inner = inner.strip()[len(pfx):]
                break

        struct = field = None
        m = _STRUCT_SUFFIX.match(tail)
        # (a trailing `Struct.Field` after the bracket; a dotted term *inside* the bracket is
        #  handled in the term loop below)
        if m:
            struct, field = m.group(1), m.group(2)

        base = index = None
        scale = 1
        disp_terms = []
        sym = lead_sym

        for sign, term in re.findall(r"([+-]?)\s*([^+\-]+)", inner):
            term = term.strip()
            if not term:
                continue
            tl = term.lower()
            m = re.fullmatch(r"([a-z]{2,3})\s*\*\s*(\d+)", tl)
            if m and m.group(1) in GPR32:
                index, scale = m.group(1), int(m.group(2))
                continue
            if tl in GPR32:
                if base is None:
                    base = tl
                else:
                    index = tl
                continue
            if re.fullmatch(r"[A-Za-z_]\w*", term) and tl not in REGS:
                # A bare identifier inside brackets is the symbol, unless it is a
                # macro constant used as a displacement (SHADE_R & friends).
                if sym is None and not term.isupper():
                    sym = term
                else:
                    disp_terms.append(sign + term)
                continue
            m = re.fullmatch(r"([A-Za-z_]\w*)\s*\.\s*([A-Za-z_]\w*)", term)
            if m:
                # `[LightMip.Mask]` -- a field of a C global, not a displacement. Record both so
                # the emitter can turn it into `[LightMip + <offset>]`.
                if sym is None:
                    sym = m.group(1)
                    struct = struct or m.group(1)
                    field = m.group(2)
                    continue
            disp_terms.append(sign + term)

        disp = " ".join(disp_terms).strip() or None
        if disp:
            val = _eval_int(disp)
            if val is not None:
                disp = val
        return Operand("mem", base=base, index=index, scale=scale, disp=disp,
                       sym=sym, struct=struct, field=field, size=size)

    # Bare identifier that is not a register: MSVC treats it as a memory operand
    # (e.g. `paddw mm3, MMXFlashOffset13`).  A trailing struct field can follow.
    m = re.fullmatch(r"([A-Za-z_]\w*)(?:\s*\.\s*([A-Za-z_]\w*))?", t)
    if m and m.group(1).lower() not in REGS:
        name, fld = m.group(1), m.group(2)
        if fld:
            return Operand("mem", sym=name, struct=None, field=fld, size=size)
        return Operand("mem", sym=name, size=size)

    # Otherwise: an immediate / constant expression.
    return Operand("imm", value=_eval_int(t), text=t)


def split_operands(text):
    """Split on commas that are not inside brackets or parens."""
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "[(":
            depth += 1
        elif ch in "])":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [o for o in (s.strip() for s in out) if o]


# ---------------------------------------------------------------------------
# Source scanning
# ---------------------------------------------------------------------------

_FUNC_RE = re.compile(
    r"^[ \t]?"                              # DrawPoly indents some definitions one space
    r"(?:static\s+|inline\s+|__declspec\([^)]*\)\s*)*"
    r"(?:[A-Za-z_][\w:<>*&]*\s+)+"          # return type (possibly qualified)
    r"((?:[A-Za-z_]\w*::)?[A-Za-z_]\w*)\s*(?:\(|$)"   # `(` may sit on the next line
)


def find_functions(path):
    """Yield (name, start_line, end_line) for every top-level function definition.

    Relies on this codebase's formatting: a definition starts in column 0.  The
    body is delimited by brace counting (comments and the `#if ASM` machinery are
    ignored), so single-line bodies like `static void MergeNone(...) {}` do not
    swallow everything up to the next column-0 brace.
    """
    lines = open(path, errors="replace").read().split("\n")
    i, n = 0, len(lines)
    while i < n:
        line = lines[i]
        if line.lstrip(" \t")[:1] not in ("", "/", "#", "}") and len(line) - len(line.lstrip(" \t")) <= 1:
            m = _FUNC_RE.match(line)
            if m:
                # Walk to the opening brace (may be on a following line).
                j = i
                while j < n and "{" not in _strip_comment(lines[j])[0]:
                    if ";" in _strip_comment(lines[j])[0]:
                        break
                    j += 1
                if j < n and "{" in _strip_comment(lines[j])[0]:
                    depth, k = 0, j
                    while k < n:
                        code = _strip_comment(lines[k])[0]
                        depth += code.count("{") - code.count("}")
                        # Brace counting alone overshoots when `#if/#else` arms
                        # hold unbalanced braces, so a column-0 `}` also ends the
                        # body -- that is this codebase's house style.
                        if k > j and code.rstrip() == "}":
                            break
                        if depth <= 0 and k >= j:
                            break
                        k += 1
                    yield m.group(1), i + 1, min(k + 1, n)
                    i = k + 1
                    continue
        i += 1


def _strip_comment(line):
    """Remove `/* */`, `//` and `;` comments; return (code, comment)."""
    # Single-line block comments first, so `/* x */ mov eax,1` still parses.
    line = re.sub(r"/\*.*?\*/", " ", line)
    cut = len(line)
    for tok in ("//", ";", "/*"):
        p = line.find(tok)
        if p != -1:
            cut = min(cut, p)
    return line[:cut], line[cut:].lstrip("/*; \t").rstrip()


def asm_lines(path, start_line, end_line):
    """Yield (lineno, code, comment) for the asm text inside a function body."""
    lines = open(path, errors="replace").read().split("\n")
    depth = None          # brace depth inside an `__asm { ... }` block
    in_block_comment = False
    for idx in range(start_line - 1, end_line):
        raw = lines[idx]
        if in_block_comment:
            end = raw.find("*/")
            if end == -1:
                continue
            raw = raw[end + 2:]
            in_block_comment = False
        code, comment = _strip_comment(raw)
        if "/*" in raw and "*/" not in raw[raw.find("/*"):]:
            in_block_comment = True
        code = code.rstrip("\\").strip()
        if not code and depth is None:
            continue

        if depth is None:
            m = re.match(r"^__asm\b(.*)$", code)
            if not m:
                continue
            rest = m.group(1).strip()
            if rest.startswith("{"):
                depth = 1
                rest = rest[1:].strip()
                if rest:
                    yield idx + 1, rest, comment
                continue
            if rest:
                # One-instruction form: `__asm emms;`
                yield idx + 1, rest.rstrip(";"), comment
                continue
            depth = 0     # brace is on the following line
            continue

        if depth == 0:
            if code.startswith("{"):
                depth = 1
                code = code[1:].strip()
                if not code:
                    continue
            else:
                depth = None
                continue

        opens, closes = code.count("{"), code.count("}")
        if closes and depth + opens - closes <= 0:
            code = code.split("}")[0].strip()
            depth = None
            if not code:
                continue
        else:
            depth += opens - closes

        if code:
            yield idx + 1, code, comment


def parse_asm(path, start_line, end_line, defines=None):
    """Decode a function's asm text into a list of Insn.

    `defines` resolves preprocessor conditionals that sit *inside* asm blocks (SoftDrv has
    `#if ALPHAFOG` arms in the middle of its light samplers). Without it both arms would be emitted
    back to back, which assembles fine and computes nonsense. Only the simple `#if NAME` / `#if 0` /
    `#if 1` / `#else` / `#endif` forms the kernels actually use are understood.
    """
    defines = defines or {}
    insns = []
    pending_labels = []
    pending_cpp = None
    cpp_stack = []          # (active_now, taken_any)

    for lineno, code, comment in asm_lines(path, start_line, end_line):
        if code.startswith("#"):
            d = code[1:].strip()
            if d.startswith("if"):
                expr = d.split(None, 1)[1].strip() if " " in d else ""
                if d.startswith("ifdef"):
                    val = expr in defines
                elif d.startswith("ifndef"):
                    val = expr not in defines
                else:
                    try:
                        val = bool(int(defines.get(expr, expr)))
                    except (TypeError, ValueError):
                        val = True      # unknown symbol: keep the arm rather than silently drop it
                cpp_stack.append([val, val])
            elif d.startswith("else"):
                if cpp_stack:
                    cpp_stack[-1][0] = not cpp_stack[-1][1]
            elif d.startswith("endif"):
                if cpp_stack:
                    cpp_stack.pop()
            pending_cpp = code
            continue

        if cpp_stack and not all(f[0] for f in cpp_stack):
            continue

        # `__asm` may prefix each line inside a macro.
        code = re.sub(r"^__asm\s+", "", code).strip().rstrip(";")
        if not code:
            continue

        # Peel leading labels (possibly several on one line).
        while True:
            m = re.match(r"^([A-Za-z_]\w*)\s*:(?!:)\s*(.*)$", code)
            if not m:
                break
            pending_labels.append(m.group(1))
            code = m.group(2).strip()
        if not code:
            continue

        parts = code.split(None, 1)
        op = parts[0].lower()
        rest = parts[1] if len(parts) > 1 else ""

        if op in DIRECTIVES:
            continue

        is_branch = op in BRANCHES or op == "call"
        operands = [parse_operand(o, is_branch) for o in split_operands(rest)]
        operands = [o for o in operands if o is not None]

        if pending_cpp:
            comment = ("%s  %s" % (pending_cpp, comment)).strip()
            pending_cpp = None

        insns.append(Insn(op, operands, pending_labels, lineno, code, comment))
        pending_labels = []

    return insns


def extract(path, names=None, defines=None):
    """Return {function_name: [Insn, ...]} for functions containing asm."""
    out = {}
    for name, a, b in find_functions(path):
        if names and name not in names:
            continue
        insns = parse_asm(path, a, b, defines)
        if insns:
            out[name] = insns
    return out


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def main(argv):
    as_json = "--json" in argv
    listing = "--list" in argv
    argv = [a for a in argv if not a.startswith("--")]
    if not argv:
        print(__doc__)
        return 2

    path, names = argv[0], argv[1:] or None

    if listing:
        for name, a, b in find_functions(path):
            insns = parse_asm(path, a, b, defines)
            if insns:
                print("%-34s lines %6d-%-6d  %4d insns" % (name, a, b, len(insns)))
        return 0

    funcs = extract(path, names)
    if as_json:
        print(json.dumps(
            {k: [i.to_dict() for i in v] for k, v in funcs.items()}, indent=1))
        return 0

    for name, insns in funcs.items():
        print("=" * 78)
        print("%s   (%d instructions)" % (name, len(insns)))
        print("=" * 78)
        for ins in insns:
            for lbl in ins.labels:
                print("  %s:" % lbl)
            note = ("   ; " + ins.comment) if ins.comment else ""
            print("    %5d  %s%s" % (ins.line, ins, note))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
