#include <string.h>
#include <ctype.h>

#include "NSDLDrv.h"
#include "NSDLInputTest.h"
#include "UnRender.h"

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
// The worker creates and presents its own WebGL context; see OpenWindow and Unlock.
#include <stdint.h>
#include <emscripten/html5.h>
#include <emscripten/html5_webgl.h>

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
#include <math.h>
// The canvas backing store must be the canvas's device-pixel size, or the browser resamples the picture and
// blurs it on a scaled display. SDL2's Emscripten backend measures and sets the CSS size of #canvas (its
// pixel_ratio is 1 without SDL_WINDOW_ALLOW_HIGHDPI) and reports that in WINDOWEVENT_RESIZED. Passing the
// device size to SDL_SetWindowSize makes SDL set the backing store to exactly that (CSS untouched), report it
// from SDL_GetWindowSize and rescale menu mouse coordinates into it by itself. Both calls are sync-proxied to
// the main thread; SDL already makes the first from this thread on every mouse move. Returns false when the
// box reads 0x0 (display:none) so the caller keeps SDL's own numbers.
static UBOOL ufrontCanvasDevicePixelSize( INT& OutW, INT& OutH )
{
	double CssW = 0.0, CssH = 0.0;
	if( emscripten_get_element_css_size( "#canvas", &CssW, &CssH ) != EMSCRIPTEN_RESULT_SUCCESS )
		return 0;
	const double Dpr = emscripten_get_device_pixel_ratio();
	// lround, not floor: a 1000.8 css box at 1.25 is 1251 device pixels.
	const INT W = (INT)lround( CssW * Dpr ), H = (INT)lround( CssH * Dpr );
	if( W <= 0 || H <= 0 )
		return 0;
	OutW = W; OutH = H;
	return 1;
}
#endif
#endif

IMPLEMENT_CLASS( UNSDLViewport );

/*-----------------------------------------------------------------------------
	UNSDLViewport implementation.
-----------------------------------------------------------------------------*/

//
// SDL_BUTTON_ -> EInputKey translation map.
//
const BYTE UNSDLViewport::MouseButtonMap[6] =
{
	/* invalid           */ IK_None,
	/* SDL_BUTTON_LEFT   */ IK_LeftMouse,
	/* SDL_BUTTON_MIDDLE */ IK_MiddleMouse,
	/* SDL_BUTTON_RIGHT  */ IK_RightMouse,
	/* SDL_BUTTON_X1     */ IK_None,
	/* SDL_BUTTON_X2     */ IK_None
};

//
// SDL_CONTROLLER_BUTTON_ -> EInputKey translation map.
//
const BYTE UNSDLViewport::JoyButtonMap[SDL_CONTROLLER_BUTTON_MAX] =
{
	/* BUTTON_A             */ IK_Joy1,
	/* BUTTON_B             */ IK_Joy2,
	/* BUTTON_X             */ IK_Joy3,
	/* BUTTON_Y             */ IK_Joy4,
	/* BUTTON_BACK          */ IK_Joy5,
	/* BUTTON_GUIDE         */ IK_Joy6,
	/* BUTTON_START         */ IK_Joy7,
	/* BUTTON_LEFTSTICK     */ IK_Joy8,
	/* BUTTON_RIGHTSTICK    */ IK_Joy9,
	/* BUTTON_LEFTSHOULDER  */ IK_Joy10,
	/* BUTTON_RIGHTSHOULDER */ IK_Joy11,
	/* BUTTON_DPAD_UP       */ IK_JoyPovUp,
	/* BUTTON_DPAD_DOWN     */ IK_JoyPovDown,
	/* BUTTON_DPAD_LEFT     */ IK_JoyPovLeft,
	/* BUTTON_DPAD_RIGHT    */ IK_JoyPovRight,
};

//
// SDL_CONTROLLER_BUTTON_ -> EInputKey translation map for UI controls.
//
const BYTE UNSDLViewport::JoyButtonMapUI[SDL_CONTROLLER_BUTTON_MAX] =
{
	/* BUTTON_A             */ IK_Enter,
	/* BUTTON_B             */ IK_Escape,
	/* BUTTON_X             */ IK_N,
	/* BUTTON_Y             */ IK_Y,
	/* BUTTON_BACK          */ IK_Escape,
	/* BUTTON_GUIDE         */ IK_Escape,
	/* BUTTON_START         */ IK_Escape,
	/* BUTTON_LEFTSTICK     */ IK_Joy8,
	/* BUTTON_RIGHTSTICK    */ IK_Joy9,
	/* BUTTON_LEFTSHOULDER  */ IK_Joy10,
	/* BUTTON_RIGHTSHOULDER */ IK_Joy11,
	/* BUTTON_DPAD_UP       */ IK_Up,
	/* BUTTON_DPAD_DOWN     */ IK_Down,
	/* BUTTON_DPAD_LEFT     */ IK_Left,
	/* BUTTON_DPAD_RIGHT    */ IK_Right,
};

//
// SDL_CONTROLLER_BUTTON_ -> EInputKey translation map.
//
const BYTE UNSDLViewport::JoyAxisMap[SDL_CONTROLLER_AXIS_MAX] =
{
	/* AXIS_LEFT_X          */ IK_JoyX,
	/* AXIS_LEFT_Y          */ IK_JoyY,
	/* AXIS_RIGHT_X         */ IK_JoyU,
	/* AXIS_RIGHT_Y         */ IK_JoyV,
	/* AXIS_LTRIGGER        */ IK_Joy12,
	/* AXIS_RTRIGGER        */ IK_Joy13,
};

//
// Additional scale to apply per SDL axis.
//
const FLOAT UNSDLViewport::JoyAxisDefaultScale[SDL_CONTROLLER_AXIS_MAX] =
{
	/* AXIS_LEFT_X          */ +60.f,
	/* AXIS_LEFT_Y          */ -60.f,
	/* AXIS_RIGHT_X         */ +60.f,
	/* AXIS_RIGHT_Y         */ +60.f,
	/* AXIS_LTRIGGER        */ +60.f,
	/* AXIS_RTRIGGER        */ +60.f,
};

