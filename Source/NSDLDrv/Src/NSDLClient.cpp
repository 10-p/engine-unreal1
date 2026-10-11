#include <stdlib.h>
#include <ctype.h>

#include "NSDLDrv.h"
#ifdef __EMSCRIPTEN__
#include <emscripten/em_asm.h>
#endif
#include "UnRender.h"

IMPLEMENT_CLASS( UNSDLClient );

/*-----------------------------------------------------------------------------
	UNSDLClient implementation.
-----------------------------------------------------------------------------*/

//
// Static init.
//
void UNSDLClient::InternalClassInitializer( UClass* Class )
{
	guard(UNSDLClient::InternalClassInitializer);
	if( appStricmp( Class->GetName(), "NSDLClient" ) == 0 )
	{
		new(Class, "DefaultDisplay",    RF_Public)UIntProperty(CPP_PROPERTY(DefaultDisplay),     "Display",  CPF_Config );
		new(Class, "StartupFullscreen", RF_Public)UBoolProperty(CPP_PROPERTY(StartupFullscreen), "Display",  CPF_Config );
		new(Class, "UseJoystick",       RF_Public)UBoolProperty(CPP_PROPERTY(UseJoystick),       "Joystick", CPF_Config );
		new(Class, "DeadZoneXYZ",       RF_Public)UFloatProperty(CPP_PROPERTY(DeadZoneXYZ),      "Joystick", CPF_Config );
		new(Class, "DeadZoneRUV",       RF_Public)UFloatProperty(CPP_PROPERTY(DeadZoneRUV),      "Joystick", CPF_Config );
		new(Class, "ScaleXYZ",          RF_Public)UFloatProperty(CPP_PROPERTY(ScaleXYZ),         "Joystick", CPF_Config );
		new(Class, "ScaleRUV",          RF_Public)UFloatProperty(CPP_PROPERTY(ScaleRUV),         "Joystick", CPF_Config );
		new(Class, "InvertY",           RF_Public)UBoolProperty(CPP_PROPERTY(InvertY),           "Joystick", CPF_Config );
		new(Class, "InvertV",           RF_Public)UBoolProperty(CPP_PROPERTY(InvertV),           "Joystick", CPF_Config );
	}
	unguard;
}

//
// UNSDLClient constructor.
//
UNSDLClient::UNSDLClient()
{
	guard(UNSDLClient::UWindowsClient);
	Controller = NULL;
	DefaultDisplay = 0;
	UseJoystick = true;
	ScaleXYZ = 100.f;
	ScaleRUV = 100.f;
	DeadZoneXYZ = 0.1f;
	DeadZoneRUV = 0.1f;

	// ★ COMPILED DEFAULTS FOR THE INHERITED UClient SETTINGS (ufront 2.35).
	//
	// UClient::InternalClassInitializer (Engine/Src/UnCamMgr.cpp) REGISTERS its nine display options and
	// assigns none of them, so with no [NSDLDrv.NSDLClient] ini section they all fell back to a
	// zero-initialised default: a 0x0 viewport, Brightness 0 (a black picture), CurvedSurfaces off. The six
	// lines above already establish this driver's OWN defaults the same way -- the inherited half was simply
	// never done.
	//
	// Every shipped config here does carry the section (Engine/Config/{Unreal,Default,web/Default}.ini), so
	// nothing changes for a normal install; this is the floor under an install whose ini lacks it, which is
	// exactly the state a retail Unreal .ini is in -- it has no section for a client class added decades
	// later. Values mirror Engine/Config/Unreal.ini, the native reference; ini still overrides.
	//
	// ⚠ Unreal 1 has NO decal system -- UClient here has no `Decals` member at all (it arrives in the UT99
	// v400 engine), so the decal half of this fix simply does not apply to this engine.
	ViewportX         = 800;
	ViewportY         = 600;
	Brightness        = 0.6f;
	MipFactor         = 1.0f;
	CaptureMouse      = 1;
	CurvedSurfaces    = 1;
	LowDetailTextures = 0;
	ScreenFlashes     = 1;
	NoLighting        = 0;
	StartupFullscreen = 0;
	InvertY           = 0;
	InvertV           = 0;
	unguard;
}

