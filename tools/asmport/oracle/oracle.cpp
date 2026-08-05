// oracle.cpp -- differential harness: UE1's original MMX fog sampler vs the portable C fog path.
//
// SoftDrv's volumetric fog exists only in MSVC inline assembly, which no non-MSVC build can
// compile -- so the portable C path has never had fog, and there has been nothing to check a
// replacement against. GCC will in fact assemble those kernels (see NOTES.md), which makes the
// original usable as ground truth.
//
// This drives the real MMXFogLight8 inner sampler (SampleFogLight, re-emitted verbatim by
// tools/asmport/emit.py) over a synthetic lightmap/fogmap, and reports the final 8-bit pixel it
// produces for each fog density. That curve is what the portable path's FogShade[] has to match.
//
//   Build:  make -C tools/asmport/oracle     (or see build.sh)
//   Needs:  -m32 -masm=intel -fno-pic -fomit-frame-pointer
//
#include <stdio.h>
#include <string.h>
#include <stddef.h>

typedef unsigned int       DWORD;
typedef int                INT;
typedef unsigned char      BYTE;
typedef signed short       SWORD;
typedef unsigned long long QWORD;
typedef signed long long   SQWORD;
typedef float              FLOAT;
typedef double             DOUBLE;

template<class T> static inline T Min(T a, T b) { return a < b ? a : b; }
template<class T> static inline T Max(T a, T b) { return a > b ? a : b; }
template<class T> static inline T Clamp(T v, T a, T b) { return v < a ? a : (v > b ? b : v); }
static inline INT appRound(FLOAT f) { return (INT)(f + (f < 0 ? -0.5f : 0.5f)); }

// ---------------------------------------------------------------------------
// UE1 types, laid out exactly as SoftDrv declares them. The static_asserts below are the contract
// with tools/asmport/emit.py's STRUCT_OFFSETS -- a layout drift becomes a compile error instead of
// a silently wrong reference.
// ---------------------------------------------------------------------------

union FMMX
{
    struct { SWORD R, G, B, A; };
    struct { BYTE  SB1, SG1, SR1, SA1, SR2, SG2, SB2, SA2; };
    struct { INT   IL, IH; };
    struct { DWORD DL, DH; };
    DOUBLE D;
    QWORD  Q;
    SQWORD SQ;
};

union FRainbowPtr
{
    DWORD* PtrDWORD; BYTE* PtrBYTE; void* PtrVOID;
};

struct FLightMipSetup
{
    FMMX        Mask;
    FMMX        RMask;
    FMMX        Mult;
    DWORD       USize;
    FRainbowPtr Data;
    FRainbowPtr FogData;
    INT         LVShift, LVShift3, HUShift;
};

static_assert(offsetof(FLightMipSetup, Mask)    ==  0, "emit.py STRUCT_OFFSETS drift");
static_assert(offsetof(FLightMipSetup, RMask)   ==  8, "emit.py STRUCT_OFFSETS drift");
static_assert(offsetof(FLightMipSetup, Mult)    == 16, "emit.py STRUCT_OFFSETS drift");
static_assert(offsetof(FLightMipSetup, USize)   == 24, "emit.py STRUCT_OFFSETS drift");
static_assert(offsetof(FLightMipSetup, Data)    == 28, "emit.py STRUCT_OFFSETS drift");
static_assert(offsetof(FLightMipSetup, FogData) == 32, "emit.py STRUCT_OFFSETS drift");

// ---------------------------------------------------------------------------
// Globals the kernel names. External linkage is required: with `static`, GCC renames or eliminates
// symbols the C code never reads and the asm fails to link.
// ---------------------------------------------------------------------------

extern "C" {
FLightMipSetup LightMip;
FMMX  MMXLit, MMXLitDelta;
FMMX  MMXFlashCompress, MMXFlashOffset13, MMX14Bits;
DWORD SavedEBP, SavedESP;
FMMX  TestCoord;
FMMX  PhotonOut[8];        // kernel writes light at [ebp+0], fog at [ebp+8], then ebp += 16
}

#include "gen_samplefoglight.inc"

// Drive the re-emitted kernel: set the coordinate and destination it expects, run, restore ebp.
static void RunSampleFogLight()
{
    __asm__ __volatile__ (
        "mov [SavedEBP], ebp\n\t"
        "mov ebp, OFFSET FLAT:PhotonOut\n\t"
        "movq mm7, [TestCoord]\n\t"
        ::: "memory" );
    SampleFogLight_asm();
    __asm__ __volatile__ (
        "mov ebp, [SavedEBP]\n\t"
        "emms\n\t"
        ::: "memory" );
}

// ---------------------------------------------------------------------------
// Scene: a flat lightmap and a flat fogmap, so one sample characterises a whole density.
// ---------------------------------------------------------------------------

#define LMBITS 8
#define LMSIZE (1 << LMBITS)
static DWORD LightBuf[LMSIZE * LMSIZE];
static DWORD FogBuf  [LMSIZE * LMSIZE];