//
// SDL_Scancode -> EInputKey translation map.
//
BYTE UNSDLViewport::KeyMap[512];
void UNSDLViewport::InitKeyMap()
{
	#define INIT_KEY_RANGE( AStart, AEnd, BStart, BEnd ) \
		for( DWORD Key = AStart; Key <= AEnd; ++Key ) KeyMap[Key] = BStart + ( Key - AStart )

	appMemset( KeyMap, 0, sizeof( KeyMap ) );

	// TODO: IK_LControl, IK_LShift, etc exist, what are they for?
	KeyMap[SDL_SCANCODE_LSHIFT] = IK_Shift;
	KeyMap[SDL_SCANCODE_RSHIFT] = IK_Shift;
	KeyMap[SDL_SCANCODE_LCTRL] = IK_Ctrl;
	KeyMap[SDL_SCANCODE_RCTRL] = IK_Ctrl;
	KeyMap[SDL_SCANCODE_LALT] = IK_Alt;
	KeyMap[SDL_SCANCODE_RALT] = IK_Alt;
	KeyMap[SDL_SCANCODE_GRAVE] = IK_Tilde;
	KeyMap[SDL_SCANCODE_ESCAPE] = IK_Escape;
	KeyMap[SDL_SCANCODE_SPACE] = IK_Space;
	KeyMap[SDL_SCANCODE_RETURN] = IK_Enter;
	KeyMap[SDL_SCANCODE_BACKSPACE] = IK_Backspace;
	KeyMap[SDL_SCANCODE_CAPSLOCK] = IK_CapsLock;
	KeyMap[SDL_SCANCODE_TAB] = IK_Tab;
	KeyMap[SDL_SCANCODE_DELETE] = IK_Delete;
	KeyMap[SDL_SCANCODE_INSERT] = IK_Insert;
	KeyMap[SDL_SCANCODE_HOME] = IK_Home;
	KeyMap[SDL_SCANCODE_END] = IK_End;
	KeyMap[SDL_SCANCODE_PAGEUP] = IK_PageUp;
	KeyMap[SDL_SCANCODE_PAGEDOWN] = IK_PageDown;
	KeyMap[SDL_SCANCODE_PRINTSCREEN] = IK_PrintScrn;
	KeyMap[SDL_SCANCODE_EQUALS] = IK_Equals;
	KeyMap[SDL_SCANCODE_SEMICOLON] = IK_Semicolon;
	KeyMap[SDL_SCANCODE_BACKSLASH] = IK_Backslash;
	KeyMap[SDL_SCANCODE_SLASH] = IK_Slash;
	KeyMap[SDL_SCANCODE_LEFTBRACKET] = IK_LeftBracket;
	KeyMap[SDL_SCANCODE_RIGHTBRACKET] = IK_RightBracket;
	KeyMap[SDL_SCANCODE_COMMA] = IK_Comma;
	KeyMap[SDL_SCANCODE_PERIOD] = IK_Period;
	KeyMap[SDL_SCANCODE_LEFT] = IK_Left;
	KeyMap[SDL_SCANCODE_UP] = IK_Up;
	KeyMap[SDL_SCANCODE_RIGHT] = IK_Right;
	KeyMap[SDL_SCANCODE_DOWN] = IK_Down;
	KeyMap[SDL_SCANCODE_0] = IK_0;
	KeyMap[SDL_SCANCODE_KP_0] = IK_NumPad0;
	KeyMap[SDL_SCANCODE_KP_PERIOD] = IK_NumPadPeriod;

	INIT_KEY_RANGE( SDL_SCANCODE_1,    SDL_SCANCODE_9,    IK_1,       IK_9 );
	INIT_KEY_RANGE( SDL_SCANCODE_A,    SDL_SCANCODE_Z,    IK_A,       IK_Z );
	INIT_KEY_RANGE( SDL_SCANCODE_KP_1, SDL_SCANCODE_KP_9, IK_NumPad1, IK_NumPad9 );
	INIT_KEY_RANGE( SDL_SCANCODE_F1,   SDL_SCANCODE_F12,  IK_F1,      IK_F12 );
	INIT_KEY_RANGE( SDL_SCANCODE_F13,  SDL_SCANCODE_F24,  IK_F13,     IK_F24 );

#if defined(__ANDROID__)
	// The system's Back (trapped by the client: SDL_HINT_ANDROID_TRAP_BACK_BUTTON) is Escape: back in the menus,
	// the menu in a game.
	KeyMap[SDL_SCANCODE_AC_BACK] = IK_Escape;
#endif

	#undef INIT_KEY_RANGE
}

#if defined(__ANDROID__)
// A Back press went to the exit question (its release goes nowhere either).
static UBOOL GBackAskedExit = 0;
#endif

//
// Static init.
//
void UNSDLViewport::InternalClassInitializer( UClass* Class )
{
	guard(UNSDLViewport::InternalClassInitializer);
	// Fill in keymap.
	InitKeyMap();
	unguard;
}

//
// Constructor.
//
UNSDLViewport::UNSDLViewport( ULevel* InLevel, UNSDLClient* InClient )
:	UViewport( InLevel, InClient )
,	Client( InClient )
{
	guard(UNSDLViewport::UNSDLViewport);

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
	SoftFB = NULL;
	SoftFBBytes = 0;
#endif

	// Set color bytes based on screen resolution.
	SDL_DisplayMode Mode;
	SDL_GetDesktopDisplayMode( InClient->DefaultDisplay, &Mode );
	ColorBytes = SDL_BYTESPERPIXEL( Mode.format );
	Caps = 0;
	if( ColorBytes == 2 && SDL_PIXELLAYOUT( Mode.format ) == SDL_PACKEDLAYOUT_565 )
	{
		Caps |= CC_RGB565;
	}

	// Inherit default display until we have a window.
	DisplayIndex = InClient->DefaultDisplay;
	DisplaySize.w = InClient->GetDefaultDisplayMode().w;
	DisplaySize.h = InClient->GetDefaultDisplayMode().h;

	// Init input.
	if( GIsEditor )
		Input->Init( this, GSystem );

	Destroyed = false;
	QuitRequested = false;
	InputTest = NULL;
	char InputTestFile[256];
	if( Parse( appCmdLine(), "INPUTTEST=", InputTestFile, ARRAY_COUNT(InputTestFile) ) )
		InputTest = new FNSDLInputTest( InputTestFile );

	unguard;
}

// UObject interface.
void UNSDLViewport::Destroy()
{
	guard(UNSDLViewport::Destroy);
	if( Client->FullscreenViewport == this )
	{
		Client->FullscreenViewport = NULL;
	}
	delete InputTest;
	InputTest = NULL;
	UViewport::Destroy();
	unguard;
}

//
// Set the mouse cursor according to Unreal or UnrealEd's mode, or to
// an hourglass if a slow task is active. Not implemented.
//
void UNSDLViewport::SetModeCursor()
{
	guard(UNSDLViewport::SetModeCursor);
	unguard;
}

//
// Update user viewport interface.
//
void UNSDLViewport::UpdateWindow()
{
	guard(UNSDLViewport::UpdateViewportWindow);

	// If not a window, exit.
	if( hWnd==NULL || OnHold )
		return;

	// Set viewport window's name to show resolution.
	char WindowName[80];
	if( !GIsEditor || (Actor->ShowFlags&SHOW_PlayerCtrl) )
	{
		appSprintf( WindowName, LocalizeGeneral("Product","Core") );
	}
	else switch( Actor->RendMap )
	{
		case REN_Wire:		strcpy(WindowName,LocalizeGeneral("ViewPersp")); break;
		case REN_OrthXY:	strcpy(WindowName,LocalizeGeneral("ViewXY")); break;
		case REN_OrthXZ:	strcpy(WindowName,LocalizeGeneral("ViewXZ")); break;
		case REN_OrthYZ:	strcpy(WindowName,LocalizeGeneral("ViewYZ")); break;
		default:			strcpy(WindowName,LocalizeGeneral("ViewOther")); break;
	}

	// Set window title.
	if( SizeX && SizeY )
	{
		appSprintf(WindowName+strlen(WindowName)," (%i x %i)",SizeX,SizeY);
		if( this == Client->CurrentViewport() )
			strcat( WindowName, " *" );
	}
	SDL_SetWindowTitle( hWnd, WindowName );

	unguard;
}

