/*=============================================================================
	SoftDrvPrivate.h: Rendering package private header.
	Copyright 1997 Epic MegaGames, Inc.
=============================================================================*/

/*------------------------------------------------------------------------------------
	Dependencies.
------------------------------------------------------------------------------------*/

#include "RenderPrivate.h"

// ufront 2.33 (Windows/MinGW): the Win32 headers reached through the chain above define IN and OUT as
// EMPTY parameter-annotation macros, which silently deletes any local named `IN` — DrawSurf.cpp's
// bilinear filter has one, and the error it produces ("expected unqualified-id before '='") points at the
// assignment rather than at the macro. Undefining is preferable to renaming the local: it is
// preprocessor-only, so the Linux and web builds are textually and byte-for-byte unaffected. Nothing in
// this engine uses the annotations.
#ifdef PLATFORM_WIN32
#undef IN
#undef OUT
#endif

/*------------------------------------------------------------------------------------
	Software rendering private definitions.
------------------------------------------------------------------------------------*/

// If ASM is disabled but we're on x86, also force-disable MMX.
#if !ASM && __INTEL__
#define GIsMMX false
#endif

// Maximum supported sizes.
// ufront 2.12: was 1200 x 2048. The software rasterizer's per-scanline arrays (DrawPoly Setup/MMXSetup/
// SpanIndex, DrawTile SpanIndex) are indexed by Viewport->SizeY, and DrawTile had NO guard, so at SizeY>1200
// (1440p/4K — reachable since the 2.12 dynamic-resolution work) it overran the span table into a self-
// referential FSpan -> the tile blitter's for(Span;Span;Span=Span->Next) walk hung the main thread (the web
// tab-freeze; music stopped, WebAudio ambient kept looping). Raised to cover 4K in any orientation; DrawTile
// now also clamps its fill to this. (fgsfdsfgs/UE1 dropped UT99's SoftDrv::SetRes SizeY clamp; this restores
// a safe ceiling.) Above the ceiling the software path fail-soft skips (no hang). X bump also fixes the
// DrawSurf Photon "crashes above 1600" note.
// ufront 2.19f: overridable at build time (-DSOFTDRV_MAX_SCREEN=n), because these are the bounds of
// STATIC arrays — they cost their full size in .bss at every resolution, not just at the ceiling.
// Measured at 4096: Setup + MMXSetup + Photon + UCoordTable + two SpanIndex = 368 KB resident whether
// the viewport is 4K or 240x240; at a 256 ceiling the same tables cost 23 KB. Lowering it is fail-soft
// (SetRes clamps X, DrawPoly skips above the Y ceiling, DrawTile clamps its span-fill), not a landmine.
#ifndef SOFTDRV_MAX_SCREEN
#define SOFTDRV_MAX_SCREEN 4096
#endif
#define MaximumYScreenSize  SOFTDRV_MAX_SCREEN
#define MaximumXScreenSize  SOFTDRV_MAX_SCREEN


// Coarse fast square root approximation
#define FASTAPPROX_MAN_BITS 8   /* Number of bits of approximate square root mantissa, <=23 */
extern FLOAT FastSqrtTbl[2<<FASTAPPROX_MAN_BITS];

void SetupFastSqrt();

//
// _rotl and _rotr alternatives for non-MSVC platforms.
//
#ifdef PLATFORM_WIN32
#include <stdlib.h> // for _rotl and _rotr
#elif defined(PLATFORM_X86)
#include <x86intrin.h> // for _rotl and _rotr
#else
// no intrinsics or only rotr
inline unsigned int _rotl( unsigned int a, int s )
{
	s &= 31;
	if( s == 0 ) return a;
	return (a << s) | (a >> (32 - s));
}
inline unsigned int _rotr( unsigned int a, int s )
{
	s &= 31;
	if( s == 0 ) return a;
	return (a >> s) | (a << (32 - s));
}
#endif

//
// An 8-byte MMX structure.
//

union FMMX
{
	struct
	{
#if __INTEL_BYTE_ORDER__
		SWORD R, G, B, A;
#else
		SWORD A, B, G, R;
#endif
	};

	struct
	{
#if __INTEL_BYTE_ORDER__
		BYTE R1, G1, B1, A1, R2, G2, B2, A2;
#else
		BYTE A2, B2, G2, R2, A1, B1, G1, R1;
#endif
	};

	struct
	{
#if __INTEL_BYTE_ORDER__
		BYTE SB1, SG1, SR1, SA1, SR2, SG2, SB2, SA2;
#else
		BYTE SA2, SR2, SG2, SB2, SA1, SB1, SG1, SR1;
#endif
	};

	struct
	{
#if __INTEL_BYTE_ORDER__
		INT IL, IH;
#else
		INT IH, IL;
#endif
	};
	struct
	{
#if __INTEL_BYTE_ORDER__
		DWORD DL, DH;
#else
		DWORD DH, DL;
#endif
	};
	struct
	{
#if __INTEL_BYTE_ORDER__
		FLOAT FL, FH;
#else
		DWORD FH, FL;
#endif
	};
	DOUBLE D;
	QWORD  Q;
	SQWORD SQ;
};


// ColorMode device constants.
enum {
		MMX15,
		MMX16,
		MMXReserved,
		MMX32,
		Pentium15,
		Pentium16,
		PentiumReserved,
		Pentium32
};

// RenderMode constants.
enum {
		DRAWNORMAL,
		DRAWTRANSLUCENT,
		DRAWMODULATED,
		DRAWSTIPPLED,
		DRAWMASKED,
		DRAWRESERVED1,
		DRAWRESERVED2
};


//
// A 3D rendering device.
//
class DLL_EXPORT USoftwareRenderDevice : public URenderDevice
{
	DECLARE_CLASS(USoftwareRenderDevice,URenderDevice,CLASS_Config)

