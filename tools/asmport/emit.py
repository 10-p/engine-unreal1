#!/usr/bin/env python3
"""
emit.py -- re-emit extracted MSVC `__asm` blocks as GCC inline assembly (GAS intel syntax).

The 1998 MMX kernels in SoftDrv are complete, working code; they are excluded from every non-MSVC
build only because of *syntax* (`__asm{}`, Intel operands) plus `UnGcc.h` forcing `ASM 0`. GCC will
happily assemble and run them, which makes them usable as an oracle: feed the original kernel and
the portable C twin the same inputs, compare the output byte for byte.

Verified requirements (see NOTES.md):
  * -m32                      the kernels are 32-bit throughout (the native build is already -m32)
  * -masm=intel               otherwise GCC substitutes AT&T operands into an intel block
  * -fno-pic                  ebx is the PIC register in 32-bit shared objects; the kernels use it
  * -fomit-frame-pointer      ebp is used as a general register (saved/restored via SavedEBP)
  * external linkage on every global the asm names, or GCC renames or eliminates it

Operand rewriting:
  [eax]FTexSetup.X       ->  [eax + <offset>]      numeric, from STRUCT_OFFSETS below
  [eax+ 8*3]FTexSetup.X  ->  [eax + 24 + <offset>]
  offset Photon          ->  OFFSET FLAT:Photon
  DWORD PTR LightOS[eax*4] -> dword ptr [LightOS + eax*4]
  MMXFlashOffset13       ->  [MMXFlashOffset13]    bare symbol = memory operand in MSVC

Labels are prefixed per kernel so several converted kernels can coexist in one object.

Usage:
    python3 emit.py <file.cpp> <function> [...]        # emit a C function per kernel
    python3 emit.py --offsets                          # dump the struct offsets it assumes
"""

import sys

import extract

# SoftDrv's asm carries `#if ALPHAFOG` arms inline; without resolving them both would be emitted
# back to back. UE1 v200 sets ALPHAFOG 0, UT99 v400 sets it 1 -- override with --alphafog.
DEFINES = {"ALPHAFOG": 0}

# Offsets the emitter substitutes for MSVC struct-relative operands.  The generated harness carries
# a static_assert for every one of these, so a layout drift is a compile error rather than a
# silently wrong render.
STRUCT_OFFSETS = {
    # struct FTexSetup { INT X; void* CoSetup; FMMX Tex, Lit; }        -- 24 bytes (3*8)
    ("FTexSetup", "X"):        0,
    ("FTexSetup", "CoSetup"):  4,
    ("FTexSetup", "Tex"):      8,
    ("FTexSetup", "Lit"):     16,

    # struct FMipSetup { FMMX Mask, RMask, Mult, HybridShift, HybridMask; DWORD USize;
    #                    FRainbowPtr Data; FTexSetup*(*Func)(FTexSetup*);
    #                    INT TexVRotate, TexURotate, TexVScale, TexUScale, LVShift, HUShift; }
    ("FMipSetup", "Mask"):         0,
    ("FMipSetup", "RMask"):        8,
    ("FMipSetup", "Mult"):        16,
    ("FMipSetup", "HybridShift"): 24,
    ("FMipSetup", "HybridMask"):  32,
    ("FMipSetup", "USize"):       40,
    ("FMipSetup", "Data"):        44,
    ("FMipSetup", "Func"):        48,

    # struct FLightMipSetup { FMMX Mask, RMask, Mult; DWORD USize; FRainbowPtr Data, FogData; ... }
    ("FLightMipSetup", "Mask"):    0,
    ("FLightMipSetup", "RMask"):   8,
    ("FLightMipSetup", "Mult"):   16,
    ("FLightMipSetup", "USize"):  24,
    ("FLightMipSetup", "Data"):   28,
    ("FLightMipSetup", "FogData"): 32,
}

# Bare identifiers that are C globals reached as memory operands.  Anything not here and not a
# register is treated as an immediate/constant expression.
SIZE_KEYWORD = {"qword": "qword ptr", "dword": "dword ptr",
                "word": "word ptr", "byte": "byte ptr"}


def struct_offset(struct, field):
    key = (struct, field)
    if key not in STRUCT_OFFSETS:
        raise KeyError("no offset known for %s.%s -- add it to STRUCT_OFFSETS" % (struct, field))
    return STRUCT_OFFSETS[key]