#if !defined(__EMSCRIPTEN__) || !defined(__EMSCRIPTEN_PTHREADS__)
//
// The sample count a render device will run with: its NumAASamples (the compiled default, then the ini), read
// from a probe instance of the device class; 0 for a device without the option.
//
static INT RenderDeviceAASamples( const char* Device )
{
	UClass* Class = Device[0] ? GObj.LoadClass( URenderDevice::StaticClass, NULL, Device, NULL, LOAD_NoWarn | LOAD_KeepImports, NULL ) : NULL;
	if( !Class )
		return 0;
	for( TFieldIterator<UIntProperty> It( Class ); It; ++It )
	{
		if( appStricmp( It->GetName(), "NumAASamples" )==0 )
		{
			UObject* Probe = ConstructClassObject<UObject>( Class );
			const INT Samples = *(INT*)( (BYTE*)Probe + It->Offset );
			delete Probe;
			return Samples;
		}
	}
	return 0;
}
#endif

//
// Open a viewport window.
//
void UNSDLViewport::OpenWindow( void* InParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY )
{
	guard(UNSDLViewport::OpenWindow);
	check(Actor);
	check(!OnHold);
	UBOOL DoRepaint=0, DoSetActive=0;
	UBOOL DoOpenGL=0;
	UBOOL NoHard=ParseParam( appCmdLine(), "nohard" );
	SDL_GLprofile GLProfile = SDL_GL_CONTEXT_PROFILE_COMPATIBILITY;
	char Device[256] = "";	// the render device about to load, as configured
	INT AASamples = 0;		// the multisampling asked of the window's GL surface
	NewX = Align(NewX,4);

	if( !Temporary && !GIsEditor && !NoHard )
	{
		// HACK: Just check if we're about to load OpenGLDrv. Not sure how else you would know to add the GL flag.
		char Temp[256] = "";
		GetConfigString( "Engine.Engine", "GameRenderDevice", Device, ARRAY_COUNT(Device) );
		appStrcpy( Temp, Device );
		appStrupr( Temp );
		if( !appStrstr( Temp, "OPENGL" ) )
		{
			GetConfigString( "Engine.Engine", "WindowedRenderDevice", Device, ARRAY_COUNT(Device) );
			appStrcpy( Temp, Device );
			appStrupr( Temp );
			if( appStrstr( Temp, "OPENGL" ) )
				DoOpenGL = 1;
		}
		else
		{
			DoOpenGL = 1;
		}
		if( DoOpenGL && appStrstr( Temp, "GLES" ) )
			GLProfile = SDL_GL_CONTEXT_PROFILE_ES;
	}

	// User window of launcher if no parent window was specified.
	if( !InParentWindow )
	{
		QWORD ParentPtr;
		Parse( appCmdLine(), "HWND=", ParentPtr );
		InParentWindow = (void*)ParentPtr;
	}

	if( Temporary )
	{
		// Create in-memory data.
		ColorBytes = 2;
		ScreenPointer = (BYTE*)appMalloc( 2 * NewX * NewY, "TemporaryViewportData" );	
		hWnd = NULL;
		debugf( NAME_Log, "Opened temporary viewport" );
	}
	else
	{
		// Get flags.
		DWORD Flags = 0;
		if( InParentWindow && (Actor->ShowFlags & SHOW_ChildWindow) )
		{
			Flags = SDL_WINDOW_SHOWN | SDL_WINDOW_BORDERLESS;
		}
		else
		{
#if defined(__ANDROID__)
			// The whole screen (immersive). SDL's Android window is the surface, the screen's size or the fixed size
			// the app gave it (GameActivity.applyRenderScale); it cannot be resized from here, and SetClientSize
			// takes its size whatever is asked.
			Flags = SDL_WINDOW_SHOWN | SDL_WINDOW_FULLSCREEN;
#else
			// Resizable, so the window can be resized and the viewport follows: the WINDOWEVENT_RESIZED
			// handler in TickInput() syncs the engine viewport and framebuffer.
			Flags = SDL_WINDOW_HIDDEN | SDL_WINDOW_RESIZABLE;
#endif
		}
		if( DoOpenGL )
		{
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
			// Do not ask SDL for a GL window. SDL_WINDOW_OPENGL makes SDL bring up its EGL path, Emscripten
			// proxies EGL to the browser main thread, and the canvas has been transferred to this worker, so
			// the main thread's getContext() throws InvalidStateError. The WebGL context is created further
			// down; SDL is here for the window and input only, which need no GL flag.
#else
			Flags |= SDL_WINDOW_OPENGL;
#endif
		}

		// Set OpenGL attributes if needed.
		if( DoOpenGL )
		{
			if( GLProfile == SDL_GL_CONTEXT_PROFILE_ES )
			{
				// Request GLES2.
				SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 2 );
				SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
			}
			SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, GLProfile );
#if defined(__ANDROID__)
			// SDL asks EGL for a 16-bit depth buffer by default, and the GLES renderer z-fights in bands on one; the
			// web build has the same 24-bit requirement.
			SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE, 24 );
#endif
#if !defined(__EMSCRIPTEN__) || !defined(__EMSCRIPTEN_PTHREADS__)
			// Multisampling belongs to the window's GL surface, so it is requested before the window and its
			// context exist, from the render device's NumAASamples. It holds for the life of the window.
			AASamples = RenderDeviceAASamples( Device );
			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, AASamples > 1 ? 1 : 0 );
			SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, AASamples > 1 ? AASamples : 0 );
