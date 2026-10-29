#include "Engine.h"
#include "SDL2/SDL.h"

/*-----------------------------------------------------------------------------
	Defines.
-----------------------------------------------------------------------------*/

#ifdef NSDLDRV_EXPORTS
#define NSDLDRV_API DLL_EXPORT
#else
#define NSDLDRV_API DLL_IMPORT
#endif


/*-----------------------------------------------------------------------------
	UNSDLViewport.
-----------------------------------------------------------------------------*/

extern NSDLDRV_API UBOOL GTickDue;

class FNSDLInputTest;

//
// A SDL2 viewport.
//
class NSDLDRV_API UNSDLViewport : public UViewport
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT( UNSDLViewport, UViewport, CLASS_Transient )
	NO_DEFAULT_CONSTRUCTOR( UNSDLViewport )

	// Constructors.
	UNSDLViewport( ULevel* InLevel, class UNSDLClient* InClient );
	static void InternalClassInitializer( UClass* Class );

	// UObject interface.
	virtual void Destroy() override;

	// UViewport interface.
	virtual UBOOL Lock( FPlane FlashScale, FPlane FlashFog, FPlane ScreenClear, DWORD RenderLockFlags, BYTE* HitData=NULL, INT* HitSize=0 ) override;
	virtual void Unlock( UBOOL Blit ) override;
	virtual UBOOL Exec( const char* Cmd, FOutputDevice* Out ) override;
	virtual void Repaint() override;
	virtual void SetModeCursor() override;
	virtual void UpdateWindow() override;
	virtual void OpenWindow( void* ParentWindow, UBOOL Temporary, INT NewX, INT NewY, INT OpenX, INT OpenY ) override;
	virtual void CloseWindow() override;
	virtual void UpdateInput( UBOOL Reset ) override;
	virtual void MakeCurrent() override;
	virtual void MakeFullscreen( INT NewX, INT NewY, UBOOL UpdateProfile ) override;
	virtual void* GetWindow() override;
	virtual void SetMouseCapture( UBOOL Capture, UBOOL Clip, UBOOL FocusOnly ) override;

	// UNSDLViewport interface.
	void SetClientSize( INT NewX, INT NewY, UBOOL UpdateProfile );
	void EndFullscreen();
	UBOOL TickInput(); // returns true if the viewport has requested death
	UBOOL InMenu(); // the console is in its Menuing state (the pad's menu map applies)

private:
	// Static variables.
	static BYTE KeyMap[SDL_NUM_SCANCODES]; // SDL_Scancode -> EInputKey map
	static const BYTE MouseButtonMap[6]; // SDL_BUTTON_ -> EInputKey map
	static const BYTE JoyButtonMap[SDL_CONTROLLER_BUTTON_MAX]; // SDL_GameControllerButton -> EInputKey map
	static const BYTE JoyButtonMapUI[SDL_CONTROLLER_BUTTON_MAX];  // SDL_GameControllerButton -> EInputKey map for UI
	static const BYTE JoyAxisMap[SDL_CONTROLLER_AXIS_MAX]; // SDL_GameControllerAxis -> EInputKey map
	static const FLOAT JoyAxisDefaultScale[SDL_CONTROLLER_AXIS_MAX];
	static const SWORD JoyAxisPressThreshold = 8192;

	// Variables.
	class UNSDLClient* Client;
	SDL_Window* hWnd;
	SDL_Renderer* SDLRen; // for accelerated SoftDrv
	SDL_Texture* SDLTex; // for use with the above renderer
	DWORD FramebufferFormat; // pixel format of the software framebuffer: SDLTex, or SoftFB on the threaded web build
	SDL_GLContext GLCtx; // for NOpenGLDrv and NOpenGLESDrv; on the threaded web build, an Emscripten WebGL handle
	// The configured render device could not start (no GL window or context, or its Init failed): SoftDrv renders
	// for the rest of the run, in a window without GL. The ini keeps the configured device.
	UBOOL SoftwareFallback;
	UBOOL FallBackToSoftware();
#if defined(__EMSCRIPTEN__) && defined(__EMSCRIPTEN_PTHREADS__)
	// On a worker there is no SDL renderer (SDL's is GLES-backed and its proxied eglCreateContext blocks the
	// engine thread), so SoftDrv renders into this buffer and Unlock blits it to the transferred OffscreenCanvas
	// through a 2D context.
	BYTE* SoftFB;
	INT SoftFBBytes;
