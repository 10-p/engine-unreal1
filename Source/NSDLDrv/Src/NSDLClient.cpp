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
		new(Class, "UseMouse",          RF_Public)UBoolProperty(CPP_PROPERTY(UseMouse),          "Display",  CPF_Config );
		new(Class, "TouchPad",          RF_Public)UBoolProperty(CPP_PROPERTY(TouchPad),          "Joystick", CPF_Config );
		new(Class, "DeadZoneXYZ",       RF_Public)UFloatProperty(CPP_PROPERTY(DeadZoneXYZ),      "Joystick", CPF_Config );
		new(Class, "DeadZoneRUV",       RF_Public)UFloatProperty(CPP_PROPERTY(DeadZoneRUV),      "Joystick", CPF_Config );
		new(Class, "ScaleXYZ",          RF_Public)UFloatProperty(CPP_PROPERTY(ScaleXYZ),         "Joystick", CPF_Config );
		new(Class, "ScaleRUV",          RF_Public)UFloatProperty(CPP_PROPERTY(ScaleRUV),         "Joystick", CPF_Config );
		new(Class, "InvertY",           RF_Public)UBoolProperty(CPP_PROPERTY(InvertY),           "Joystick", CPF_Config );
		new(Class, "InvertV",           RF_Public)UBoolProperty(CPP_PROPERTY(InvertV),           "Joystick", CPF_Config );
		new(Class, "PadWalkTilt",       RF_Public)UFloatProperty(CPP_PROPERTY(PadWalkTilt),      "Joystick", CPF_Config );
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
	TouchPadJoystick = NULL;
	DefaultDisplay = 0;
	UseJoystick = true;
#if defined(__ANDROID__)
	UseMouse = false;	// a phone is played with its pads and the on-screen pad
#else
	UseMouse = true;
#endif
	TouchPad = true;	// Android only
	ScaleXYZ = 100.f;
	ScaleRUV = 100.f;
	DeadZoneXYZ = 0.1f;
	DeadZoneRUV = 0.1f;
	PadWalkTilt = 0.5f;	// the move stick walks below half its travel

	// Compiled defaults for the inherited UClient settings, for an ini with no [NSDLDrv.NSDLClient] section (a
	// retail Unreal .ini has none): UClient::InternalClassInitializer (Engine/Src/UnCamMgr.cpp) registers its
	// display options and assigns none of them. Values mirror Engine/Config/Unreal.ini; the ini still overrides.
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

	// Log what the client resolved to: UnGame.cpp passes Client->ViewportX/Y straight into OpenWindow, so
	// these values are the window.
	debugf( NAME_Init, "NSDLClient: %ix%i, Brightness=%.2f, CurvedSurfaces=%i, ScreenFlashes=%i, LowDetailTextures=%i",
		ViewportX, ViewportY, Brightness, (INT)CurvedSurfaces, (INT)ScreenFlashes, (INT)LowDetailTextures );

	Controller = NULL;

	// Without the mouse, touches are not turned into mouse events either (SDL's default does).
	if( !UseMouse )
	{
		SDL_SetHint( SDL_HINT_TOUCH_MOUSE_EVENTS, "0" );
		debugf( NAME_Init, "NSDLClient: UseMouse=False: mouse events are ignored" );
	}
#if defined(__ANDROID__)
	// The system's Back is a key (SDL_SCANCODE_AC_BACK, the game's Escape) instead of closing the activity.
	SDL_SetHint( SDL_HINT_ANDROID_TRAP_BACK_BUTTON, "1" );
	// Landscape only, either way up: SDLActivity sets the orientation from this hint when the window is created.
	SDL_SetHint( SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight" );
#endif

	// The game-controller subsystem is always up; whether a pad is used is UseJoystick, which the
	// viewport reads every tick (UNSDLViewport::TickInput), so the game's own option takes effect at once.
	if ( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER ) < 0 )
	{
		appErrorf( "SDL_Init failed: %s", SDL_GetError() );
		appExit();
	}

	atexit( SDL_Quit );

	// Open a pad already connected. Later ones arrive through SDL_CONTROLLERDEVICEADDED
	// (UNSDLViewport::TickInput).
	SDL_GameControllerEventState( SDL_ENABLE );
	if( UseJoystick )
	{
		for( INT i=0; i<SDL_NumJoysticks() && !Controller; i++ )
			OpenPad( i, NULL );
	}
	else debugf( NAME_Init, "NSDLClient: UseJoystick=False: no game pads" );

	// The on-screen pad, a virtual SDL controller (Android): after the pads present, so a real pad is the one opened.
	if( UseJoystick )
		InitTouchPad();

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

	// Close any viewport still open (`exit`, SIGTERM) while this client and SDL video are alive: a viewport's
	// Destroy removes it from Viewports, clears FullscreenViewport and shuts its render device down. Left to the
	// final purge, that would run after the engine has deleted this client.
	for( INT i=Viewports.Num()-1; i>=0; i-- )
		delete Viewports(i);

	if (Controller)
	{
		SDL_GameControllerClose(Controller);
		Controller = NULL;
	}
	ExitTouchPad();

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

//
// Whether a viewport's console is in its Menuing state (the menus are up).
//
UBOOL UNSDLClient::MenuActive()
{
	guard(UNSDLClient::MenuActive);
	for( INT i=0; i<Viewports.Num(); i++ )
		if( ((UNSDLViewport*)Viewports(i))->InMenu() )
			return 1;
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	Game pads (one open pad at a time).
-----------------------------------------------------------------------------*/

#ifdef __EMSCRIPTEN__
//
// The buttons pressed right now on the browser's first listed pad named Id (bit i = Gamepad.buttons[i].pressed,
// the W3C "standard" layout), 0 if none: the press that made the browser list the pad, which SDL's Emscripten driver
// keeps as the starting state and never reports. Runs on the page's main thread (the engine is on a worker).
// (In the EM_ASM block: no top-level commas.)
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