#endif
		}

		// Set position and size.
		if( OpenX==-1 )
			OpenX = SDL_WINDOWPOS_UNDEFINED;
		if( OpenY==-1 )
			OpenY = SDL_WINDOWPOS_UNDEFINED;

		// If switching renderers, destroy the old window.
		if( hWnd && ( DoOpenGL != !!( SDL_GetWindowFlags( hWnd ) & SDL_WINDOW_OPENGL ) ) )
		{
			CloseWindow();
		}

		// Create or update the window.
		if( !hWnd )
		{
			// Creating new viewport.
			hWnd = SDL_CreateWindow( "", OpenX, OpenY, NewX, NewY, Flags );
			if( !hWnd && DoOpenGL && AASamples > 1 )
			{
				// Try without multisampling.
				debugf( NAME_Warning, "Could not create a %ix multisampled OpenGL window: %s. Trying without multisampling.", AASamples, SDL_GetError() );
				SDL_GL_SetAttribute( SDL_GL_MULTISAMPLEBUFFERS, 0 );
				SDL_GL_SetAttribute( SDL_GL_MULTISAMPLESAMPLES, 0 );
				AASamples = 0;
				hWnd = SDL_CreateWindow( "", OpenX, OpenY, NewX, NewY, Flags );
			}
			if( !hWnd && DoOpenGL )
			{
				// Try without GL.
				debugf( NAME_Warning, "Could not create OpenGL window: %s. Trying without OpenGL.", SDL_GetError() );
				Flags &= ~SDL_WINDOW_OPENGL;
				DoOpenGL = 0;
				hWnd = SDL_CreateWindow( "", OpenX, OpenY, NewX, NewY, Flags );
			}
			if( !hWnd )
			{
				appErrorf( "Could not create SDL window: %s", SDL_GetError() );
			}

			// Set parent window.
			if( InParentWindow && (Actor->ShowFlags & SHOW_ChildWindow) )
			{
				SDL_SetWindowModalFor( hWnd, (SDL_Window*)InParentWindow );
			}

			debugf( NAME_Log, "Opened viewport" );
			DoSetActive = DoRepaint = 1;
		}
		else
		{
			// Resizing existing viewport.
			SetClientSize( NewX, NewY, false );
		}

		// Create GL context or SDL renderer if needed.
		if( DoOpenGL )
		{
			if( !GLCtx )
			{
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
				// Create the WebGL context directly, not through SDL2. SDL2's Emscripten GL path is all EGL
				// (SDL_EGL_CreateContext_impl), and Emscripten's EGL emulation always proxies to the browser
				// main thread. Under -sPROXY_TO_PTHREAD the canvas has been transferred to this worker, so the
				// main thread can no longer getContext() on it and SDL_GL_CreateContext fails. This context
				// belongs to the worker, so GL calls are not proxied.
				//
				// SDL2 still owns the window and all input; the only SDL call replaced is context creation.
				EmscriptenWebGLContextAttributes Attrs;
				emscripten_webgl_init_context_attributes( &Attrs );
				// WebGL 2 (majorVersion 2), on which NOpenGLESDrv's GLES2 calls run unchanged, for its
				// antialiasing (msaa-backbuffer.js needs multisampled renderbuffers). -WEBGL=1 asks for WebGL 1,
				// and a browser without WebGL 2 gets it.
				INT WebGLMajor = 2;
				if( Parse( appCmdLine(), "WEBGL=", WebGLMajor ) && WebGLMajor != 1 )
					WebGLMajor = 2;
				Attrs.majorVersion              = WebGLMajor;
				Attrs.minorVersion              = 0;
				Attrs.alpha                     = 0;
				Attrs.depth                     = 1;
				Attrs.stencil                   = 1;
				Attrs.antialias                 = 0;
				// explicitSwapControl + renderViaOffscreenBackBuffer are what make the frame ours to
				// present: without them the browser composites at its own cadence and commit_frame() is
				// meaningless on a worker.
				Attrs.explicitSwapControl       = 1;
				Attrs.renderViaOffscreenBackBuffer = 1;
				EMSCRIPTEN_WEBGL_CONTEXT_HANDLE WebGLCtx = emscripten_webgl_create_context( "#canvas", &Attrs );
				if( WebGLCtx <= 0 && Attrs.majorVersion == 2 )
				{
					debugf( NAME_Init, "No WebGL 2 context (handle %i): WebGL 1, without antialiasing", (INT)WebGLCtx );
					Attrs.majorVersion = 1;
					WebGLCtx = emscripten_webgl_create_context( "#canvas", &Attrs );
				}
				if( WebGLCtx <= 0 )
				{
					appErrorf( "Could not create WebGL context on the worker (handle %i)", (INT)WebGLCtx );
				}
				if( emscripten_webgl_make_context_current( WebGLCtx ) != EMSCRIPTEN_RESULT_SUCCESS )
				{
					appErrorf( "Could not make the worker WebGL context current" );
				}
				// Stash it where the rest of the viewport already tests for "do we have GL".
				GLCtx = (SDL_GLContext)(intptr_t)WebGLCtx;
				debugf( NAME_Init, "Created WebGL %i.0 context on the worker (handle %i)", (INT)Attrs.majorVersion, (INT)WebGLCtx );
#else
				GLCtx = SDL_GL_CreateContext( hWnd );
				if( !GLCtx )
				{
					appErrorf( "Could not create GL context: %s", SDL_GetError() );
				}
#endif
			}
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
			emscripten_webgl_make_context_current( (EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)(intptr_t)GLCtx );
#else
			SDL_GL_MakeCurrent( hWnd, GLCtx );
#endif
		}
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
		else
		{
			// No SDL renderer here: SDL's Emscripten renderer is GLES-backed, so SDL_CreateRenderer goes through
			// eglCreateContext, which Emscripten proxies to the browser main thread, where the canvas has been
			// transferred to this worker and getContext() throws; the proxied call never completes and blocks
			// the engine thread. SoftDrv renders into SoftFB instead, and Unlock blits it to the canvas through
			// a 2D context. A null device never locks, so it pays nothing for this.
			FramebufferFormat = SDL_PIXELFORMAT_ARGB8888;
			ColorBytes   = SDL_BYTESPERPIXEL( FramebufferFormat );
			Caps         = 0;
			INT Needed   = NewX * NewY * ColorBytes;
			if( SoftFB && SoftFBBytes < Needed )
			{
				appFree( SoftFB );
				SoftFB = NULL;
			}
			if( !SoftFB )
			{
				SoftFB = (BYTE*)appMalloc( Needed, "ufrontSoftFB" );
				SoftFBBytes = Needed;
			}
		}
#else
		else
		{
			SDL_SetHint( SDL_HINT_RENDER_SCALE_QUALITY, "nearest" );
			SDLRen = SDL_CreateRenderer( hWnd, -1, 0 );
			if( !SDLRen )
			{
				// Fallback to software.
				debugf( NAME_Warning, "Could not create SDL renderer: %s. Trying software.", SDL_GetError() );
				SDLRen = SDL_CreateRenderer( hWnd, -1, SDL_RENDERER_SOFTWARE );
				if( !SDLRen )
				{
					appErrorf( "Could not create SDL renderer: %s", SDL_GetError() );
				}
			}
			// Create framebuffer texture.
			FramebufferFormat = SDL_PIXELFORMAT_ARGB8888;
			ColorBytes = SDL_BYTESPERPIXEL( FramebufferFormat );
			Caps = ( SDL_PIXELLAYOUT( FramebufferFormat ) == SDL_PACKEDLAYOUT_565 ) ? CC_RGB565 : 0;
			SDLTex = SDL_CreateTexture( SDLRen, FramebufferFormat, SDL_TEXTUREACCESS_STREAMING, NewX, NewY );
			if( !SDLTex )
			{
				appErrorf( "Could not create framebuffer texture: %s", SDL_GetError() );
			}
		}
#endif

		SDL_ShowWindow( hWnd );

		// Get this window's display parameters.
		SDL_DisplayMode DisplayMode;
		DisplayIndex = SDL_GetWindowDisplayIndex( hWnd );
		if( SDL_GetWindowDisplayMode( hWnd, &DisplayMode ) == 0 )
		{
			DisplaySize.w = DisplayMode.w;
			DisplaySize.h = DisplayMode.h;
		}

		// Adopt the window's real size instead of the ini size, and match the drawing buffer and the software
		// framebuffer to it; WINDOWEVENT_RESIZED (TickInput) keeps it in sync.
		int RealW = 0, RealH = 0;
		SDL_GetWindowSize( hWnd, &RealW, &RealH );
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
		// The canvas's device-pixel size, unaligned: Align rounds up, and any width other than the box's own
		// is a fractional rescale in the browser, the blur this exists to remove.
		INT DevW = 0, DevH = 0;
		if( ufrontCanvasDevicePixelSize( DevW, DevH ) )
		{
			RealW = DevW;
			RealH = DevH;
		}
		const INT WantX = RealW;
#else
		const INT WantX = Align( RealW, 4 );
