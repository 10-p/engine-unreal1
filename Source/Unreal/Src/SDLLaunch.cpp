#include "SDL2/SDL.h"
#ifdef PLATFORM_WIN32
#include <windows.h>
#endif
#ifdef PLATFORM_PSVITA
#include <vitasdk.h>
#include <vitaGL.h>
#include <unistd.h>
#endif
#ifdef __EMSCRIPTEN__
#include <unistd.h> // chdir (ufront 2.6b)
#endif

#include "Engine.h"

extern CORE_API FGlobalPlatform GTempPlatform;
extern DLL_IMPORT UBOOL GTickDue;
extern "C" {HINSTANCE hInstance;}
extern "C" {char GCC_HIDDEN THIS_PACKAGE[64]="Launch";}

// FExecHook.
class FExecHook : public FExec
{
	UBOOL Exec( const char* Cmd, FOutputDevice* Out )
	{
		return 0;
	}
};

FExecHook GLocalHook;
DLL_EXPORT FExec* GThisExecHook = &GLocalHook;

#ifdef __EMSCRIPTEN__
// ufront 2.6c: the running engine, for WebRTCDrv's listen-status push + ue_exec_command. Set at the end
// of InitEngine. WebRTCDrv.cpp declares `extern UEngine* GWebRTCEngine`.
UEngine* GWebRTCEngine = NULL;
#endif

#ifdef PLATFORM_PSVITA

//
// PSVita-specific globals.
//

#define MAX_PATH 1024
#define SYSTEM_PATH "data/unreal/System/"

// 200MB libc heap, 512K main thread stack, 16MB for loading game DLLs
// the rest goes to vitaGL
extern "C" { SceUInt32 sceUserMainThreadStackSize = 512 * 1024; }
extern "C" { unsigned int _pthread_stack_default_user = 512 * 1024; }
extern "C" { unsigned int _newlib_heap_size_user = 200 * 1024 * 1024; }
#define VGL_MEM_THRESHOLD ( 16 * 1024 * 1024 )

static char GRootPath[MAX_PATH] = "app0:/";

//
// PSVita-specific functions.
//

static bool FindRootPath( char* Out, int OutLen )
{
	static const char *Drives[] = { "uma0", "imc0", "ux0" };

	// check if an unreal folder exists on one of the drives
	// default to the last one (ux0)
	for ( unsigned int i = 0; i < sizeof(Drives) / sizeof(*Drives); ++i )
	{
		snprintf( Out, OutLen, "%s:/" SYSTEM_PATH, Drives[i] );
		SceUID Dir = sceIoDopen( Out );
		if ( Dir >= 0 )
		{
			sceIoDclose( Dir );
			return true;
		}
	}

	// not found
	return false;
}

static INT PowerCallback( INT NotifyID, INT NotifyCnt, INT PowerInfo, void* Common )
{
	if ( PowerInfo & ( SCE_POWER_CB_APP_RESUME | SCE_POWER_CB_APP_RESUMING ) )
	{
		debugf( "PowerCallback: resuming..." );
		appHandleSuspendResume( false );
	}
	else if ( PowerInfo & ( SCE_POWER_CB_BUTTON_PS_PRESS | SCE_POWER_CB_APP_SUSPEND | SCE_POWER_CB_SYSTEM_SUSPEND ) )
	{
		debugf( "PowerCallback: suspending..." );
		appHandleSuspendResume( true );
	}

	return 0;
}

static INT CallbackThread( DWORD Argc, void* Argv )
{
	const INT CbID = sceKernelCreateCallback( "Power Callback", 0, PowerCallback, nullptr );
	scePowerRegisterCallback( CbID );
	while( true )
		sceKernelDelayThreadCB( 10000000 );
	return 0;
}

[[noreturn]] static void EarlyError( const char* Msg )
{
	fprintf( stderr, "FATAL ERROR: %s\n", Msg );
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, "Fatal Error", Msg, nullptr );
	sceKernelExitProcess( 0 );
	abort();
}

