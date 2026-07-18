/*=============================================================================
	NullDrv.cpp: Null (GPU-free) render device.
	ufront: a no-op URenderDevice — selected via -NULLRENDERER for a headless run (no WebGL context).
=============================================================================*/

#include "NullDrvPrivate.h"

/*-----------------------------------------------------------------------------
	Global implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_PACKAGE(NullDrv);
IMPLEMENT_CLASS(UNullRenderDevice);

/*-----------------------------------------------------------------------------
	UNullRenderDevice implementation.
-----------------------------------------------------------------------------*/

void UNullRenderDevice::InternalClassInitializer( UClass* Class )
{
	// The null device has nothing to configure (no options / no GL state).
}

UBOOL UNullRenderDevice::Init( UViewport* InViewport )
{
	guardSlow(UNullRenderDevice::Init);
	Viewport            = InViewport;
	SpanBased           = 0;
	FrameBuffered       = 0;
	SupportsFogMaps     = 0;
	SupportsDistanceFog = 0;
	// No GL context / no surface setup here: the NSDL viewport (OpenWindow) already picked the software
	// SDL_Texture surface because the device name lacks "OPENGL". We draw nothing.
	return 1;
	unguardSlow;
}

void UNullRenderDevice::Exit() {}
void UNullRenderDevice::Flush() {}
UBOOL UNullRenderDevice::Exec( const char* /*Cmd*/, FOutputDevice* /*Out*/ ) { return 0; }
void UNullRenderDevice::Lock( FPlane, FPlane, FPlane, DWORD, BYTE*, INT* ) {}
void UNullRenderDevice::Unlock( UBOOL /*Blit*/ ) {}
void UNullRenderDevice::DrawComplexSurface( FSceneNode*, FSurfaceInfo&, FSurfaceFacet& ) {}
void UNullRenderDevice::DrawGouraudPolygon( FSceneNode*, FTextureInfo&, FTransTexture**, int, DWORD, FSpanBuffer* ) {}
void UNullRenderDevice::DrawTile( FSceneNode*, FTextureInfo&, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FLOAT, FSpanBuffer*, FLOAT, FPlane, FPlane, DWORD ) {}
void UNullRenderDevice::Draw2DLine( FSceneNode*, FPlane, DWORD, FVector, FVector ) {}
void UNullRenderDevice::Draw2DPoint( FSceneNode*, FPlane, DWORD, FLOAT, FLOAT, FLOAT, FLOAT ) {}
void UNullRenderDevice::ClearZ( FSceneNode* ) {}
void UNullRenderDevice::PushHit( const BYTE*, INT ) {}
void UNullRenderDevice::PopHit( INT, UBOOL ) {}
void UNullRenderDevice::GetStats( char* Result ) { if( Result ) Result[0] = 0; }
void UNullRenderDevice::ReadPixels( FColor* ) {}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