#endif
		if( RealW > 0 && RealH > 0 && ( WantX != NewX || RealH != NewY ) )
		{
			NewX = WantX;
			NewY = RealH;
			SDL_SetWindowSize( hWnd, NewX, NewY );
			if( SDLRen && SDLTex )
			{
				SDL_DestroyTexture( SDLTex );
				SDLTex = SDL_CreateTexture( SDLRen, FramebufferFormat, SDL_TEXTUREACCESS_STREAMING, NewX, NewY );
				if( !SDLTex )
					appErrorf( "Could not create framebuffer texture: %s", SDL_GetError() );
			}
		}
	}

	SizeX = NewX;
	SizeY = NewY;

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
	// Grow the software framebuffer to the size adopted above: SoftFB is allocated from the requested size
	// before the drawable-size block replaces NewX/NewY, and SoftDrv writes SizeX*SizeY*ColorBytes bytes into it.
	// Sized here because this is the one place SizeX/SizeY are final, so every path that can change them (first
	// open, the drawable-size block, the WINDOWEVENT_RESIZED re-entry from TickInput) is covered.
	if( SoftFB )
	{
		const INT Needed = SizeX * SizeY * ColorBytes;
		if( SoftFBBytes < Needed )
		{
			appFree( SoftFB );
			SoftFB      = (BYTE*)appMalloc( Needed, "ufrontSoftFB" );
			SoftFBBytes = Needed;
		}
	}
#endif

	if( !RenDev && Temporary )
		Client->TryRenderDevice( this, "SoftDrv.SoftwareRenderDevice", 0 );
	if( !RenDev && !GIsEditor && !NoHard )
		Client->TryRenderDevice( this, "ini:Engine.Engine.GameRenderDevice", Client->StartupFullscreen );
	if( !RenDev )
		Client->TryRenderDevice( this, "ini:Engine.Engine.WindowedRenderDevice", 0 );
	check(RenDev);

	if( !Temporary )
		UpdateWindow();
	if( DoRepaint )
		Repaint();

	unguard;
}

//
// Close a viewport window.  Assumes that the viewport has been opened with
// OpenViewportWindow.  Does not affect the viewport's object, only the
// platform-specific information associated with it.
//
void UNSDLViewport::CloseWindow()
{
	guard(UNSDLViewport::CloseWindow);

	if( hWnd )
	{
		if( SDLTex )
		{
			SDL_DestroyTexture( SDLTex );
			SDLTex = NULL;
		}
		if( SDLRen )
		{
			SDL_DestroyRenderer( SDLRen );
			SDLRen = NULL;
		}
		if( GLCtx )
		{
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
			// GLCtx is an Emscripten WebGL handle here, not an SDL context: SDL never made it and must not be
			// asked to destroy it.
			emscripten_webgl_destroy_context( (EMSCRIPTEN_WEBGL_CONTEXT_HANDLE)(intptr_t)GLCtx );
#else
			SDL_GL_DeleteContext( GLCtx );
#endif
			GLCtx = NULL;
		}
		SDL_DestroyWindow( hWnd );
		hWnd = NULL;
	}

	unguard;
}

//
// Lock the viewport window and set the approprite Screen and RealScreen fields
// of Viewport.  Returns 1 if locked successfully, 0 if failed.  Note that a
// lock failing is not a critical error; it's a sign that a DirectDraw mode
// has ended or the user has closed a viewport window.
//
UBOOL UNSDLViewport::Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData, INT* HitSize )
{
	guard(UNSDLViewport::LockWindow);
	uclock(Client->DrawCycles);

	// Make sure window is lockable.
	if( !hWnd )
	{
		return 0;
	}

	if( OnHold || !SizeX || !SizeY )
	{
		appErrorf( "Failed locking viewport" );
		return 0;
	}

#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
	if( SoftFB )
	{
		// No SDL texture to lock on a worker — the engine draws straight into our framebuffer.
		ScreenPointer = SoftFB;
		Stride = SizeX;
	}
#else
	if( SDLRen && SDLTex )
	{
		// Obtain pointer to screen.
		Stride = SizeX;
		ScreenPointer = NULL;
		SDL_LockTexture( SDLTex, NULL, (void **)&ScreenPointer, &Stride );
		Stride /= ColorBytes;
		check(ScreenPointer);
	}
#endif

	// Success.
	uunclock(Client->DrawCycles);

	return UViewport::Lock( FlashScale, FlashFog, ScreenClear, RenderLockFlags, HitData, HitSize );

	unguard;
}

//
// Unlock the viewport window.  If Blit=1, blits the viewport's frame buffer.
//
void UNSDLViewport::Unlock( UBOOL Blit )
{
	guard(UNSDLViewport::Unlock);

	Client->DrawCycles=0;
	uclock(Client->DrawCycles);

	// Unlock base.
	UViewport::Unlock( Blit );

	// Blit, if desired.
	if( Blit && hWnd && !OnHold )
	{
		if( GLCtx )
		{
			// Flip OpenGL buffers.
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
			// The context was created with explicitSwapControl, so this is the present. SDL_GL_SwapWindow
			// would go through the EGL emulation, which cannot see the worker's context.
			emscripten_webgl_commit_frame();
#else
			SDL_GL_SwapWindow( hWnd );
#endif
		}
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
		else if( SoftFB )
		{
			// Present the software framebuffer on the worker's own OffscreenCanvas. Emscripten sets
			// Module.canvas to the transferred canvas on the thread it was given to, and a 2D context is
			// available there because SoftDrv never creates a WebGL one (a canvas has only one context).
			// SDL_PIXELFORMAT_ARGB8888 is B,G,R,A in memory; ImageData wants R,G,B,A — hence the swizzle,
			// done 32 bits at a time rather than per channel.
			EM_ASM({
				var cv = Module['canvas'];
				if( !cv ) return;
				var ctx = Module.__ufrontSoftCtx;
				if( !ctx )
				{
					ctx = Module.__ufrontSoftCtx = cv.getContext('2d');
					if( !ctx ) return;
				}
				// One declaration per statement: EM_ASM is a macro and the preprocessor splits on
				// top-level commas — braces do not protect them, only parentheses do.
				var w = $1;
				var h = $2;
				var n = w * h;
				if( !Module.__ufrontSoftImg || Module.__ufrontSoftImg.width !== w || Module.__ufrontSoftImg.height !== h )
				{
					if( cv.width !== w ) cv.width = w;
					if( cv.height !== h ) cv.height = h;
					Module.__ufrontSoftImg = ctx.createImageData(w, h);
				}
				var src = new Uint32Array(HEAPU8.buffer, $0, n);
				var dst = new Uint32Array(Module.__ufrontSoftImg.data.buffer);
				for( var i = 0; i < n; i++ )
				{
					var v = src[i];
					dst[i] = (v & 0xff00ff00) | ((v & 0xff) << 16) | ((v >> 16) & 0xff) | 0xff000000;
				}
				ctx.putImageData(Module.__ufrontSoftImg, 0, 0);
			}, SoftFB, SizeX, SizeY);
		}
#else
		else if( SDLRen && SDLTex )
		{
			// Blitting with SDLRenderer.
			SDL_UnlockTexture( SDLTex );
			SDL_RenderCopy( SDLRen, SDLTex, NULL, NULL );
			SDL_RenderPresent( SDLRen );
		}
#endif
	}

	uunclock(Client->DrawCycles);

	unguard;
}

