/*=============================================================================
	NullDrvPrivate.h: Null render device (GPU-free) private header.
	ufront: a no-op URenderDevice for a headless / "poor-man's dedicated server" run.
=============================================================================*/

#include "RenderPrivate.h"

/*------------------------------------------------------------------------------------
	UNullRenderDevice.
------------------------------------------------------------------------------------*/

//
// ufront: a null render device — implements the v200 URenderDevice interface with no-op draw calls and no
// GL context. Selected via -NULLRENDERER. The NSDL viewport decides GL-vs-software purely from the render
// device NAME containing "OPENGL" (UNSDLViewport::OpenWindow, before Init runs). "NullDrv.NullRenderDevice"
// lacks "OPENGL", so the viewport creates an SDL_Texture software surface and NO WebGL context — the game
// runs GPU-free. Mirrors UT2004's Engine.NullRenderDevice (UE2.5) for the older UE1 immediate-mode interface,
// and engine-ut99's NullDrv for the v400 interface.
//
class DLL_EXPORT UNullRenderDevice : public URenderDevice
{
	DECLARE_CLASS(UNullRenderDevice,URenderDevice,CLASS_Config)

	// Constructors.
	static void InternalClassInitializer( UClass* Class );

	// URenderDevice interface (v200). Only Init does anything (claims the viewport); every draw is a no-op.
	UBOOL Init( UViewport* InViewport );
	void Exit();
	void Flush();
	UBOOL Exec( const char* Cmd, FOutputDevice* Out );
	void Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize );
	void Unlock( UBOOL Blit );
	void DrawComplexSurface( FSceneNode* Frame, FSurfaceInfo& Surface, FSurfaceFacet& Facet );
	void DrawGouraudPolygon( FSceneNode* Frame, FTextureInfo& Info, FTransTexture** Pts, int NumPts, DWORD PolyFlags, FSpanBuffer* Span );
	void DrawTile( FSceneNode* Frame, FTextureInfo& Info, FLOAT X, FLOAT Y, FLOAT XL, FLOAT YL, FLOAT U, FLOAT V, FLOAT UL, FLOAT VL, FSpanBuffer* Span, FLOAT Z, FPlane Color, FPlane Fog, DWORD PolyFlags );
	void Draw2DLine( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FVector P1, FVector P2 );
	void Draw2DPoint( FSceneNode* Frame, FPlane Color, DWORD LineFlags, FLOAT X1, FLOAT Y1, FLOAT X2, FLOAT Y2 );
	void ClearZ( FSceneNode* Frame );
	void PushHit( const BYTE* Data, INT Count );
	void PopHit( INT Count, UBOOL bForce );
	void GetStats( char* Result );
	void ReadPixels( FColor* Pixels );
};

/*------------------------------------------------------------------------------------
	The End.
------------------------------------------------------------------------------------*/