//
// Initialize the platform-specific viewport manager subsystem.
// Must be called after the Unreal object manager has been initialized.
// Must be called before any viewports are created.
//
void UNSDLClient::Init( UEngine* InEngine )
{
	guard(UNSDLClient::Init);

	// Init base.
	UClient::Init( InEngine );

	// ★ SAY WHAT THE CLIENT ACTUALLY RESOLVED TO (ufront 2.35). UnGame.cpp passes Client->ViewportX/Y
	// straight into OpenWindow, so these values ARE the window -- and until now nothing said what they
	// were, which is how a zero default (a 0x0 window, Brightness 0) could look like a broken renderer.
	// Same one-line-at-Init treatment as engine-ut99's NSDLClient. (Unreal 1 has no decal system, so
	// there is no Decals to report here -- that member arrives in the UT99 v400 engine.)
	debugf( NAME_Init, "NSDLClient: %ix%i, Brightness=%.2f, CurvedSurfaces=%i, ScreenFlashes=%i, LowDetailTextures=%i",
		ViewportX, ViewportY, Brightness, (INT)CurvedSurfaces, (INT)ScreenFlashes, (INT)LowDetailTextures );

	Controller = NULL;

	// ufront: UseJoystick (registered, never read before) decides whether pads are used at all.
	if ( SDL_Init( SDL_INIT_VIDEO | (UseJoystick ? SDL_INIT_GAMECONTROLLER : 0) ) < 0 )
	{
		appErrorf( "SDL_Init failed: %s", SDL_GetError() );
		appExit();
	}

	atexit( SDL_Quit );

	// A pad already connected (on the web: one pressed on the launcher page). Later ones arrive through
	// SDL_CONTROLLERDEVICEADDED (UNSDLViewport::UpdateInput): there was no hot-plug before.
	if( UseJoystick )
	{
		for( INT i=0; i<SDL_NumJoysticks() && !Controller; i++ )
			OpenPad( i, NULL );
		SDL_GameControllerEventState( SDL_ENABLE );
	}
	else debugf( NAME_Init, "NSDLClient: UseJoystick=False: no game pads" );

	// Not calling SDL_StartTextInput because that pops up on-screen keyboards sometimes.
	SDL_EventState( SDL_TEXTINPUT, SDL_ENABLE );

	SDL_GetDesktopDisplayMode( DefaultDisplay, &DefaultDisplayMode );

	unguard;
}

//
// Shut down the platform-specific viewport manager subsystem.
//
void UNSDLClient::Destroy()
{
	guard(UNSDLClient::Destroy);

	// ufront 2.84 -- close any viewport still open (`exit`, SIGTERM) while this client and SDL video are alive: a
	// viewport's Destroy removes it from Viewports, clears FullscreenViewport and shuts its render device down.
	// Left to the final purge, that ran after the engine had deleted this client (AddressSanitizer:
	// heap-use-after-free at every exit, every width).
	for( INT i=Viewports.Num()-1; i>=0; i-- )
		delete Viewports(i);

	if (Controller)
	{
		SDL_GameControllerClose(Controller);
		Controller = NULL;
	}

	SDL_QuitSubSystem( SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER );

	UClient::Destroy();

	unguard;
}

//
// Failsafe routine to shut down viewport manager subsystem
// after an error has occured. Not guarded.
//
void UNSDLClient::ShutdownAfterError()
{
	debugf( NAME_Exit, "Executing UNSDLClient::ShutdownAfterError" );

	if( Engine && Engine->Audio )
	{
		Engine->Audio->ConditionalShutdownAfterError();
	}

	for( INT i=Viewports.Num()-1; i>=0; i-- )
	{
		UNSDLViewport* Viewport = (UNSDLViewport*)Viewports(i);
	}

	Super::ShutdownAfterError();
}

//
// Enable or disable all viewport windows that have ShowFlags set (or all if ShowFlags=0).
//
void UNSDLClient::EnableViewportWindows( DWORD ShowFlags, int DoEnable )
{
	guard(UNSDLClient::EnableViewportWindows);
	unguard;
}

//
// Show or hide all viewport windows that have ShowFlags set (or all if ShowFlags=0).
//
void UNSDLClient::ShowViewportWindows( DWORD ShowFlags, int DoShow )
{
	guard(UNSDLClient::ShowViewportWindows);
	unguard;
}

//
// Configuration change.
//
void UNSDLClient::PostEditChange()
{
	guard(UNSDLClient::PostEditChange);
	Super::PostEditChange();
	unguard;
}

// Perform background processing.  Should be called 100 times
// per second or more for best results.
//
void UNSDLClient::Poll()
{
	guard(UNSDLClient::Poll);
	unguard;
}