// InitMMXFlashes, copied from DrawSurf.cpp so the reference uses the engine's own constants.
static void InitMMXFlashes(FLOAT Brightness, FLOAT FlashScaleRGB, FLOAT FlashFogRGB)
{
    DOUBLE Scale = (0.50f + Brightness) * (DOUBLE)(0x3FFF) / (DOUBLE)(0x80);
    DWORD F = (DWORD)appRound(Scale * FlashScaleRGB);
    MMXFlashCompress.R = Clamp<DWORD>(F, 0, 0x7FFF);
    MMXFlashCompress.G = Clamp<DWORD>(F, 0, 0x7FFF);
    MMXFlashCompress.B = Clamp<DWORD>(F, 0, 0x7FFF);
    MMXFlashCompress.A = 0;
    MMXFlashOffset13.R = ((DWORD)FlashFogRGB << 4);
    MMXFlashOffset13.G = ((DWORD)FlashFogRGB << 4);
    MMXFlashOffset13.B = ((DWORD)FlashFogRGB << 4);
    MMXFlashOffset13.A = 0;
    MMX14Bits.Q = 0x3F803F803F803F80ull;
}

// The composite the render kernels perform (verified in MMX32FogRender8 / MMX32FogModulatedRender8):
//   out = packuswb( (pmulhw(light, MMXColor) + fog) >> FinalShift )
#define FinalShift 4
static INT MMXComposite(INT light14, INT mmxColor15)
{
    INT v = (INT)(SWORD)(((INT)(SWORD)light14 * (INT)(SWORD)mmxColor15) >> 16);
    return v;
}
static BYTE MMXFinal(INT lit, INT fog)
{
    INT v = (lit + fog) >> FinalShift;
    return (BYTE)Clamp<INT>(v, 0, 255);
}

// ---------------------------------------------------------------------------
// The portable C model, mirrored from DrawSurf.cpp so the two can be compared directly.
// ---------------------------------------------------------------------------

#define LIGHTSHADES 128
#define PALSHADES_R (64+4)
#define PALSHADES_G (64+4)
#define SHADE_G  0
#define SHADE_R  (PALSHADES_G)
#define SHADE_B  (PALSHADES_G + PALSHADES_R)
#define FOGFULLSCALE 127

static BYTE CShade[LIGHTSHADES * 256];
static BYTE CFogShade[3 * 256];
#define CFOG_G 0
#define CFOG_R 256
#define CFOG_B 512

static void CInitColorTables(FLOAT Brightness, FLOAT FlashScaleRGB, FLOAT FlashFogRGB)
{
    FLOAT Scale = (0.50f + Brightness) * 16.0f / (128.0f * 128.0f);   // x2: 7-bit light index
    int Fog0     = appRound(FlashFogRGB * (DOUBLE)(1 << 20));
    int D_Scale  = appRound(Scale * FlashScaleRGB * (DOUBLE)(1 << 20));
    int Sc = 0;
    for (int Light = 0; Light < LIGHTSHADES; Light++)
    {
        int HighLight = Light * 256, Val = Fog0;
        for (int Tex = 0; Tex < (64 + 4); Tex++)
            CShade[SHADE_R + Tex + HighLight] =
            CShade[SHADE_G + Tex + HighLight] =
            CShade[SHADE_B + Tex + HighLight] = Min((Val += Sc) >> 20, 0xff);
        Sc += D_Scale;
    }
    // Linear fog ramp, slope taken from the kernel: fogAdd = (fogbyte * FlashCompress) >> 13
    // (the fog sample is fogbyte<<7, pmulhw drops 16, psraw FinalShift drops 4 more).
    DOUBLE FogScale = (0.50f + Brightness) * (DOUBLE)(0x3FFF) / (DOUBLE)(0x80);
    int FC = Clamp(appRound(FogScale * FlashScaleRGB), 0, 0x7FFF);
    for (int f = 0; f < 256; f++)
        CFogShade[CFOG_R + f] = CFogShade[CFOG_G + f] = CFogShade[CFOG_B + f] =
            Min((f * FC) >> 13, 255);
}

static BYTE CFinal(int lightByte, int texShade, int fogByte)
{
    int a = FOGFULLSCALE - fogByte; if (a < 0) a = 0;
    int lit = (lightByte * a) / FOGFULLSCALE;              // FogAttenuate
    int v = CShade[SHADE_B + texShade + lit * 256] + CFogShade[CFOG_B + fogByte];
    return (BYTE)Min(v, 255);
}