//
// Make this viewport the current one.
// If Viewport=0, makes no viewport the current one.
//
void UNSDLViewport::MakeCurrent()
{
	guard(UNSDLViewport::MakeCurrent);
	Current = 1;
	for( INT i=0; i<Client->Viewports.Num(); i++ )
	{
		UViewport* OldViewport = Client->Viewports(i);
		if( OldViewport->Current && OldViewport != this )
		{
			OldViewport->Current = 0;
			OldViewport->UpdateWindow();
		}
	}
	if( GLCtx )
	{
		SDL_GL_MakeCurrent( hWnd, GLCtx );
	}
	UpdateWindow();
	unguard;
}

//
// Repaint the viewport.
//
void UNSDLViewport::Repaint()
{
	guard(UNSDLViewport::Repaint);
	if( !OnHold && RenDev && SizeX && SizeY )
		Client->Engine->Draw( this, 0 );
	unguard;
}

//
// Set the client size (viewport view size) of a viewport.
//
void UNSDLViewport::SetClientSize( INT NewX, INT NewY, UBOOL UpdateProfile )
{
	guard(UNSDLViewport::SetClientSize);

	if( hWnd )
	{
		SDL_SetWindowSize( hWnd, NewX, NewY );
#if defined(__ANDROID__)
		// The surface's size is the frame's: a requested size is not one the window can take.
		SDL_GetWindowSize( hWnd, &NewX, &NewY );
		NewX = Align( NewX, 4 );
#endif
		// Resize output texture if required.
		if( SDLRen && SDLTex )
		{
			SDL_DestroyTexture( SDLTex );
			SDLTex = SDL_CreateTexture( SDLRen, FramebufferFormat, SDL_TEXTUREACCESS_STREAMING, NewX, NewY );
			if( !SDLTex )
			{
				appErrorf( "Could not create framebuffer texture: %s", SDL_GetError() );
			}
		}
	}

	SizeX = NewX;
	SizeY = NewY;

	// Optionally save this size in the profile.
	if( UpdateProfile )
	{
		Client->ViewportX = NewX;
		Client->ViewportY = NewY;
		Client->SaveConfig();
	}

	unguard;
}

//
// Return the viewport's window.
//
void* UNSDLViewport::GetWindow()
{
	return (void*)hWnd;
}

//
// Try to make this viewport fullscreen, matching the fullscreen
// mode of the nearest x-size to the current window. If already in
// fullscreen, returns to non-fullscreen.
//
void UNSDLViewport::MakeFullscreen( INT NewX, INT NewY, UBOOL UpdateProfile )
{
	guard(UNSDLViewport::MakeFullscreen);

	// If someone else is fullscreen, stop them.
	if( Client->FullscreenViewport )
		Client->EndFullscreen();

	// Save this window.
	SavedX = SizeX;
	SavedY = SizeY;

	// Fullscreen rendering. For now no borderless.
	Client->FullscreenViewport = this;
	SetClientSize( NewX, NewY, false );
	SDL_SetWindowFullscreen( hWnd, SDL_WINDOW_FULLSCREEN );

	if( UpdateProfile )
	{
		Client->ViewportX = NewX;
		Client->ViewportY = NewY;
		Client->SaveConfig();
	}

	SetMouseCapture(1, 1, 0);

	unguard;
}

//
//
//
void UNSDLViewport::EndFullscreen()
{
	guard(UNSDLViewport::EndFullscreen);

#if !defined(__ANDROID__)	// (Android: the window stays the whole screen.)
	SDL_SetWindowFullscreen( hWnd, 0 );
#endif
	SetClientSize( SavedX, SavedY, false );

	unguard;
}

//
// Update input for viewport.
//
void UNSDLViewport::UpdateInput( UBOOL Reset )
{
	guard(UNSDLViewport::UpdateInput);

	if( Reset )
		appMemset( (void*)JoyAxis, 0, sizeof(JoyAxis) );

	unguard;
}

//
// If the cursor is currently being captured, stop capturing, clipping, and 
// hiding it, and move its position back to where it was when it was initially
// captured.
//
void UNSDLViewport::SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL OnlyFocus )
{
	guard(UNSDLViewport::SetMouseCapture);

	// Without the mouse there is nothing to capture.
	if( !Client->UseMouse )
		return;

	// If only focus, reject.
#ifndef __EMSCRIPTEN__
	if( OnlyFocus )
		if( hWnd != SDL_GetMouseFocus() )
			return;
#else
	// No focus gate: the canvas fills the window and SDL2's Emscripten port defers the pointer lock to the
	// next click anyway, so gating the menu->game capture transition on SDL_GetMouseFocus() (which can be
	// stale on the transition frame) would drop the lock request.
	(void)OnlyFocus;
#endif

	// If capturing, windows requires clipping in order to keep focus.
	Clip |= Capture;

	// Handle capturing.
	SDL_SetRelativeMouseMode( (SDL_bool)Capture );

	unguard;
}

UBOOL UNSDLViewport::CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta )
{
	guard(UWindowsViewport::CauseInputEvent);

	// Route to engine if a valid key
	if( iKey > 0 )
		return Client->Engine->InputEvent( this, (EInputKey)iKey, Action, Delta );
	else
		return 0;

	unguard;
}

//
// One stick or trigger value (SDL's -32768..32767; a trigger 0..32767). A trigger is a button with a threshold, a
// stick has its dead zone applied and is sent every tick while it is off centre (TickInput).
//
void UNSDLViewport::UpdatePadAxis( INT Axis, INT NewValue )
{
	const BYTE Key = JoyAxisMap[Axis];
	const INT PrevValue = JoyAxis[Axis];
	INT DeadZone = 0;
	if ( Key < IK_JoyX )
	{
		// Treat the axis like a trigger.
		if ( PrevValue < JoyAxisPressThreshold && NewValue >= JoyAxisPressThreshold )
			CauseInputEvent( Key, IST_Press );
		else if ( PrevValue >= JoyAxisPressThreshold && NewValue < JoyAxisPressThreshold )
			CauseInputEvent( Key, IST_Release );
	}
	else
	{
		// Apply deadzone.
		if ( Key >= IK_JoyX && Key <= IK_JoyZ )
			DeadZone = Client->DeadZoneXYZ * 32767.f;
		else if ( Key == IK_JoyR || Key == IK_JoyU || Key == IK_JoyV )
			DeadZone = Client->DeadZoneRUV * 32767.f;
		if ( Abs(NewValue) < DeadZone )
			NewValue = 0;
	}
	JoyAxis[Axis] = NewValue;
}

// The keys a pad's held-at-connect press gave the game. SDL never reports their release (it never saw them go
// down), so they are a tap: released at the next tick.
static BYTE GHeldTap[32];
static INT GHeldTapNum = 0;
static INT GPadsOn = -1;	// UseJoystick as last seen (-1: not yet; the client's Init said it)

//
// Whether the console is in its Menuing state: the pad's buttons then take the menu map (JoyButtonMapUI). Not
// while the key-binding menu waits for a key, which is its own state.
//
UBOOL UNSDLViewport::InMenu()
{
	return Console
		&& ((UObject*)Console)->GetMainFrame()
		&& ((UObject*)Console)->GetMainFrame()->StateNode
		&& ((UObject*)Console)->GetMainFrame()->StateNode->GetFName() == "Menuing";
}