//
// Perform timer-tick processing on all visible viewports.  This causes
// all realtime viewports, and all non-realtime viewports which have been
// updated, to be blitted.
//
void UNSDLClient::Tick()
{
	guard(UNSDLClient::Tick);

	// Process input and blit any viewports that need blitting.
	UNSDLViewport* BestViewport = NULL;
	for( INT i=0; i<Viewports.Num(); i++ )
	{
		UNSDLViewport* Viewport = CastChecked<UNSDLViewport>(Viewports(i));
		if( !Viewport->GetWindow() )
		{
			// Window was closed via close button.
			delete Viewport;
			return;
		}
		else if (	Viewport->IsRealtime()
			&&	(Viewport==FullscreenViewport || FullscreenViewport==NULL)
			&&	Viewport->SizeX && Viewport->SizeY && !Viewport->OnHold
			&&	(!BestViewport || Viewport->LastUpdateTime<BestViewport->LastUpdateTime) )
		{
			BestViewport = Viewport;
		}
		// Tick input for this viewport and see if it wants to die.
		if( Viewport->TickInput() )
		{
			delete Viewport;
			return;
		}
	}

	if( BestViewport )
		BestViewport->Repaint();

	unguard;
}

//
// Create a new viewport.
//
UViewport* UNSDLClient::NewViewport( class ULevel* InLevel, const FName Name )
{
	guard(UNSDLClient::NewViewport);
	return new( GObj.GetTransientPackage(), Name )UNSDLViewport( InLevel, this );
	unguard;
}

//
// Return the current viewport.  Returns NULL if no viewport has focus.
//
UViewport* UNSDLClient::CurrentViewport()
{
	guard(UNSDLClient::CurrentViewport);

	if( FullscreenViewport )
		return FullscreenViewport;

	SDL_Window *MouseWin = SDL_GetMouseFocus();
	UNSDLViewport* TestViewport = NULL;
	for( int i=0; i<Viewports.Num(); i++ )
	{
		TestViewport = (UNSDLViewport*)Viewports(i);
		if( TestViewport->Current || (SDL_Window*)TestViewport->GetWindow() == MouseWin )
			return TestViewport;
	}

	return NULL;

	unguard;
}

//
// Assemble display mode array.
//
static INT Compare( const SDL_Rect& A, const SDL_Rect& B )
{
	if( A.w == B.w )
		return A.h - B.h;
	return A.w - B.w;
}
static UBOOL operator==( const SDL_Rect& A, const SDL_Rect& B )
{
	return A.w == B.w && A.h == B.h;
}
const TArray<SDL_Rect>& UNSDLClient::GetDisplayResolutions()
{
	guard(UNSDLClient::GetDisplayResolutions)

	DisplayResolutions.Empty();

	SDL_DisplayMode Mode;
	SDL_Rect Rect = { 0, 0, 0, 0 };
	if( SDL_GetDesktopDisplayMode( DefaultDisplay, &Mode ) == 0 )
	{
		// we're only interested in size
		Rect.w = Mode.w;
		Rect.h = Mode.h;
		DisplayResolutions.AddItem( Rect );
	}

	const INT NumModes = SDL_GetNumDisplayModes( DefaultDisplay );
	
	for( INT i=0; i<NumModes; ++i )
	{
		if( SDL_GetDisplayMode( DefaultDisplay, i, &Mode ) == 0 )
		{
			// we're only interested in size
			Rect.w = Mode.w;
			Rect.h = Mode.h;
			DisplayResolutions.AddUniqueItem( Rect );
		}
	}

	appSort( &DisplayResolutions(0), DisplayResolutions.Num() );

	return DisplayResolutions;

	unguard;
}

//
// Command line.
//
UBOOL UNSDLClient::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UNSDLClient::Exec);

	if( ParseCommand( &Cmd, "EndFullscreen" ) )
	{
		EndFullscreen();
		return 1;
	}
	else if( ParseCommand( &Cmd, "GetRes" ) )
	{
		FString Result;
		GetDisplayResolutions();
		for( INT i=0; i<DisplayResolutions.Num(); ++i )
		{
			const SDL_Rect& Mode = DisplayResolutions(i);
			Result.Appendf( "%ix%i ", Mode.w, Mode.h );
		}
		Out->Log( *Result );
		return 1;
	}
	else if( UClient::Exec( Cmd, Out ) )
	{
		return 1;
	}

	return 0;

	unguard;
}

