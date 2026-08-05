# asmport — reading UE1's MMX-only SoftDrv kernels

`Source/SoftDrv/Src/{DrawSurf,DrawPoly,DrawTile}.cpp` carry ~11.2k x86/MMX instructions behind
`#if ASM`.  At `ASM=0` (web, ARM, any non-MSVC target) they compile to empty stubs and
`GIsMMX` is forced false, so the renderer runs its portable "Pentium" C twins instead.

Almost all of that assembly is a *performance* alternative — every block in `DrawTile.cpp`,
`DrawPoly.cpp`, `Fire/UnFractal.cpp`, `Render/RenderPrivate.h` and `Core/UnFile.h` has a C
counterpart.  Only two *features* live exclusively in the MMX path: **fog maps** and **texture
smoothing**.  These tools exist to recover the exact arithmetic of those kernels so the portable C
path can reproduce it, rather than transliterating 17.6k lines of assembly nobody can review.

## Tools

    python3 extract.py --list  <file.cpp>              # functions containing asm
    python3 extract.py         <file.cpp> [fn ...]     # decoded instruction dump
    python3 extract.py --json  <file.cpp> [fn ...]     # IR as JSON

    python3 trace.py --summary <file.cpp> <fn ...>     # immediates by opcode, symbols, opcode mix
    python3 trace.py --blocks  <file.cpp> <fn ...>     # control-flow skeleton
    python3 trace.py           <file.cpp> <fn ...>     # symbolic trace (start here for semantics)

Python 3, standard library only.  `extract.py` handles the MSVC dialect this codebase uses:
struct-relative operands (`[eax]FTexSetup.X`), `offset sym`, sized symbol references
(`[DWORD PTR LightOS+12]`), bare-symbol memory operands (`paddw mm3, MMXFlashOffset13`), `;` and
`//` and `/* */` comments, preprocessor lines inside `__asm` blocks, and the one-instruction
`__asm emms;` form.

`trace.py` rewrites each instruction as an assignment naming a portable helper (`add_i16x4`,
`mulhi_i16x4`, `shr64`, `packus_i16_to_u8`, ...), spilling long expressions into numbered
temporaries.  It is a reading aid — the output is not compilable and is not meant to be.

`--summary` is usually the first thing to run: grouping every immediate by the opcode that consumed
it is how the fog scale and the dither offsets below fell out.

Known limitation: function boundaries come from brace counting plus this codebase's column-0 `}`
house style.  Unbalanced braces inside `#if/#else` arms could still confuse it; check `--list`
line ranges look sane before trusting a dump.

## Findings

### Fog (`MMXFogLight8` @ DrawSurf.cpp:19317, `SampleFogLight` @ :19433)

Fog and light are sampled from `LightMip.FogData` / `LightMip.Data` by the *same* bilinear code,
run twice with a different base pointer.  This works because `FLightManager` builds both maps from
the same `Index->UClamp/VClamp/Pan/UScale` (`UnLight.cpp:1710-1720` vs `1800-1812`) — identical
dimensions and pan, so one coordinate walk serves both.

Per 8-pixel sample (`:19550-19586`), with `MMX14Bits = 0x3F80` per lane (= `127 << 7`, "1.0"):

    light = bilerp(LightData)                       # i16x4 from unpacked bytes
    fog   = bilerp(FogData)
    light = mulhi_i16x4(light, MMXFlashCompress)    # :19550
    light = mulhi_i16x4(light, 0x3F80 - fog)        # :19580  "light * flashcompress * (1-fogmap)"
    Photon[X].light = light << 4                    # :19586  14-bit scale
    Photon[X].fog   = mulhi_i16x4(fog, MMXFlashCompress) + MMXFlashOffset13   # :19585  13-bit scale

The render kernels then composite (`MMX32FogRender8` @ :15479-15491):

    out = packus_i16_to_u8( (light * MMXColors[texel]) + fog >> FinalShift )     # FinalShift = 4

So fog is **not** additive-only — it is an alpha blend: fog attenuates the lit texture by
`(1 - fog)` *and* adds its own color.  A purely additive port would wash out bright surfaces and
under-fog dark ones.

**Do not double-count the flash terms.**  `MMXFlashCompress` (`:370-373`) is the same quantity the
C path folds into `Shade[]` as `D_ScaleR/G/B`, and `MMXFlashOffset13` (`:378-381`) is the same
`FlashFog` the C path bakes in as `FogR/G/B` (`InitColorTables`, `:430-436`).  The portable fog
term must carry the **fog map only**; brightness and screen flash are already in the LUT.

Portable C equivalent -- see "The oracle" below for the measured constants; the first attempt at
deriving them by reading alone got both the slope and the full-scale point wrong.