//
// Nothing may stay held when the pad goes (removed, or UseJoystick off): every pad key is released (both
// maps) and the axes the frame loop re-sends are zeroed.
//
void UNSDLViewport::ReleasePad()
{
	guard(UNSDLViewport::ReleasePad);
	for( INT b=0; b<SDL_CONTROLLER_BUTTON_MAX; b++ )
	{
		if( JoyButtonMap[b] )
			CauseInputEvent( JoyButtonMap[b], IST_Release );
		if( JoyButtonMapUI[b] && JoyButtonMapUI[b] != JoyButtonMap[b] )
			CauseInputEvent( JoyButtonMapUI[b], IST_Release );
	}
	for( INT a=0; a<SDL_CONTROLLER_AXIS_MAX; a++ )
	{
		if( JoyAxisMap[a] && JoyAxisMap[a] < IK_JoyX && JoyAxis[a] >= JoyAxisPressThreshold )
			CauseInputEvent( JoyAxisMap[a], IST_Release );
		JoyAxis[a] = 0;
	}
	unguard;
}

UBOOL UNSDLViewport::TickInput()
{
	guard(UNSDLViewport::TickInput);

	SDL_Event Ev;
	INT Tmp;
	// DOUBLE: appSeconds() can be a large number (seconds since the epoch on some targets), and a FLOAT that size
	// moves in steps of up to 128 s: DeltaTime would be 0 on nearly every frame, and a held stick, scaled by it
	// below, would not move. The first call has no previous time, and a long hitch is not one big step.
	const DOUBLE CurTime = appSeconds();
	const FLOAT DeltaTime = InputUpdateTime > 0.0 ? (FLOAT)Clamp( CurTime - InputUpdateTime, 0.0, 0.25 ) : 0.f;
	for( INT k = 0; k < GHeldTapNum; k++ )
		CauseInputEvent( GHeldTap[k], IST_Release );
	GHeldTapNum = 0;

	if( InputTest )
		InputTest->Tick( this );

	// UseJoystick, every tick. The game's own joystick option (`set ini:Engine.Engine.ViewportManager UseJoystick
	// ...`) sets it on this live client, so it takes effect at once: off closes the pad (keys released), back on
	// opens the first one present (a pad connected while it was off has already sent its ADDED event).
	{
		const INT On = Client->UseJoystick ? 1 : 0;
		if( GPadsOn >= 0 && On != GPadsOn )
		{
			debugf( NAME_Init, "NSDLClient: game pads %s (UseJoystick)", On ? "on" : "off" );
			if( !On )
			{
				if( SDL_GameController* GC = Client->GetController() )
					if( Client->ClosePad( SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( GC ) ) ) )
						ReleasePad();
			}
			else
				for( INT i=0; i<SDL_NumJoysticks() && !Client->GetController(); i++ )
					Client->OpenPad( i, NULL );
		}
		GPadsOn = On;
	}

	// The on-screen pad's state (Android), read by SDL when it updates its joysticks in the poll below.
	Client->FeedTouchPad();

	while( SDL_PollEvent( &Ev ) )
	{
		switch( Ev.type )
		{
			case SDL_QUIT:
				// signal to client and remember set a flag just in case
				QuitRequested = true;
				return true;
			case SDL_TEXTINPUT:
				for( const char *p = Ev.text.text; *p && p < Ev.text.text + sizeof( Ev.text.text ); ++p )
				{
					if( *p < 0 )
						break;
					if( isprint( *p ) || *p == '\r' )
						Client->Engine->Key( this, (EInputKey)*p );
				}
				break;
			case SDL_KEYDOWN:
				if( Ev.key.keysym.sym == SDLK_RETURN && (Ev.key.keysym.mod & KMOD_ALT) )
				{
					Exec("ToggleFullscreen", this);
					break;
				}
#if defined(__ANDROID__)
				// Back with nothing left to go back to asks whether to exit the game.
				if( Ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK && !Ev.key.repeat && Client->BackLeavesGame() )
				{
					GBackAskedExit = 1;
					Client->AskToExit();
					break;
				}
#endif
			case SDL_KEYUP:
#if defined(__ANDROID__)
				// The release of a Back that asked to exit is not Escape's either.
				if( Ev.type == SDL_KEYUP && Ev.key.keysym.scancode == SDL_SCANCODE_AC_BACK && GBackAskedExit )
				{
					GBackAskedExit = 0;
					break;
				}
#endif
				CauseInputEvent( KeyMap[Ev.key.keysym.scancode], ( Ev.type == SDL_KEYDOWN ) ? IST_Press : IST_Release );
				break;
			case SDL_MOUSEBUTTONDOWN:
			case SDL_MOUSEBUTTONUP:
				if( !Client->UseMouse )
					break;
				CauseInputEvent( MouseButtonMap[Ev.button.button], ( Ev.type == SDL_MOUSEBUTTONDOWN ) ? IST_Press : IST_Release );
				break;
			case SDL_MOUSEWHEEL:
				if( !Client->UseMouse )
					break;
				if( Ev.wheel.y )
				{
					CauseInputEvent( IK_MouseW, IST_Axis, Ev.wheel.y );
					if( Ev.wheel.y < 0 )
					{
						CauseInputEvent( IK_MouseWheelDown, IST_Press );
						CauseInputEvent( IK_MouseWheelDown, IST_Release );
					}
					else if( Ev.wheel.y > 0 )
					{
						CauseInputEvent( IK_MouseWheelUp, IST_Press );
						CauseInputEvent( IK_MouseWheelUp, IST_Release );
					}
				}
				break;
			// Hot-plug: a pad connected after start opens here (the first one; UseJoystick gates it,
			// UNSDLClient::OpenPad), and the one in use closes when it goes.
			case SDL_CONTROLLERDEVICEADDED:
				{
					// A real pad takes over from the on-screen pad (Android), which stays attached and takes the place
					// back when the real one goes (below).
					if( SDL_GameController* GC = Client->GetController() )
						if( Client->IsTouchPad( SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( GC ) ) ) && !SDL_JoystickIsVirtual( Ev.cdevice.which ) )
							if( Client->ClosePad( SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( GC ) ) ) )
								ReleasePad();
					DWORD Held = 0;
					if( Client->OpenPad( Ev.cdevice.which, &Held ) && Held )
					{
						// The press that made the browser list the pad (Held is set on the web only) is never
						// reported by SDL; give it to the game now (W3C "standard" button order -> SDL's; the
						// menus' map while a menu is up, as below).
						static const INT W3CToSDL[17] =
						{
							SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
							SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, -1, -1,
							SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
							SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
							SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
							SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT, SDL_CONTROLLER_BUTTON_GUIDE,
						};
						const BYTE* JoyMap = InMenu() ? JoyButtonMapUI : JoyButtonMap;
						for( INT b=0; b<17; b++ )
						{
							if( !(Held & (1u<<b)) )
								continue;
							if( b==6 || b==7 )
							{
								// The triggers are buttons in the browser's layout and axes in SDL's.
								const INT Axis = b==6 ? SDL_CONTROLLER_AXIS_TRIGGERLEFT : SDL_CONTROLLER_AXIS_TRIGGERRIGHT;
								if( JoyAxisMap[Axis] )
								{
									CauseInputEvent( JoyAxisMap[Axis], IST_Press );
									GHeldTap[GHeldTapNum++] = JoyAxisMap[Axis];	// released next tick (a tap)
								}
							}
							else if( JoyMap[W3CToSDL[b]] )
							{
								CauseInputEvent( JoyMap[W3CToSDL[b]], IST_Press );
								GHeldTap[GHeldTapNum++] = JoyMap[W3CToSDL[b]];	// released next tick (a tap)
							}
						}
						debugf( NAME_Init, "NSDLClient: buttons held at the connect given to the game: 0x%x", Held );
					}
				}
				break;
			case SDL_CONTROLLERDEVICEREMOVED:
				if( Client->ClosePad( Ev.cdevice.which ) )
				{
					ReleasePad();	// nothing may stay held
					// The next pad present takes the place (the on-screen pad, Android, attached while a real one had it).
					for( INT i=0; i<SDL_NumJoysticks() && !Client->GetController(); i++ )
						Client->OpenPad( i, NULL );
				}
				break;
			case SDL_CONTROLLERBUTTONDOWN:
			case SDL_CONTROLLERBUTTONUP:
				{
					// HACK: Swap to alternate bindings when in menus, but not when waiting for keypress in the keybind menu.
					const BYTE* JoyMap = InMenu() ? JoyButtonMapUI : JoyButtonMap;
					CauseInputEvent( JoyMap[Ev.cbutton.button], ( Ev.type == SDL_CONTROLLERBUTTONDOWN ) ? IST_Press : IST_Release );
				}
				break;
			case SDL_CONTROLLERAXISMOTION:
				// The sticks and triggers are read from the pad's state every tick (below), not from these.
				break;
			case SDL_MOUSEMOTION:
				if( !Client->UseMouse )
					break;
				if( !Client->FullscreenViewport && !SDL_GetRelativeMouseMode() )
				{
					// If cursor isn't captured, just do MousePosition.
					Client->Engine->MousePosition( this, 0, Ev.motion.x, Ev.motion.y );
				}
				else
				{
					DWORD ViewportButtonFlags = 0;
					if( Ev.motion.state & SDL_BUTTON_LMASK ) ViewportButtonFlags |= MOUSE_Left;
					if( Ev.motion.state & SDL_BUTTON_RMASK ) ViewportButtonFlags |= MOUSE_Right;
					if( Ev.motion.state & SDL_BUTTON_MMASK ) ViewportButtonFlags |= MOUSE_Middle;
					if( Ev.motion.xrel || Ev.motion.yrel )
					{
						Client->Engine->MouseDelta( this, ViewportButtonFlags, Ev.motion.xrel, -Ev.motion.yrel );
						if( Ev.motion.xrel ) CauseInputEvent( IK_MouseX, IST_Axis, Ev.motion.xrel );
						if( Ev.motion.yrel ) CauseInputEvent( IK_MouseY, IST_Axis, -Ev.motion.yrel );
					}
				}
				break;
			case SDL_WINDOWEVENT:
				// Follow window resizes. SetClientSize re-syncs the SDL window, the software framebuffer and
				// SizeX/SizeY; the GL render device re-issues glViewport from Viewport->SizeX/SizeY on the next
				// frame. The no-op guard keeps the SDL_SetWindowSize inside SetClientSize from recursing.
				if( ( Ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED || Ev.window.event == SDL_WINDOWEVENT_RESIZED )
					&& Ev.window.data1 > 0 && Ev.window.data2 > 0 )
				{
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
					// SDL reports the CSS size and has just reset the backing store to it, so re-apply
					// whenever its size differs from the device size, not only when ours does. The
					// echo SIZE_CHANGED from SetClientSize's SDL_SetWindowSize carries the device size and
					// stays a no-op. Unaligned on purpose (see ufrontCanvasDevicePixelSize).
					INT NewX = Ev.window.data1, NewY = Ev.window.data2;
					ufrontCanvasDevicePixelSize( NewX, NewY );
					if( NewX != Ev.window.data1 || NewY != Ev.window.data2 || NewX != SizeX || NewY != SizeY )
						SetClientSize( NewX, NewY, false );
#else
					const INT NewX = Align( Ev.window.data1, 4 ), NewY = Ev.window.data2;
					if( NewX != SizeX || NewY != SizeY )
						SetClientSize( NewX, NewY, false );
#endif
				}
				break;
			default:
				break;
		}
	}

	// The sticks and triggers come from the pad's state every tick, not from SDL_CONTROLLERAXISMOTION. A joystick
	// driver that reports no axis values when a pad opens (SDL's Emscripten one) makes SDL take the first value an
	// axis reports as its resting point and post no event for it (SDL_PrivateJoystickAxis), so the first push of a
	// stick would be lost and a held stick taken for the centre. The state is always right.
	if( SDL_GameController* GC = Client->GetController() )
		for( INT a = 0; a < SDL_CONTROLLER_AXIS_MAX; a++ )
			UpdatePadAxis( a, SDL_GameControllerGetAxis( GC, (SDL_GameControllerAxis)a ) );

	// Constantly hammer the input system with axis events for axes that are not zero.
	for ( INT i = 0; i < SDL_CONTROLLER_AXIS_MAX; ++i )
	{
		const BYTE Key = JoyAxisMap[i];
		const SWORD Value = JoyAxis[i];
		if ( Value && Key && Key >= IK_JoyX )
		{
			const FLOAT FltValue = Clamp( Value / 32767.f, -1.f, 1.f );
			FLOAT Scale = ( Key >= IK_JoyX && Key <= IK_JoyZ ) ? Client->ScaleXYZ : Client->ScaleRUV;
			Scale *= JoyAxisDefaultScale[i] * DeltaTime;
			if ( ( Client->InvertV && Key == IK_JoyV ) || ( Client->InvertY && Key == IK_JoyY ) )
				Scale = -Scale;
			CauseInputEvent( Key, IST_Axis, FltValue * Scale );
		}
	}

	InputUpdateTime = CurTime;

	// What the on-screen pad draws (menus or game) and whether it is in use, for the app's UI thread (Android).
	Client->PublishTouchPadState();

	return QuitRequested;

	unguard;
}