void UNSDLClient::EndFullscreen()
{
	UNSDLViewport* Viewport = Cast<UNSDLViewport>(FullscreenViewport);
	if( Viewport )
	{
		Viewport->EndFullscreen();
	}
	FullscreenViewport = NULL;
}

//
// Try switching to a new rendering device.
//
void UNSDLClient::TryRenderDevice( UViewport* Viewport, const char* ClassName, UBOOL Fullscreen )
{
	guard(UNSDLClient::TryRenderDevice);

	// Shut down current rendering device.
	if( Viewport->RenDev )
	{
		Viewport->RenDev->Exit();
		delete Viewport->RenDev;
		Viewport->RenDev = NULL;
	}

	// Find device driver.
	UClass* RenderClass = GObj.LoadClass( URenderDevice::StaticClass, NULL, ClassName, NULL, LOAD_KeepImports, NULL );
	if( RenderClass )
	{
		Viewport->RenDev = ConstructClassObject<URenderDevice>( RenderClass );
		if( Viewport->Client->Engine->Audio && !GIsEditor )
			Viewport->Client->Engine->Audio->SetViewport( NULL );
		if( Viewport->RenDev->Init( Viewport ) )
		{
			Viewport->Actor->XLevel->DetailChange( Viewport->RenDev->HighDetailActors );
			if( Fullscreen && !Viewport->Client->FullscreenViewport )
				Viewport->MakeFullscreen( Viewport->Client->ViewportX, Viewport->Client->ViewportY, 1 );
		}
		else
		{
			debugf( NAME_Log, LocalizeError("Failed3D") );
			delete Viewport->RenDev;
			Viewport->RenDev = NULL;
		}
		if( Viewport->Client->Engine->Audio && !GIsEditor )
			Viewport->Client->Engine->Audio->SetViewport( Viewport );
	}

	unguard;
}

/*-----------------------------------------------------------------------------
	ufront: game pads (from the UC2 port's SDL2Drv/Src/SDL2Pads.cpp, one pad here; as engine-ut99).
-----------------------------------------------------------------------------*/

#ifdef __EMSCRIPTEN__
//
// H-3: the buttons pressed right now on the browser's first listed pad named Id (bit i = Gamepad.buttons[i].pressed,
// the W3C "standard" layout), 0 if none: the press that made the browser list the pad, which SDL's Emscripten driver
// keeps as the starting state and never reports. Runs on the page's main thread (the engine is on a worker).
// (In the EM_ASM block: no top-level commas and no // comments.)
//
static DWORD WebPadPressed( const char* Id )
{
	return (DWORD)MAIN_THREAD_EM_ASM_INT({
		if (typeof navigator === 'undefined' || !navigator.getGamepads) return 0;
		var id = UTF8ToString($0);
		var pads = navigator.getGamepads();
		for (var i = 0; i < pads.length; i++) {
			var g = pads[i];
			if (!g || g.id !== id || !g.buttons) continue;
			var m = 0;
			for (var b = 0; b < g.buttons.length && b < 31; b++)
				if (g.buttons[b] && g.buttons[b].pressed) m = m | (1 << b);
			return m;
		}
		return 0;
	}, Id );
}
#endif

UBOOL UNSDLClient::OpenPad( INT DeviceIndex, DWORD* Held )
{
	guard(UNSDLClient::OpenPad);
	if( Held )
		*Held = 0;
	if( !UseJoystick || Controller || !SDL_IsGameController( DeviceIndex ) )
		return 0;
	Controller = SDL_GameControllerOpen( DeviceIndex );
	if( !Controller )
	{
		debugf( NAME_Init, "NSDLClient: game pad %i could not be opened: %s", DeviceIndex, SDL_GetError() );
		return 0;
	}
	const char* Name = SDL_GameControllerName( Controller );
	debugf( NAME_Init, "NSDLClient: game pad connected: %s", Name ? Name : "(no name)" );
#ifdef __EMSCRIPTEN__
	// SDL names the joystick by the browser's Gamepad.id.
	const char* Id = SDL_JoystickName( SDL_GameControllerGetJoystick( Controller ) );
	if( Held && Id )
		*Held = WebPadPressed( Id );
#endif
	return 1;
	unguard;
}

UBOOL UNSDLClient::ClosePad( INT InstanceId )
{
	guard(UNSDLClient::ClosePad);
	if( !Controller || SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( Controller ) ) != InstanceId )
		return 0;
	SDL_GameControllerClose( Controller );
	Controller = NULL;
	debugf( NAME_Init, "NSDLClient: game pad disconnected" );
	return 1;
	unguard;
}