static void PlatformPreInit()
{
	sceTouchSetSamplingState( SCE_TOUCH_PORT_BACK, SCE_TOUCH_SAMPLING_STATE_STOP );
	scePowerSetArmClockFrequency( 444 );
	scePowerSetBusClockFrequency( 222 );
	scePowerSetGpuClockFrequency( 222 );
	scePowerSetGpuXbarClockFrequency( 166 );
	sceSysmoduleLoadModule( SCE_SYSMODULE_NET );

	if ( !FindRootPath( GRootPath, sizeof(GRootPath) ) )
		EarlyError( "Could not find Unreal directory" );

	if ( chdir( GRootPath ) < 0 )
		EarlyError( "Could not chdir to Unreal directory" );

	SceUID Th = sceKernelCreateThread( "CallbackThread", CallbackThread, 0x10000100, 0x10000, 0, 0, nullptr );
	if( Th >= 0 )
		sceKernelStartThread( Th, 0, nullptr );

	vglInitWithCustomThreshold( 0, 960, 544, VGL_MEM_THRESHOLD, 0, 0, 0, SCE_GXM_MULTISAMPLE_2X );
	vglSetSemanticBindingMode( VGL_MODE_POSTPONED );
}

#else

void PlatformPreInit()
{

}

#endif


//
// Handle an error.
//
void HandleError()
{
	GIsGuarded=0;
	GIsCriticalError=1;
	debugf( NAME_Exit, "Shutting down after catching exception" );
	GObj.ShutdownAfterError();
	debugf( NAME_Exit, "Exiting due to exception" );
	GErrorHist[ARRAY_COUNT(GErrorHist)-1]=0;
	SDL_ShowSimpleMessageBox( SDL_MESSAGEBOX_ERROR, LocalizeError("Critical"), GErrorHist, SDL_GetKeyboardFocus() );
}

//
// ufront (renderer parity, 2.12): unified -*RENDERER render-device selection, SHARED native+web (the
// #ifdef __EMSCRIPTEN__ guard was removed 2026-07-16 so `./Unreal -SOFTWARERENDERER` / -NOPENGLESRENDERER /
// -NULLRENDERER work identically on native — the run-tree launch scripts are only a testing convenience).
// Ported from engine-ut99's SelectRenderDevice scheme to the v200 (plain-char, no TCHAR) API. Each switch
// names a render device; NSDLViewport picks a GL/GLES context (device name contains "OPENGL"/"GLES") or the
// software SDL_Texture surface otherwise. Precedence: a switch, else the persisted ini choice IF it is a
// HARDWARE default OR the user's explicit pick (ufrontUserRenderDevice marker — so the shipped Default.ini
// GameRenderDevice=SoftDrv never auto-adopts, yet a user's SoftDrv choice sticks), else the preference order.
//
struct FRenderSwitch { const char* Param; const char* Device; };
static const FRenderSwitch GRenderSwitches[] =
{
	{ "NOPENGLESRENDERER", "NOpenGLESDrv.NOpenGLESRenderDevice" }, // GLES2 -> WebGL (the hardware default)
	{ "SOFTWARERENDERER",  "SoftDrv.SoftwareRenderDevice" },       // CPU rasterizer (GPU-free)
	{ "NULLRENDERER",      "NullDrv.NullRenderDevice" },           // no-op device (GPU-free; headless/dedicated)
};
// Default when nothing is chosen: best HW first (unreal1 ships only NOpenGLES on web).
static const char* GRenderPreference[] =
{
	"NOpenGLESDrv.NOpenGLESRenderDevice",
};
// The class a -*RENDERER switch requested (empty if none) — persisted after a successful Init().
static char GRequestedRenderDevice[256] = "";

// Short class name (after the last '.') of a "Package.Class" string.
static const char* RenderClassName( const char* Device )
{
	const char* ClassName = Device;
	for( const char* P=Device; *P; ++P )
		if( *P=='.' ) ClassName = P+1;
	return ClassName;
}

