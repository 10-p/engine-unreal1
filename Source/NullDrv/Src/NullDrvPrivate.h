/*=============================================================================
	NullDrvPrivate.h: Null render device (GPU-free) private header.
	A no-op URenderDevice for a headless run.
=============================================================================*/

#include "RenderPrivate.h"

/*------------------------------------------------------------------------------------
	UNullRenderDevice.
------------------------------------------------------------------------------------*/

//
// A null render device: the v200 URenderDevice interface with no-op draw calls and no GL context. Selected
// via -NULLRENDERER. The NSDL viewport decides GL or software from the configured render device name
// containing "OPENGL" (UNSDLViewport::OpenWindow, before Init runs); "NullDrv.NullRenderDevice" lacks it, so
// no GL context is created and the game runs GPU-free.
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
