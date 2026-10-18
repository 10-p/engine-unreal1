# asmport — tools for reading SoftDrv's MMX kernels

`Source/SoftDrv/Src/{DrawSurf,DrawPoly,DrawTile}.cpp` hold x86/MMX kernels in MSVC `__asm` blocks
behind `#if ASM`.  Non-MSVC builds set `ASM 0` (`Source/Core/Inc/UnGcc.h`), so the renderer runs
its portable "Pentium" C path instead.  These tools decode the kernels' arithmetic, and run one of
them, so the C path's volumetric fog and texture smoothing can match the MMX kernels.

The scripts are Python 3, standard library only, and can be run from any directory.

## extract.py

Finds the functions that contain `__asm` and decodes them into an instruction IR.

    python3 extract.py --list  <file.cpp>              # functions containing asm
    python3 extract.py         <file.cpp> [fn ...]     # decoded instruction dump
    python3 extract.py --json  <file.cpp> [fn ...]     # IR as JSON

It handles the MSVC dialect the sources use: struct-relative operands (`[eax]FTexSetup.X`),
`offset sym`, sized symbol references (`[DWORD PTR LightOS+12]`), bare-symbol memory operands
(`paddw mm3, MMXFlashOffset13`), `;`, `//` and `/* */` comments, preprocessor lines inside `__asm`
blocks, and the one-instruction `__asm emms;` form.  Function boundaries come from brace counting
plus the sources' column-0 `}` style; check that the `--list` line ranges look sane before trusting
a dump.

## trace.py

    python3 trace.py --summary <file.cpp> <fn ...>     # immediates by opcode, symbols, opcode mix
    python3 trace.py --blocks  <file.cpp> <fn ...>     # control-flow skeleton
    python3 trace.py           <file.cpp> <fn ...>     # symbolic trace

Rewrites each instruction as an assignment naming a portable helper (`add_i16x4`, `mulhi_i16x4`,
`shr64`, `packus_i16_to_u8`, ...), spilling long expressions into numbered temporaries.  The output
is a reading aid, not compilable code.  `--summary` groups every immediate by the opcode that
consumed it, which is the quickest way to find shift amounts and biases.

## emit.py

    python3 emit.py <file.cpp> <fn> [...]              # one GCC inline-asm C function per kernel
    python3 emit.py <file.cpp> <fn> --sub <label>      # only the local subroutine at <label>
    python3 emit.py --offsets                          # the struct offsets it substitutes

Re-emits a kernel as GCC inline assembly (GAS intel syntax): struct-relative operands become
numeric offsets from `STRUCT_OFFSETS`, `#if ALPHAFOG` arms are resolved (`--alphafog <n>`, default
0 as in `DrawSurf.cpp`), labels are prefixed per kernel, and a `--sub` slice drops its trailing
`ret`.  The generated code needs:

    -m32                  the kernels are 32-bit throughout
    -masm=intel           else GCC substitutes AT&T operands into an intel block
    -fno-pic              ebx is the PIC register in 32-bit PIC; the kernels use it freely
    -fomit-frame-pointer  ebp is a general register here (saved/restored via SavedEBP)
    external linkage      on every global the asm names, or GCC renames/eliminates it

## oracle/

    tools/asmport/oracle/build.sh

regenerates `oracle/gen_samplefoglight.inc` (`emit.py ... MMXFogLight8 --sub SampleFogLight
--alphafog 0`), compiles `oracle/oracle.cpp` with the flags above and runs it.  `oracle.cpp` drives
the re-emitted `SampleFogLight` over a flat synthetic lightmap and fogmap and prints two reports:

- for fog bytes 0..127, the additive fog term of the kernel next to the portable model's
  `FogShade[]` value, their difference, and the worst deviation;
- the unfogged MMX vs Pentium brightness ratio at full lightmap for palette values 8..248, and the
  mean ratio (`InitMMXFlashes` and `InitColorTables` use different `Scale` formulas).

`oracle.cpp` static_asserts the `FLightMipSetup` offsets that `emit.py` substitutes, so a layout
drift is a compile error.

## Kernel arithmetic the C path follows

**Fog** (`MMXFogLight8`, `SampleFogLight`, `MMX32FogRender8`).  Fog and light are sampled by the
same bilinear code from `LightMip.FogData` and `LightMip.Data`; `FLightManager` builds both maps
from the same `Index->UClamp/VClamp/Pan/UScale`, so one coordinate walk serves both.  Per sample,
with `MMX14Bits = 0x3F80` (`127 << 7`) per lane:

    light = mulhi_i16x4(bilerp(LightData), MMXFlashCompress)
    light = mulhi_i16x4(light, 0x3F80 - bilerp(FogData)) << 4                   # 14-bit scale
    fog   = mulhi_i16x4(bilerp(FogData), MMXFlashCompress) + MMXFlashOffset13   # 13-bit scale
    out   = packus_i16_to_u8((mulhi_i16x4(light, MMXColors[texel]) + fog) >> FinalShift)   # FinalShift = 4

Fog is an alpha blend: it attenuates the lit texel by `(127 - fogbyte) / 127` (full density is fog
byte 127, not 255) and adds `(fogbyte * FlashCompress) >> 13`.  `MMXFlashCompress` and
`MMXFlashOffset13` carry brightness and screen flash, which the C path already folds into `Shade[]`
(`D_Scale*` and `FogR/G/B` in `InitColorTables`), so the C fog term carries the fog map only.

**Texture smoothing** (`MMX32Render8A/B`, `InitBilinearKernel`).  A 2x2 knight's-move coordinate
dither: the sample position is jittered per pixel and one texel is fetched.  The offset index is
`(X&1) | ((Y&1) << 1)` over `{A0, A1, B0, B1}` (X parity from `test esi,1`, Y parity from the
dispatcher's `if (YR & 1) MMX32Render8A(); else MMX32Render8B();`).  The offsets, in 1/16 texel
(`FuzzFracBits = 4`), are `A = (-4,-8)`, `B = (-8,0)`, `C = (0,+4)`, `D = (+4,-4)`.  The Pentium
hybrid packing (`InitTexPentium`) masks V's fraction away, so the C filtered passes apply the dither
in plain 16.16 U/V instead, scaled with the mip: `(sixteenths << 12) << iMip`.