// True iff the render-device class is compiled into / loadable by this build.
static UBOOL IsRenderDeviceAvailable( const char* Device )
{
#ifdef __EMSCRIPTEN__
	// web (single static binary): FindObject inspects only ALREADY-registered objects (a statically-linked
	// render device registers at startup) — no file system, no throw.
	return GObj.FindObject( UClass::StaticClass, ANY_PACKAGE, RenderClassName(Device) ) != NULL;
#else
	// native (shared .so packages): render-device classes are NOT registered until their package is loaded, and
	// SelectRenderDevice() runs before Engine->Init(), so FindObject would see nothing. Load the class by name
	// (LOAD_NoWarn) — a compiled-in driver resolves + registers, an absent one returns NULL. UObject is the
	// permissive base (URenderDevice's header isn't included here). Native builds NOpenGLES + SoftDrv + Null
	// (the full switch/preference set), so this resolves for all.
	return GObj.LoadClass( UObject::StaticClass, NULL, Device, NULL, LOAD_NoWarn, NULL ) != NULL;
#endif
}

// True iff Device is a hardware render device eligible as an IMPLICIT web default (in GRenderPreference).
// SoftDrv/NullDrv are opt-in ONLY (via their switch), never auto-adopted from the shipped/persisted ini
// (unreal1's Default.ini ships GameRenderDevice=SoftDrv — a 1998 default; on web that must resolve to
// NOpenGLES/WebGL, not the CPU rasterizer). Mirrors engine-ut99's IsHardwareDefaultEligible.
static UBOOL IsHardwareDefaultEligible( const char* Device )
{
	for( INT i=0; i<ARRAY_COUNT(GRenderPreference); i++ )
		if( appStricmp( Device, GRenderPreference[i] )==0 )
			return 1;
	return 0;
}

static void SetRenderDeviceConfig( const char* Device )
{
	SetConfigString( "Engine.Engine", "GameRenderDevice",     Device );
	SetConfigString( "Engine.Engine", "WindowedRenderDevice", Device );
	SetConfigString( "Engine.Engine", "RenderDevice",         Device );
}

// Choose the web render device BEFORE Engine->Init() (replaces the old -SOFTWARE if/else).
static void SelectRenderDevice()
{
	// 1) A command-line -*RENDERER switch (first present wins).
	for( INT i=0; i<ARRAY_COUNT(GRenderSwitches); i++ )
	{
		if( ParseParam( appCmdLine(), GRenderSwitches[i].Param ) )
		{
			const char* Device = GRenderSwitches[i].Device;
			if( IsRenderDeviceAvailable( Device ) )
			{
				debugf( "ufront: -%s selects render device %s", GRenderSwitches[i].Param, Device );
				SetRenderDeviceConfig( Device );
				appStrcpy( GRequestedRenderDevice, Device ); // persist iff it initializes
				return;
			}
			debugf( NAME_Warning, "ufront: render device %s not available -> keeping the default; -%s ignored", Device, GRenderSwitches[i].Param );
			break; // fall through to the default selection
		}
	}

	// 2) The persisted ini choice, if compiled in AND a hardware default (so the shipped SoftDrv never wins).
	char Persisted[256] = "", UserSel[256] = "";
	if( GetConfigString( "Engine.Engine", "GameRenderDevice", Persisted, ARRAY_COUNT(Persisted) ) && Persisted[0] )
	{
		// A persisted device sticks if it is a hardware default OR was EXPLICITLY user-selected: the marker
		// ufrontUserRenderDevice is written by PersistRenderChoiceIfSwitched ONLY after a -*RENDERER switch
		// actually initialized, so it distinguishes a user's SoftDrv pick (sticky, per owner 2.12) from the
		// shipped Default.ini GameRenderDevice=SoftDrv (no marker -> still overridden to a hardware default first boot).
		const UBOOL bUserSelected =
			GetConfigString( "Engine.Engine", "ufrontUserRenderDevice", UserSel, ARRAY_COUNT(UserSel) )
			&& UserSel[0] && appStricmp( Persisted, UserSel )==0;
		if( IsRenderDeviceAvailable( Persisted ) && ( IsHardwareDefaultEligible( Persisted ) || bUserSelected ) )
		{
			debugf( "ufront 2.12: using the persisted %s render device %s", bUserSelected ? "user-selected" : "hardware", Persisted );
			SetRenderDeviceConfig( Persisted );
			return;
		}
		debugf( NAME_Warning, "ufront: persisted render device %s is not an eligible hardware default (unavailable or opt-in) -> preference order", Persisted );
	}

	// 3) Preference order — first compiled-in device wins.
	for( INT i=0; i<ARRAY_COUNT(GRenderPreference); i++ )
	{
		if( IsRenderDeviceAvailable( GRenderPreference[i] ) )
		{
			debugf( "ufront: default render device %s (preference order)", GRenderPreference[i] );
			SetRenderDeviceConfig( GRenderPreference[i] );
			return;
		}
	}
	debugf( NAME_Warning, "ufront: no known render device is compiled into this build!" );
}