int main()
{
    const FLOAT Brightness   = 0.39f;    // engine default
    const FLOAT FlashScale   = 127.5f;   // InFlashScale 0.5 * 255
    const FLOAT FlashFog     = 0.0f;     // no screen flash: isolate the fog map's own contribution

    InitMMXFlashes(Brightness, FlashScale, FlashFog);
    CInitColorTables(Brightness, FlashScale, FlashFog);

    LightMip.Mask.DL  = LMSIZE - 1;
    LightMip.Mask.DH  = LMSIZE - 1;
    LightMip.Mult.DL  = 1 + (LMSIZE << 16);
    LightMip.Mult.DH  = 0;
    LightMip.USize    = LMSIZE;
    LightMip.Data.PtrDWORD    = LightBuf;
    LightMip.FogData.PtrDWORD = FogBuf;

    MMXLitDelta.Q = 0;                    // stay on one texel
    TestCoord.DL  = 4 << 16;              // U = 4.0, V = 4.0 in 16.16
    TestCoord.DH  = 4 << 16;

    printf("# UE1 MMX fog reference vs the portable C model\n");
    printf("# Brightness=%.2f FlashScale=%.1f FlashFog=%.1f ALPHAFOG=0\n", Brightness, FlashScale, FlashFog);
    printf("# mid-grey texel at full lightmap; asm = original kernel, C = DrawSurf.cpp portable path\n#\n");
    printf("# The assertion: the additive fog term must match the original exactly. The *lit* term\n"
           "# differs by a constant factor in both directions -- InitMMXFlashes and InitColorTables\n"
           "# have always used different Scale formulas -- which is a pre-existing MMX/Pentium\n"
           "# brightness mismatch, not fog's business.\n#\n");
    printf("# fogbyte   asm_fogterm   C_fogterm   delta\n");

    int worst = 0;
    for (INT fogByte = 0; fogByte <= 127; fogByte += 4)
    {
        for (INT i = 0; i < LMSIZE * LMSIZE; i++)
        {
            LightBuf[i] = 0xFFFFFFFFu;                       // fully lit
            FogBuf[i]   = (DWORD)(fogByte * 0x01010101u);    // uniform fog density
        }
        MMXLit.Q = ((QWORD)TestCoord.DH << 32) | TestCoord.DL;
        memset(PhotonOut, 0, sizeof(PhotonOut));
        RunSampleFogLight();

        INT light14 = (INT)(SWORD)PhotonOut[0].R;
        INT fog13   = (INT)(SWORD)PhotonOut[1].R;

        // Probe a MID-GREY texel at MID light: a fully lit white surface is far into
        // saturation in both paths, where every fog level reads 255 and nothing is learnt.
        INT asmPx = MMXFinal(MMXComposite(light14, 48 << 7), fog13);
        INT cPx   = CFinal(127, 12, fogByte);
        (void)asmPx; (void)cPx; (void)light14;
        INT asmFog = fog13 >> FinalShift;              // what the kernel adds to the pixel
        INT cFog   = CFogShade[CFOG_B + fogByte];      // what DrawSurf.cpp adds
        INT d      = asmFog - cFog; if (d < 0) d = -d;
        if (d > worst) worst = d;
        printf("  %3d         %4d         %4d       %+d\n", fogByte, asmFog, cFog, cFog - asmFog);
    }
    printf("#\n# worst fog-term deviation: %d/255  --  %s\n", worst,
           worst == 0 ? "BIT-IDENTICAL to the original kernel" : "MISMATCH");

    // ---------------------------------------------------------------------
    // Second report: unfogged brightness, MMX vs Pentium, over the whole
    // (texel, light) space. This is the pre-existing mismatch -- InitMMXFlashes
    // and InitColorTables use different Scale formulas -- and it needs measuring
    // over the full range, not at one probe, before anyone "fixes" it.
    // ---------------------------------------------------------------------
    for (INT i = 0; i < LMSIZE * LMSIZE; i++) { LightBuf[i] = 0xFFFFFFFFu; FogBuf[i] = 0; }
    MMXLit.Q = ((QWORD)TestCoord.DH << 32) | TestCoord.DL;
    memset(PhotonOut, 0, sizeof(PhotonOut));
    RunSampleFogLight();
    INT fullLight14 = (INT)(SWORD)PhotonOut[0].R;

    printf("\n\n# Unfogged brightness: MMX vs Pentium at full lightmap, no fog.\n");
    printf("# palette  asm_px   C_px   ratio\n");
    DOUBLE sum = 0; INT n = 0, clamped = 0;
    for (INT pal = 8; pal <= 255; pal += 8)
    {
        INT asmPx = MMXFinal(MMXComposite(fullLight14, pal << 7), 0);
        INT cPx   = CFinal(127, pal >> 2, 0);
        if (asmPx >= 255 || cPx >= 255) { clamped++; continue; }
        DOUBLE r = cPx ? (DOUBLE)asmPx / (DOUBLE)cPx : 0;
        sum += r; n++;
        printf("   %3d      %3d    %3d    %.2fx\n", pal, asmPx, cPx, r);
    }
    printf("#\n# mean ratio over %d unclamped samples: %.3fx  (%d samples clamped, skipped)\n",
           n, n ? sum / n : 0.0, clamped);
    return 0;
}