/*-----------------------------------------------------------------------------
	Command line.
-----------------------------------------------------------------------------*/

UBOOL UNSDLViewport::Exec( const char* Cmd, FOutputDevice* Out )
{
	guard(UNSDLViewport::Exec);
	if( UViewport::Exec( Cmd, Out ) )
	{
		return 1;
	}
	else if( ParseCommand(&Cmd, "ToggleFullscreen") )
	{
		// Toggle fullscreen.
		if( Client->FullscreenViewport )
			Client->EndFullscreen();
		else if( !(Actor->ShowFlags & SHOW_ChildWindow) )
			Client->TryRenderDevice( this, "ini:Engine.Engine.GameRenderDevice", 1 );
		return 1;
	}
	else if( ParseCommand(&Cmd, "GetCurrentRes") )
	{
		Out->Logf( "%ix%i", SizeX, SizeY );
		return 1;
	}
	else if( ParseCommand(&Cmd, "SetRes") )
	{
		INT X=appAtoi(Cmd), Y=appAtoi(appStrchr(Cmd,'x') ? appStrchr(Cmd,'x')+1 : appStrchr(Cmd,'X') ? appStrchr(Cmd,'X')+1 : "");
		if( X && Y )
		{
			if( Client->FullscreenViewport )
				MakeFullscreen( X, Y, 1 );
			else
				SetClientSize( X, Y, 1 );
		}
		return 1;
	}
	else if( ParseCommand(&Cmd, "Preferences") )
	{
		if( Client->FullscreenViewport )
			Client->EndFullscreen();
		return 1;
	}
	else return 0;
	unguard;
}
