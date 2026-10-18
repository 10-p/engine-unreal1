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
#include <unistd.h> // chdir
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
// The running engine, for WebRTCDrv's listen-status push and ue_exec_command; set at the end of InitEngine.
// WebRTCDrv.cpp declares `extern UEngine* GEngineForExec`.
UEngine* GEngineForExec = NULL;
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
// Render-device selection, the same on every target. Each -*RENDERER switch names a render device; NSDLViewport
// picks a GL/GLES context (device name contains "OPENGL"/"GLES") or the software SDL_Texture surface otherwise.
// Precedence: a switch, else the persisted ini choice if it is a hardware default or the user's explicit pick
// (the ufrontUserRenderDevice marker, so a shipped GameRenderDevice=SoftDrv is not adopted but a user's SoftDrv
// choice sticks), else the preference order.
//
struct FRenderSwitch { const char* Param; const char* Device; };
static const FRenderSwitch GRenderSwitches[] =
{
#ifdef UT_HAS_NOPENGLDRV
	// Desktop GL (NOpenGLDrv), present when BUILD_NOPENGLDRV is on (the native and Windows targets).
	{ "NOPENGLRENDERER",   "NOpenGLDrv.NOpenGLRenderDevice" },     // desktop OpenGL 1.4 compatibility
#endif
	{ "NOPENGLESRENDERER", "NOpenGLESDrv.NOpenGLESRenderDevice" }, // GLES2 (WebGL on the web)
	{ "SOFTWARERENDERER",  "SoftDrv.SoftwareRenderDevice" },       // CPU rasterizer (GPU-free)
	{ "NULLRENDERER",      "NullDrv.NullRenderDevice" },           // no-op device (GPU-free; headless/dedicated)
};
// Default when nothing is chosen: the hardware render devices, best first.
static const char* GRenderPreference[] =
{
#if defined(UT_HAS_NOPENGLDRV) && defined(PLATFORM_WIN32)
	// On Windows desktop GL leads: a GLES2 context there needs a driver that exposes ES contexts through WGL
	// or ANGLE's libEGL/libGLESv2 beside the exe, whereas opengl32 is always present. The shipped ini's SoftDrv
	// is not hardware-eligible, so this list is what a fresh Windows install lands on.
	"NOpenGLDrv.NOpenGLRenderDevice",
	"NOpenGLESDrv.NOpenGLESRenderDevice",
#else
	// Elsewhere GLES2 leads, so a native build runs the web's renderer unless asked otherwise.
	"NOpenGLESDrv.NOpenGLESRenderDevice",
#ifdef UT_HAS_NOPENGLDRV
	"NOpenGLDrv.NOpenGLRenderDevice",
#endif
#endif
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
static UBOOL IsPackageClassAvailable( const char* Device )
{
#ifdef __EMSCRIPTEN__
	// FindObject inspects only registered objects (a statically linked render device registers at startup):
	// no file access, no throw.
	return GObj.FindObject( UClass::StaticClass, ANY_PACKAGE, RenderClassName(Device) ) != NULL;
#else
	// Loads the class by name (LOAD_NoWarn): a compiled-in driver resolves and registers, an absent one returns
	// NULL. UObject is the permissive base (URenderDevice's header is not included here).
	return GObj.LoadClass( UObject::StaticClass, NULL, Device, NULL, LOAD_NoWarn, NULL ) != NULL;
#endif
}

// True iff Device is a hardware render device eligible as an implicit default (it is in GRenderPreference).
// SoftDrv/NullDrv are opt-in only (via their switch), never adopted from a shipped or persisted ini (the web
// Default.ini, like the retail one, ships GameRenderDevice=SoftDrv).
static UBOOL IsHardwareDefaultEligible( const char* Device )
{
	for( INT i=0; i<ARRAY_COUNT(GRenderPreference); i++ )
		if( appStricmp( Device, GRenderPreference[i] )==0 )
			return 1;
	return 0;
}

#ifndef __EMSCRIPTEN__
// Replaces a configured subsystem this build does not contain (the retail ini names Galaxy.GalaxyAudioSubsystem;
// the engine loads it with LOAD_NoFail, so a missing one is a fatal error) with the first present entry of
// Preference. A present one is left alone.
// Not compiled on Emscripten, where InitEngine sets the web subsystems outright.
static void NormalizeSubsystem( const char* Section, const char* Key, const char* const* Preference, INT Num )
{
	char Configured[256] = "";
	if( GetConfigString( Section, Key, Configured, ARRAY_COUNT(Configured) ) && Configured[0]
	    && IsPackageClassAvailable( Configured ) )
		return;	// what the user asked for is here — do not touch it

	for( INT i=0; i<Num; i++ )
	{
		if( IsPackageClassAvailable( Preference[i] ) )
		{
			debugf( "ufront: [%s] %s=%s is not in this build -> %s",
				Section, Key, Configured[0] ? Configured : "(unset)", Preference[i] );
			SetConfigString( Section, Key, Preference[i] );
			return;
		}
	}
	debugf( NAME_Warning, "ufront: [%s] %s=%s is not in this build and no replacement is either",
		Section, Key, Configured[0] ? Configured : "(unset)" );
}
#endif

static void SetRenderDeviceConfig( const char* Device )
{
	SetConfigString( "Engine.Engine", "GameRenderDevice",     Device );
	SetConfigString( "Engine.Engine", "WindowedRenderDevice", Device );
	SetConfigString( "Engine.Engine", "RenderDevice",         Device );
}

// Chooses the render device before Engine->Init(), on every target.
static void SelectRenderDevice()
{
	// 1) A command-line -*RENDERER switch (first present wins).
	for( INT i=0; i<ARRAY_COUNT(GRenderSwitches); i++ )
	{
		if( ParseParam( appCmdLine(), GRenderSwitches[i].Param ) )
		{
			const char* Device = GRenderSwitches[i].Device;
			if( IsPackageClassAvailable( Device ) )
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

	// 2) The persisted ini choice, if compiled in and either a hardware default or the user's own pick.
	char Persisted[256] = "", UserSel[256] = "";
	if( GetConfigString( "Engine.Engine", "GameRenderDevice", Persisted, ARRAY_COUNT(Persisted) ) && Persisted[0] )
	{
		// The marker ufrontUserRenderDevice is written by PersistRenderChoiceIfSwitched only after a -*RENDERER
		// switch initialized, so it tells a user's SoftDrv pick (sticky) from a shipped GameRenderDevice=SoftDrv
		// (no marker, so the first boot still takes a hardware default).
		const UBOOL bUserSelected =
			GetConfigString( "Engine.Engine", "ufrontUserRenderDevice", UserSel, ARRAY_COUNT(UserSel) )
			&& UserSel[0] && appStricmp( Persisted, UserSel )==0;
		if( IsPackageClassAvailable( Persisted ) && ( IsHardwareDefaultEligible( Persisted ) || bUserSelected ) )
		{
			debugf( "ufront: using the persisted %s render device %s", bUserSelected ? "user-selected" : "hardware", Persisted );
			SetRenderDeviceConfig( Persisted );
			return;
		}
		debugf( NAME_Warning, "ufront: persisted render device %s is not an eligible hardware default (unavailable or opt-in) -> preference order", Persisted );
	}

	// 3) Preference order — first compiled-in device wins.
	for( INT i=0; i<ARRAY_COUNT(GRenderPreference); i++ )
	{
		if( IsPackageClassAvailable( GRenderPreference[i] ) )
		{
			debugf( "ufront: default render device %s (preference order)", GRenderPreference[i] );
			SetRenderDeviceConfig( GRenderPreference[i] );
			return;
		}
	}
	debugf( NAME_Warning, "ufront: no known render device is compiled into this build!" );
}

// After Engine->Init(): persist a -*RENDERER switch choice so it sticks next boot. If the engine did not
// change GameRenderDevice during Init, mark it as the user's pick and save the config (SaveAllConfigs).
static void PersistRenderChoiceIfSwitched()
{
	if( GRequestedRenderDevice[0]==0 )
		return;
	char Active[256] = "";
	if( GetConfigString( "Engine.Engine", "GameRenderDevice", Active, ARRAY_COUNT(Active) ) && appStricmp( Active, GRequestedRenderDevice )==0 )
	{
		// Mark this as an explicit user selection, so SelectRenderDevice honors it next boot even when it is
		// not a hardware default (a user-chosen SoftDrv, as opposed to a shipped one).
		SetConfigString( "Engine.Engine", "ufrontUserRenderDevice", GRequestedRenderDevice );
		GConfigCache.SaveAllConfigs();
		debugf( "ufront: persisted render device %s as a user selection (sticks next boot)", GRequestedRenderDevice );
	}
	else
	{
		debugf( NAME_Warning, "ufront: render device changed from %s during init -> not persisted", GRequestedRenderDevice );
	}
}

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
	// Audio: NOpenALDrv on openal-soft (EFX, HRTF), as on native; only the music decoder thread is absent, so
	// the libxmp stream is pumped from Update().
	SetConfigString( "Engine.Engine", "AudioDevice", "NOpenALDrv.NOpenALAudioSubsystem" );
	// BSD sockets do not work in a browser: the net driver is the WebRTC driver (its transport is the host's
	// webrtc_* ABI).
	SetConfigString( "Engine.Engine", "NetworkDevice", "WebRTCDrv.WebRTCNetDriver" );
	// CaptureMouse=True: UGameEngine::Tick captures the pointer during play and releases it in a menu or pause;
	// SDL2's Emscripten port takes the browser pointer lock on the next click.
	SetConfigString( "NSDLDrv.NSDLClient", "CaptureMouse", "True" );
#endif

#if defined(PLATFORM_SDL) && !defined(__EMSCRIPTEN__)
	// CaptureMouse=True whatever the ini says (NSDLDrv has no click-drag look, so False would leave no mouse look):
	// UGameEngine::Tick captures the pointer during play and releases it in a menu or pause.
	SetConfigString( "NSDLDrv.NSDLClient", "CaptureMouse", "True" );
#endif

#ifndef __EMSCRIPTEN__
	// Replace any subsystem this build cannot load with the best one it has (NormalizeSubsystem), so the same
	// binary boots from a retail ini, from this repo's web-configured asset tree or from a user's own. Lists are
	// best-first; SoundDrv's silent subsystem comes last, for a build without NOpenALDrv.
	{
		static const char* const ViewportPreference[] = { "NSDLDrv.NSDLClient" };
		static const char* const AudioPreference[]    = { "NOpenALDrv.NOpenALAudioSubsystem",
		                                                  "SoundDrv.NullAudioSubsystem" };
		static const char* const NetPreference[]      = { "IpDrv.TcpNetDriver" };
		NormalizeSubsystem( "Engine.Engine", "ViewportManager", ViewportPreference, ARRAY_COUNT(ViewportPreference) );
		NormalizeSubsystem( "Engine.Engine", "AudioDevice",     AudioPreference,    ARRAY_COUNT(AudioPreference) );
		NormalizeSubsystem( "Engine.Engine", "NetworkDevice",   NetPreference,      ARRAY_COUNT(NetPreference) );
	}
#endif

	// Render-device selection, before Engine->Init() loads the subsystems.
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

	// Persist a -*RENDERER switch choice so it sticks next boot (no-op unless a switch was given and survived Init).
	PersistRenderChoiceIfSwitched();
#ifdef __EMSCRIPTEN__
	// Expose the engine to WebRTCDrv (listen-status push) and ue_exec_command.
	GEngineForExec = Engine;
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

// The per-frame yield. The engine thread must return to the worker's event loop every frame, because that is
// when the OffscreenCanvas is composited. It resumes from a setTimeout(0) timer: a timer task interleaves with
// rendering, a chain of MessagePort tasks does not. EM_ASYNC_JS registers the import with Asyncify itself.
EM_ASYNC_JS(void, ufront_yield_frame, (void), {
    await new Promise((resolve) => { setTimeout(resolve, 0); });
});


// Runs a console command and returns its captured output (FStringOut); the host calls it with ccall. The text
// is held in a static FString, so the returned pointer stays valid until the next call.
extern "C" char* EMSCRIPTEN_KEEPALIVE ue_exec_command( const char* Cmd )
{
	static FString Captured;
	Captured = "";
	if( GEngineForExec && Cmd )
	{
		FStringOut Out;
		GEngineForExec->Exec( Cmd, &Out );
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

		// Client frame-rate cap. UGameEngine::GetMaxTickRate() returns 0 for standalone play (no net driver), so
		// this loop would run as fast as it can, and ULevel::Tick floors DeltaSeconds at 0.001s (UnLevTic.cpp):
		// above 1000 fps the world advances more time than really elapsed. A net/demo cap (GetMaxTickRate>0)
		// wins; otherwise [Engine.Engine] FrameRateCap from the ini, default 120; <=0 means uncapped.
		FLOAT Cap = (FLOAT)MaxTickRate;
		if( Cap <= 0.f )
		{
			static FLOAT ClientCap = -1.f;
			if( ClientCap < -0.5f )
			{
				FLOAT V = 0.f;
				char Buf[64];
				// The ini key is the only setting. The launcher sets it with
				// -ini:System:[Engine.Engine]:FrameRateCap=<n>, only when the player changed it.
				if( GetConfigString( "Engine.Engine", "FrameRateCap", Buf, ARRAY_COUNT(Buf) ) )
					ClientCap = appAtof( Buf );
				else
					ClientCap = 120.f;
				debugf( NAME_Init, "ufront: client frame rate cap %.0f fps%s", ClientCap, ClientCap > 0.f ? "" : " (uncapped)" );
			}
			Cap = ClientCap;
		}
#ifdef __EMSCRIPTEN__
		// Every frame returns to the event loop (Asyncify suspends the wasm): emscripten_sleep for the budget left
		// under the cap, else ufront_yield_frame. emscripten_set_main_loop is not used: its simulate_infinite_loop
		// unwind collides with main()'s C++ try/catch.
		DOUBLE Budget = Cap > 0.f ? (1.0 / Cap) : 0.0;
		INT SleepMs = (INT)((Budget - (appSeconds() - OldTime)) * 1000.0);
		if( SleepMs > 1 )
			emscripten_sleep( SleepMs );
		else
			ufront_yield_frame();
#else
		if( Cap > 0.f )
		{
			DOUBLE Delta = (1.0/Cap) - (appSeconds()-OldTime);
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
	// Unreal runs from its System/ directory like a native install: Default.ini is preloaded at /System, and
	// config and the package Paths (../System, ../Maps, ...) resolve relative to the base dir. chdir before
	// appBaseDir() caches getcwd(), so appBaseDir() is "/System/".
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