### Texture smoothing (`MMX32Render8A` @ :14236, `InitBilinearKernel` @ :510)

Not bilinear filtering — a 2x2 knight's-move *coordinate dither*.  The sample position is jittered
per pixel; no extra texels are fetched.

    movq  mm1, mm7            # copy texture coordinate
    paddd mm1, [KnightA1]     # 2x i32 lanes: high = U offset, low = V offset
    paddd mm7, mm6            # Tex += DTex
    psllq mm1, 12 ; punpckhdq ; psrlq [MMXCoShift] ; pand [MMXCoMask]   # -> texel index

Selection: `test esi,1` picks offset 0 vs 1 within a set (**X parity**), and the dispatcher picks
the `A` vs `B` kernel by **Y parity** (`if (YR & 1) MMX32Render8A(); else MMX32Render8B();`).
So the offset index is `(X&1) | ((Y&1) << 1)` over `{A0, A1, B0, B1}`.

Offsets are quarter- and half-texel steps in 1/16-texel units (`FuzzFracBits = 4`):
`A = (-4,-8)`, `B = (-8,0)`, `C = (0,+4)`, `D = (+4,-4)` (`:551-563`).

The offsets live in the *MMX* coordinate domain, which keeps V's fractional bits in the low dword
and recovers them via `psllq 12` + `punpckhdq`.  The Pentium hybrid packing does **not** — its
`Tex.DH` is `[U int | U frac | V int]` with V's fraction masked away (`InitTexPentium` :165-171,
`Mask.DH = VSize-1`), so a sub-texel V dither added to `Tex.DH` is a no-op.

Consequence for the port: do **not** try to convert the knight offsets into the hybrid domain.
Give the filtered passes a plain 16.16 `U`/`V` representation (the setup loop already has both as
plain DWORDs at the call site, `:21451`) and apply the dither there.  The same representation is
what bilinear needs anyway, so one filtered coordinate format serves both modes.  Scale the offset
with the mip: `offset_16_16 = (sixteenths << 12) << iMip`.

## The oracle (tools/asmport/oracle)

`emit.py` re-emits an extracted kernel as GCC inline assembly, so the original can be *run* and used
as ground truth instead of read.  Proven working; `oracle/build.sh` regenerates and runs it.

Requirements, all load-bearing:

    -m32                  the kernels are 32-bit throughout (the native build is already -m32)
    -masm=intel           else GCC substitutes AT&T operands into an intel block
    -fno-pic              ebx is the PIC register in 32-bit PIC; the kernels use it freely
    -fomit-frame-pointer  ebp is a general register here (saved/restored via SavedEBP)
    external linkage      on every global the asm names, or GCC renames/eliminates it

Two more gotchas the emitter handles: `#if ALPHAFOG` arms sit *inside* asm blocks (emit both and it
assembles fine and computes nonsense -- v200 is ALPHAFOG 0, v400 is 1), and `[LightMip.Mask]` is a
field of a C global that has to become `[LightMip + offset]`.  Struct offsets are asserted on the C
side, so a layout drift is a compile error rather than a silently wrong reference.

### What it established

Fog composites as `out = clamp_u8( (light*texel + fog) >> 4 )`, with:

    fog term:      fogAdd = (fogbyte * FlashCompress) >> 13
                   (sample is fogbyte<<7, pmulhw drops 16, psraw FinalShift drops 4)
    attenuation:   light *= (127 - fogbyte) / 127
                   MMX14Bits is 0x3F80 == 127<<7, so FULL DENSITY IS FOG BYTE 127, not 255

Both were wrong in the first C attempt: the additive term was derived from `Shade[]` at max texel,
which saturates, giving a step function instead of a ramp (`FogShade[63]` was 251/255); and the fog
sample was halved to 7 bits like the light, doubling the apparent density.  The corrected table is
now **bit-identical to the original kernel** over the whole density range.

### Pre-existing findings (not caused by this work)

- **The portable C light pass is ~1 LSB per channel dark.**  `LightPentium`'s assembly does
  `lea eax,[eax+esp+0x010101]` -- a round-to-nearest bias on each tap pair -- and the `#if !ASM`
  fallback omits it.  Confirmed by differential test: adding the bias makes the two bit-identical
  over 653 cases.  This affects every non-x86 UE1 build (web, ARM, UT99 native) on every lit pixel.
- **MMX and Pentium paths differ by a constant brightness factor.**  `InitMMXFlashes` uses
  `Scale = (0.5+B)*0x3FFF/0x80` while `InitColorTables` uses `Scale = (0.5+B)*8/(128*128)`; the MMX
  path comes out roughly 1.8x brighter for the same texel and light.  Long-standing, independent of
  fog, and deliberately not "fixed" here.