GLOBAL_STRUCT_TYPE = {
    "LightMip": "FLightMipSetup",
    "Mips":     "FMipSetup",
}


def render_operand(op, labelpfx):
    if op.kind == "reg":
        return op.name
    if op.kind == "imm":
        return op.text.strip()
    if op.kind == "label":
        return labelpfx + op.name
    if op.kind == "sym":
        return "OFFSET FLAT:" + op.name

    # memory
    parts = []
    if op.sym:
        parts.append(op.sym)
    if op.field:
        # `[eax]FTexSetup.X` gives the struct name directly; `[LightMip.Mask]` names a global whose
        # type has to be looked up.
        stype = op.struct if (op.struct and op.struct not in GLOBAL_STRUCT_TYPE) \
            else GLOBAL_STRUCT_TYPE.get(op.sym or op.struct, op.struct)
        parts.append(str(struct_offset(stype, op.field)))
    if op.base:
        parts.append(op.base)
    if op.index:
        parts.append("%s*%d" % (op.index, op.scale))
    if op.disp not in (None, ""):
        parts.append(str(op.disp))

    inner = " + ".join(p for p in parts if p not in (None, ""))
    inner = inner.replace("+ -", "- ")
    pfx = SIZE_KEYWORD.get(op.size, "") if op.size else ""
    return ("%s [%s]" % (pfx, inner)).strip()


def emit_kernel(name, insns, labelpfx=None):
    """Return C source for one kernel as a GCC inline-asm function."""
    labelpfx = labelpfx or (name + "_")
    lines = []
    clobbers = set()

    for ins in insns:
        for lbl in ins.labels:
            lines.append('"%s%s:\\n\\t"' % (labelpfx, lbl))
        ops = ", ".join(render_operand(o, labelpfx) for o in ins.operands)
        text = ("%s %s" % (ins.op, ops)).strip()
        comment = ("   // " + ins.comment) if ins.comment else ""
        lines.append('"%s\\n\\t"%s' % (text, comment))
        for o in ins.operands:
            if o.kind == "reg" and o.name in extract.GPR32 and o.name not in ("esp", "ebp"):
                clobbers.add(o.name)

    clob = ",".join(sorted('"%s"' % c for c in clobbers) + ['"memory"'])

    body = "\n        ".join(lines)
    return (
        "// Re-emitted from the original MSVC __asm block -- do not hand-edit; regenerate with\n"
        "//   python3 tools/asmport/emit.py <file.cpp> %s\n"
        "static void %s_asm()\n"
        "{\n"
        "    __asm__ __volatile__ (\n"
        "        %s\n"
        "        ::: %s );\n"
        "}\n" % (name, name, body, clob)
    )


def slice_sub(insns, label):
    """Instructions from `label` up to (not including) the terminating `ret`.

    The MMX light passes keep their bilinear sampler as a local `call`/`ret` subroutine
    (SampleFogLight). Emitting just that range gives a leaf kernel that can be driven directly with
    synthetic inputs, instead of having to reconstruct the whole FTexSetup span-walker state the
    enclosing kernel expects. The trailing `ret` is dropped -- inside an inline-asm block it would
    return from the enclosing C function.
    """
    out, seen = [], False
    for ins in insns:
        if label in ins.labels:
            seen = True
        if not seen:
            continue
        if ins.op in ("ret", "retn"):
            break
        out.append(ins)
    if not out:
        raise KeyError("label %s not found" % label)
    return out


def main(argv):
    if "--offsets" in argv:
        for (s, f), off in sorted(STRUCT_OFFSETS.items()):
            print("%-22s %-14s %d" % (s, f, off))
        return 0

    if "--alphafog" in argv:
        i = argv.index("--alphafog")
        DEFINES["ALPHAFOG"] = int(argv[i + 1])
        argv = argv[:i] + argv[i + 2:]

    sub = None
    if "--sub" in argv:
        i = argv.index("--sub")
        sub = argv[i + 1]
        argv = argv[:i] + argv[i + 2:]
    argv = [a for a in argv if not a.startswith("--")]

    if len(argv) < 2:
        print(__doc__)
        return 2

    path, names = argv[0], argv[1:]
    funcs = extract.extract(path, set(names), defines=DEFINES)
    for n in names:
        if n not in funcs:
            print("!! %s: not found" % n, file=sys.stderr)
            continue
        insns = funcs[n]
        name = n
        if sub:
            insns = slice_sub(insns, sub)
            name = sub
        print(emit_kernel(name, insns))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