	// Temp statistics.
	INT SurfPalBuilds;
	// Which filtering to use when HighRes/LowResTextureSmooth have already enabled it for this
	// resolution: False = the original 2x2 knight's-move coordinate dither (one texel, ~free),
	// True = true bilinear (four texels and a blend). Smoothing off is point sampling either way,
	// so this knob only ever picks between the two filtered modes.
	UBOOL			BilinearFiltering;

	// Preference variables.
	UBOOL FastTranslucency;
	UBOOL HighResTextureSmooth;
	UBOOL LowResTextureSmooth;
	FLOAT DetailBias;
	UBOOL RAM32Mode;

	// Current texture smoothing status (per-frame)
	UBOOL TextureSmooth;


	// Master Screen Destination variables.
	// Variables.
	DWORD RenderMode;  // 
	DWORD ColorMode;   //

	INT FrameLocksCounter;
	INT GByteStride;
	FVector FlashScale, FlashFog;
	FLOAT  PerspError;
	FLOAT  GMasterScale;
	FVector GFloatScale, GFloatFog, GMaxColor, GFloatRange;
	TArray<BYTE> HitStack;
	BYTE* HitData;
	INT* HitSize;
	INT HitCount;
	

	// URenderDevice interface.
	static void InternalClassInitializer( UClass* Class );
	UBOOL Init( UViewport* InViewport );
	void Exit();
	void Flush();
	UBOOL Exec( const char* Cmd, FOutputDevice* Out );
	void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* InHitData, INT* InHitSize );
	void Unlock( UBOOL Blit );
	void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet );
	void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Texture, FTransTexture** Pts, INT NumPts, DWORD PolyFlags, FSpanBuffer* SpanBuffer );
	void DrawTile( FSceneNode* Frame, FTextureInfo& Texture, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Light, FPlane Fog, DWORD PolyFlags );
	void GetStats( char* Result );
	void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 );
	void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 );
	void PushHit( const BYTE* Data, INT Count );
	void PopHit( INT Count, UBOOL bForce );
	void ReadPixels( FColor* Pixels );
	void ClearZ( FSceneNode* Frame ){};

private:
	// USoftwareRenderDevice interface.
	void InitDrawSurf();

	void MMXFlashTriangle( FSceneNode* Frame, FTextureInfo& Texture, DWORD PolyFlags, FSpanBuffer* SpanBuffer );
	void PentiumFlashTriangle( FSceneNode* Frame, FTextureInfo& Texture, DWORD PolyFlags, FSpanBuffer* SpanBuffer );

	void MMXDrawRectangle( FSceneNode* Node, FTextureInfo& Texture,FTransTexture* Pts, DWORD PolyFlags, FSpanBuffer* SpanBuffer );
	void PentiumDrawRectangle( FSceneNode* Node, FTextureInfo& Texture,FTransTexture* Pts, DWORD PolyFlags, FSpanBuffer* SpanBuffer );
	void InitMMXFlashes( FLOAT Brightness, INT ColorBytes, DWORD Caps);
	void InitColorTables( FLOAT Brightness, INT ColorBytes, DWORD Caps);
	void InitDepthFogTable();

	void InnerGouraudMMX32(DWORD PolyFlags, INT MinY, INT MaxY, FSceneNode* Frame,FMipmap* Mip,FSpanBuffer* SpanBuffer);
    void InnerGouraudMMX16(DWORD PolyFlags, INT MinY, INT MaxY, FSceneNode* Frame,FMipmap* Mip,FSpanBuffer* SpanBuffer);
	void InnerGouraudMMX15(DWORD PolyFlags, INT MinY, INT MaxY, FSceneNode* Frame,FMipmap* Mip,FSpanBuffer* SpanBuffer);

	void BlitTile32(FSpanBuffer* Span);
	void BlitMask32(FSpanBuffer* Span);
	void BlitTile1516(FSpanBuffer* Span);
	void BlitMask1516(FSpanBuffer* Span);

	void ClearScreenFast16(_WORD* Dest, DWORD Color);
	void ClearScreenFast32(DWORD* Dest, DWORD Color);

	void RenderSurfSpansMMX32( FRainbowPtr& DestPtr, FSurfaceInfo& Surface,INT TaskStartY, INT TaskEndY);
	void RenderSurfSpansMMX15( FRainbowPtr& DestPtr, FSurfaceInfo& Surface,INT TaskStartY, INT TaskEndY);
	void RenderSurfSpansMMX16( FRainbowPtr& DestPtr, FSurfaceInfo& Surface,INT TaskStartY, INT TaskEndY);
};




#if ASM

//
// Fast floating point power routines.
// Made coarse but fast for per-span subdivision requirement determination....
// About 12 cycles on the Pentium.
//

inline FLOAT FastSqrt(FLOAT F)
{
	__asm
	{
		mov  eax,[F]                        // get float as int.
		shr  eax,(23 - FASTAPPROX_MAN_BITS) - 2 // shift away unused low mantissa.
		mov  ebx,[F]						// get float as int.
		and  eax, ((1 << (FASTAPPROX_MAN_BITS+1) )-1) << 2 // 2 to avoid "[eax*4]".
		and  ebx, 0x7F000000				// 7 bit exp., wipe low bit+sign.
		shr  ebx, 1							// exponent/2.
		mov  eax,DWORD PTR FastSqrtTbl [eax]	// index hi bit is exp. low bit.
		add  eax,ebx						// recombine with exponent.
		mov  [F],eax						// store.
	}
	return F;								// compiles to fld [F].
}

#else

inline FLOAT FastSqrt(FLOAT F) {return appSqrt(F);}

#endif


/*------------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------------*/