#endif
	UBOOL Destroyed;
	INT DisplayIndex;
	SDL_Rect DisplaySize;
	SWORD JoyAxis[SDL_CONTROLLER_AXIS_MAX];
	SWORD MoveStick[2];	// the left stick's X and Y before any dead zone: the move stick (TickMoveStick)
	void UpdatePadAxis( INT Axis, INT NewValue );	// one stick or trigger value, read from the pad's state
	void TickMoveStick( FLOAT DeltaTime );
	void ReleasePad();	// every pad key up and the axes zeroed (the pad went, or UseJoystick went off)
	FNSDLInputTest* InputTest;	// the input test channel (-INPUTTEST=), NULL without it
	friend class FNSDLInputTest;
	UBOOL QuitRequested;
	DOUBLE InputUpdateTime;	// DOUBLE: see TickInput

	// Info saved during captures and fullscreen sessions.
	INT SavedX, SavedY;

	// UNSDLViewport private methods.
	static void InitKeyMap();
	UBOOL CauseInputEvent( INT iKey, EInputAction Action, FLOAT Delta=0.0 );
};

/*-----------------------------------------------------------------------------
	UNSDLClient.
-----------------------------------------------------------------------------*/

//
// SDL2 implementation of the client.
//
class NSDLDRV_API UNSDLClient : public UClient, public FNotifyHook
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT( UNSDLClient, UClient, CLASS_Transient|CLASS_Config )

	// Configuration.
	INT DefaultDisplay;
	UBOOL StartupFullscreen;
	UBOOL BorderlessFullscreen;	// fullscreen is the display's own mode at its size; off: a mode change to ViewportX/Y
	UBOOL UseJoystick;
	UBOOL UseMouse;		// mouse events reach the game (False: ignored, never captured; the compiled default on Android)
	UBOOL TouchPad;		// Android: the on-screen pad, a virtual SDL controller fed by the app (NSDLAndroid.cpp)
	UBOOL InvertY;
	UBOOL InvertV;
	FLOAT ScaleXYZ;
	FLOAT ScaleRUV;
	FLOAT DeadZoneXYZ;
	FLOAT DeadZoneRUV;
	FLOAT PadWalkTilt;	// the move stick out of its dead zone but tilted less than this (0..1) walks; 0: never

	// Constructors.
	UNSDLClient();
	static void InternalClassInitializer( UClass* Class );

	// UObject interface.
	virtual void Destroy() override;
	virtual void PostEditChange() override;
	virtual void ShutdownAfterError() override;

	// UClient interface.
	virtual void Init( UEngine* InEngine ) override;
	virtual void ShowViewportWindows( DWORD ShowFlags, int DoShow ) override;
	virtual void EnableViewportWindows( DWORD ShowFlags, int DoEnable ) override;
	virtual void Poll() override;
	virtual UViewport* CurrentViewport() override;
	virtual UBOOL Exec( const char* Cmd, FOutputDevice* Out=GSystem ) override;
	virtual void Tick() override;
	virtual UViewport* NewViewport( class ULevel* InLevel, const FName Name ) override;
	virtual void EndFullscreen() override;

	// UNSDLClient interface.
	void TryRenderDevice( UViewport* Viewport, const char* ClassName, UBOOL Fullscreen );
	const TArray<SDL_Rect>& GetDisplayResolutions();
	inline SDL_GameController* GetController() { return Controller; }
	// Open the pad at SDL device index DeviceIndex when none is open and UseJoystick is on; on the web Held gets the
	// buttons already down at the connect (W3C indices). ClosePad closes it when InstanceId is ours.
	UBOOL OpenPad( INT DeviceIndex, DWORD* Held );
	UBOOL ClosePad( INT InstanceId );
	inline const SDL_DisplayMode& GetDefaultDisplayMode() const { return DefaultDisplayMode; }
	UBOOL MenuActive(); // a viewport's console is in its Menuing state

	// The on-screen pad (NSDLAndroid.cpp; nothing off Android).
	void InitTouchPad();
	void ExitTouchPad();
	void FeedTouchPad();
	UBOOL IsTouchPad( INT InstanceId );
	void PublishTouchPadState();
	UBOOL SwitchTouchPad();

	// Gyro aiming's turn into the viewport's player, once a tick (NSDLAndroid.cpp; nothing off Android).
	void ApplyGyro( UViewport* Viewport );

	// Android's Back with nothing left to go back to (NSDLAndroid.cpp; nothing off Android).
	UBOOL BackLeavesGame();
	void AskToExit();

private:
	// Variables.
	SDL_GameController* Controller;
	void* TouchPadJoystick; // SDL_Joystick* of the on-screen pad (a virtual SDL controller), Android
	SDL_DisplayMode DefaultDisplayMode;
	TArray<SDL_Rect> DisplayResolutions;
};