// After Engine->Init(): persist a -*RENDERER switch choice so it sticks next boot (the frontend renderer
// dropdown is session-only). If the engine didn't rewrite GameRenderDevice during Init, save the config
// (SaveAllConfigs -> MEMFS -> the user-data IDB layer). Mirrors engine-ut99's PersistRenderChoiceIfSwitched.
static void PersistRenderChoiceIfSwitched()
{
	if( GRequestedRenderDevice[0]==0 )
		return;
	char Active[256] = "";
	if( GetConfigString( "Engine.Engine", "GameRenderDevice", Active, ARRAY_COUNT(Active) ) && appStricmp( Active, GRequestedRenderDevice )==0 )
	{
		// Mark this as an EXPLICIT user selection so SelectRenderDevice honors it next boot even when it is
		// not a hardware default (i.e. a user-chosen SoftDrv). Distinguishes it from the shipped SoftDrv.
		SetConfigString( "Engine.Engine", "ufrontUserRenderDevice", GRequestedRenderDevice );
		GConfigCache.SaveAllConfigs();
		debugf( "ufront 2.12: persisted render device %s as a user selection (sticks next boot)", GRequestedRenderDevice );
	}
	else
	{
		debugf( NAME_Warning, "ufront: render device changed from %s during init -> not persisted", GRequestedRenderDevice );
	}
}

// (render-device selection above is compiled + used on both native and web — see the header note.)

//
// Initialize.
//
UEngine* InitEngine()
{
	guard(InitEngine);

	// Platform init.
	appInit();
	GDynMem.Init( 65536 );

	// Init subsystems.
	GSceneMem.Init( 32768 );

	// First-run menu.
	UBOOL FirstRun=0;
	GetConfigBool( "FirstRun", "FirstRun", FirstRun );

#ifdef __EMSCRIPTEN__
	// ufront 2.12 (web audio): route audio to the real OpenAL subsystem (NOpenALDrv) — Emscripten's built-in
	// OpenAL for SFX + a FetchContent'd libxmp for tracker music, pumped from Update() (the web build is
	// single-threaded; EFX/HRTF are compiled out — see NOpenALDrv.cpp NOPENAL_WEB). Replaces the earlier
	// null subsystem. The driver's constructor defaults are audible (MasterVolume=255) so no ini is required.
	SetConfigString( "Engine.Engine", "AudioDevice", "NOpenALDrv.NOpenALAudioSubsystem" );
	// ufront 2.6c: BSD sockets don't work in a browser — auto-switch the net driver to the WebRTC driver
	// (its transport is the glue webrtc_* ABI). Mirrors engine-ut2004's TcpNetDriver->WebRTCNetDriver switch.
	SetConfigString( "Engine.Engine", "NetworkDevice", "WebRTCDrv.WebRTCNetDriver" );
	// ufront 2.12 (web mouse): the SDL client ships CaptureMouse=False (classic click-drag-to-look), but the
	// web wants UT2004-style dynamic pointer lock — captured while playing, released in a menu. Enable it:
	// UGameEngine::Tick then drives SetMouseCapture from menu state (bShowMenu), NSDLViewport::SetMouseCapture
	// calls SDL_SetRelativeMouseMode, and SDL2's Emscripten port defers the browser lock to the next click
	// (user gesture) + re-grabs on mousedown after ESC drops it — so this one flag yields click-to-lock +
	// ESC-to-unlock. Native keeps the ini's CaptureMouse (revisit in the native pass).
	SetConfigString( "NSDLDrv.NSDLClient", "CaptureMouse", "True" );
	// ufront (renderer parity): pick the render device (Game/Windowed/RenderDevice) via the unified switch
	// scheme — a -*RENDERER switch (-NOPENGLESRENDERER / -SOFTWARERENDERER / -NULLRENDERER), else the persisted
	// ini choice if it is a compiled hardware default, else preference (NOpenGLES). Replaces the old binary
	// -SOFTWARE toggle; the shipped Default.ini GameRenderDevice=SoftDrv no longer auto-adopts (see
	// IsHardwareDefaultEligible). NSDLViewport reads the device name: "GLES" -> an ES context, else the
	// SDL_Texture software surface (SoftDrv/Null both run GPU-free).
#endif

#if defined(PLATFORM_SDL) && !defined(__EMSCRIPTEN__)
	// ufront 2.12 (native mouse — the "native pass" the web block above deferred). Enable dynamic pointer lock
	// on native too: the shipped NSDLClient CaptureMouse=False is the classic Windows click-drag-to-look mode,
	// but NSDLDrv never calls UViewport::SetDrag, so that path is DEAD on the SDL build (no mouse-look at all
	// on native). With CaptureMouse=True the existing (unguarded) release-in-menu block in UGameEngine::Tick
	// drives UViewport::SetMouseCapture -> SDL_SetRelativeMouseMode: captured while playing, released in menus
	// (unreal1's model). Forced here so it holds regardless of the retail asset ini (which ships False).
	// Excluded on Emscripten (web set this in the __EMSCRIPTEN__ block above) -> web wasm byte-identical.
	SetConfigString( "NSDLDrv.NSDLClient", "CaptureMouse", "True" );
#endif

	// ufront 2.12: unified render-device selection — SHARED native+web, run BEFORE Engine->Init() (which loads
	// the subsystems). A -*RENDERER switch wins, else a persisted hardware default OR the user's marked pick,
	// else the preference order. (Was #ifdef __EMSCRIPTEN__; now also honors `./Unreal -SOFTWARERENDERER` etc.
	// on native.)
	SelectRenderDevice();

	// Create the global engine object.
	UClass* EngineClass;
	if( !GIsEditor )
	{
		// Create game engine.
		EngineClass = GObj.LoadClass( UGameEngine::StaticClass, NULL, "ini:Engine.Engine.GameEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}
	else if( ParseParam( appCmdLine(),"MAKE" ) )
	{
		// Create editor engine.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_DisallowFiles | LOAD_KeepImports, NULL );
	}
	else
	{
		// Editor.
		EngineClass = GObj.LoadClass( UEngine::StaticClass, NULL, "ini:Engine.Engine.EditorEngine", NULL, LOAD_NoFail | LOAD_KeepImports, NULL );
	}

	// Init engine.
	UEngine* Engine = ConstructClassObject<UEngine>( EngineClass );
	Engine->Init();

	// ufront 2.12: persist a -*RENDERER switch choice so it sticks next boot — SHARED native+web (writes the
	// ufrontUserRenderDevice marker; no-op unless a switch was given + it survived Init).
	PersistRenderChoiceIfSwitched();
#ifdef __EMSCRIPTEN__
	// ufront 2.6c: expose the engine to WebRTCDrv (listen-status push) + ue_exec_command.
	GWebRTCEngine = Engine;
#endif

	return Engine;

	unguard;
}

//
// Unreal's main message loop.  All windows in Unreal receive messages
// somewhere below this function on the stack.
//
#ifdef __EMSCRIPTEN__
#include <emscripten.h>

// ufront 2.6c (ADR-10): run a console command and RETURN its captured output (not fire-and-forget). The
// frontend/E2E call this via ccall for deterministic assertions + live debugging. UE1's FStringOut is a
// string-capturing FOutputDevice; the captured text is held in a static FString so the returned pointer
// stays valid until the next call (the JS side reads it synchronously during the ccall).
extern "C" char* EMSCRIPTEN_KEEPALIVE ue_exec_command( const char* Cmd )
{
	static FString Captured;
	Captured = "";
	if( GWebRTCEngine && Cmd )
	{
		FStringOut Out;
		GWebRTCEngine->Exec( Cmd, &Out );
		Captured = Out;
	}
	return (char*)(*Captured);
}
#endif

void MainLoop( UEngine* Engine )
{
	guard(MainLoop);

	GIsRunning = 1;
	DOUBLE OldTime = appSeconds();
	while( GIsRunning && !GIsRequestingExit )
	{
		// Update the world.
		DOUBLE NewTime = appSeconds();
		Engine->Tick( NewTime - OldTime );
		OldTime = NewTime;

		// Enforce optional maximum tick rate.
		INT MaxTickRate = Engine->GetMaxTickRate();
#ifdef __EMSCRIPTEN__
		// ufront 2.6b — main-loop inversion (docs/10 §2), matching engine-ut2004: a browser can't block its
		// main thread, so each frame we yield via emscripten_sleep, which ASYNCIFY-suspends the wasm and lets
		// the browser event loop run (rendering + rAF) before resuming. We do NOT use emscripten_set_main_loop:
		// its simulate_infinite_loop unwind exception collides with UE1 main()'s C++ try/catch. Always yield
		// >= 1 ms so the browser makes progress even when a frame overruns the tick budget.
		DOUBLE Budget = MaxTickRate ? (1.0 / MaxTickRate) : 0.0;
		INT SleepMs = (INT)((Budget - (appSeconds() - OldTime)) * 1000.0);
		emscripten_sleep( SleepMs > 1 ? SleepMs : 1 );
#else
		if( MaxTickRate )
		{
			DOUBLE Delta = (1.0/MaxTickRate) - (appSeconds()-OldTime);
			if( Delta > 0.0 )
				appSleep( Delta );
		}
#endif
	}
	GIsRunning = 0;
	unguard;
}

//
// Exit the engine.
//
void ExitEngine( UEngine* Engine )
{
	guard(ExitEngine);

	GObj.Exit();
	GMem.Exit();
	GDynMem.Exit();
	GSceneMem.Exit();
	GCache.Exit(1);
	appDumpAllocs( &GTempPlatform );

	unguard;
}

#ifdef PLATFORM_WIN32
INT WINAPI WinMain( HINSTANCE hInInstance, HINSTANCE hPrevInstance, char* InCmdLine, INT nCmdShow )
#else
int main( int argc, const char** argv )
#endif
{
#ifdef PLATFORM_WIN32
	hInstance = hInInstance;
#else
	hInstance = NULL;
	// Remember arguments since we don't have GetCommandLine().
	appSetCmdLine( argc, argv );
	PlatformPreInit();
#endif

#ifdef __EMSCRIPTEN__
	// ufront 2.6b (web port): the MEMFS root is "/", but Unreal runs from its System/ dir like a native
	// install — Default.ini is preloaded at /System, and config + the package-search Paths (../System,
	// ../Maps, ...) resolve relative to the base dir. FetchFS has already created /System (from the manifest)
	// in preRun. chdir here, BEFORE appBaseDir() caches getcwd(), so appBaseDir() becomes "/System/".
	chdir( "/System" );
#endif

	GIsStarted = 1;

	// Set package name.
	appStrcpy( THIS_PACKAGE, appPackage() );

	// Init mode.
	GIsServer = 1;
	GIsClient = !ParseParam(appCmdLine(),"SERVER") && !ParseParam(appCmdLine(),"MAKE");
	GIsEditor = ParseParam(appCmdLine(),"EDITOR") || ParseParam(appCmdLine(),"MAKE");

	// Init windowing.
	appChdir( appBaseDir() );

	// Init log.
	// TODO: GLog
	GExecHook = GThisExecHook;

	// Begin.
#ifndef _DEBUG
	try
	{
#endif
		// Start main loop.
		GIsGuarded=1;
		GSystem = &GTempPlatform;
		UEngine* Engine = InitEngine();
		if( !GIsRequestingExit )
			MainLoop( Engine );
		ExitEngine( Engine );
		GIsGuarded=0;
#ifndef _DEBUG
	}
	catch( ... )
	{
		// Crashed.
		try {HandleError();} catch( ... ) {}
	}
#endif

	// Shut down.
	GExecHook=NULL;
	appExit();
	GIsStarted = 0;
	return 0;
}
