/*=============================================================================
	NSDLAndroid.cpp: the on-screen pad and the Back key of the Android app.

	GameActivity (Android/app/src/main/java/com/unreal1/android/GameActivity.java) draws a pad over
	SDL's surface and reports its buttons and sticks through the JNI functions below, from Android's
	UI thread. The client gives SDL a virtual game controller (SDL_JoystickAttachVirtualEx) and copies
	that state into it before every event poll, so the on-screen pad reaches the game as any SDL pad
	does (NSDLViewport.cpp: the menus' pad map, the Joy bindings). The overlay asks which layout to
	draw (menus or game) and whether the pad is in use through nativeTouchPadState, and asks for
	player one's place through nativeTouchPadWant: for the on-screen pad when the screen is touched,
	for a real pad when that pad is used.

	Back with nothing left to go back to (the entry level with the menu closed) asks the activity to
	confirm exiting the game (GameActivity.showExitDialog); everywhere else Back is Escape
	(NSDLViewport.cpp).

	Gyro aiming (GyroAim.java, the launcher's switch and sensitivities) sends the view's turn in degrees; in play
	ApplyGyro makes it the player's turn and look-up axes in rotation units, apart from the mouse's sensitivity and
	smoothing, and while a menu is up drops it.

	Off Android the client's functions here do nothing.
=============================================================================*/

#if defined(__ANDROID__)
#include <jni.h>
#include <atomic>
#endif

#include "NSDLDrv.h"

#if defined(__ANDROID__)

// What the overlay holds: SDL_GameControllerButton bits and SDL_GameControllerAxis values (sticks -1..1 with
// y positive down, as SDL's; triggers 0..1). Written by the UI thread, read by the engine's.
static std::atomic<DWORD>	GTouchButtons( 0 );
static std::atomic<FLOAT>	GTouchAxes[SDL_CONTROLLER_AXIS_MAX];

// For the overlay, written once a tick by the engine's thread.
enum
{
	TOUCHPAD_Attached	= 1,	// the virtual controller exists
	TOUCHPAD_InUse		= 2,	// it is the pad the game reads (no real pad has that place)
	TOUCHPAD_Menu		= 4,	// the menus are up
};
static std::atomic<INT>		GTouchState( 0 );

// Which pad the overlay last asked to be player one's: the on-screen pad after a touch on it, a real pad after that
// pad's input (the app sees a real pad's input even while the game reads the on-screen pad). Taken by SwitchTouchPad.
enum
{
	TOUCHPAD_WantNone	= 0,
	TOUCHPAD_WantTouch	= 1,
	TOUCHPAD_WantReal	= 2,
};
static std::atomic<INT>		GTouchPadWant( TOUCHPAD_WantNone );

// Gyro aiming's turn in view degrees, to the right and up: the UI thread adds, the engine's thread takes and clears it
// each tick.
static std::atomic<FLOAT>	GGyroYaw( 0.f );
static std::atomic<FLOAT>	GGyroPitch( 0.f );

static void AtomicAdd( std::atomic<FLOAT>& Value, FLOAT Delta )
{
	FLOAT Old = Value.load();
	while( !Value.compare_exchange_weak( Old, Old + Delta ) ) {}
}

extern "C"
{

JNIEXPORT void JNICALL Java_com_unreal1_android_GameActivity_nativeTouchPadButton( JNIEnv*, jclass, jint Button, jboolean Down )
{
	if( Button < 0 || Button >= SDL_CONTROLLER_BUTTON_MAX )
		return;
	if( Down )
		GTouchButtons.fetch_or( 1u << Button );
	else
		GTouchButtons.fetch_and( ~(1u << Button) );
}

JNIEXPORT void JNICALL Java_com_unreal1_android_GameActivity_nativeTouchPadAxis( JNIEnv*, jclass, jint Axis, jfloat Value )
{
	if( Axis < 0 || Axis >= SDL_CONTROLLER_AXIS_MAX )
		return;
	GTouchAxes[Axis].store( Value );
}

JNIEXPORT jint JNICALL Java_com_unreal1_android_GameActivity_nativeTouchPadState( JNIEnv*, jclass )
{
	return GTouchState.load();
}

JNIEXPORT void JNICALL Java_com_unreal1_android_GameActivity_nativeTouchPadWant( JNIEnv*, jclass, jboolean Touch )
{
	GTouchPadWant.store( Touch ? TOUCHPAD_WantTouch : TOUCHPAD_WantReal );
}

JNIEXPORT void JNICALL Java_com_unreal1_android_GameActivity_nativeGyroLook( JNIEnv*, jclass, jfloat YawDegrees, jfloat PitchDegrees )
{
	AtomicAdd( GGyroYaw, YawDegrees );
	AtomicAdd( GGyroPitch, PitchDegrees );
}

} // extern "C"

//
// The virtual controller: every SDL controller button and axis, in SDL's own order (SDL maps a virtual
// controller's buttons and axes to the controller's one for one), so the on-screen pad is read like any pad.
//
void UNSDLClient::InitTouchPad()
{
	guard(UNSDLClient::InitTouchPad);
	if( !TouchPad )
	{
		debugf( NAME_Init, "NSDLClient: no on-screen pad (TouchPad=False)" );
		return;
	}
	SDL_VirtualJoystickDesc Desc;
	SDL_zero( Desc );
	Desc.version	= SDL_VIRTUAL_JOYSTICK_DESC_VERSION;
	Desc.type		= SDL_JOYSTICK_TYPE_GAMECONTROLLER;
	Desc.naxes		= SDL_CONTROLLER_AXIS_MAX;
	Desc.nbuttons	= SDL_CONTROLLER_BUTTON_MAX;
	Desc.name		= "Unreal on-screen pad";
	INT Index = SDL_JoystickAttachVirtualEx( &Desc );
	if( Index < 0 )
	{
		debugf( NAME_Init, "NSDLClient: no on-screen pad: SDL_JoystickAttachVirtualEx failed: %s", SDL_GetError() );
		return;
	}
	TouchPadJoystick = SDL_JoystickOpen( Index );
	if( !TouchPadJoystick )
	{
		debugf( NAME_Init, "NSDLClient: no on-screen pad: SDL_JoystickOpen failed: %s", SDL_GetError() );
		SDL_JoystickDetachVirtual( Index );
		return;
	}
	FeedTouchPad();
	// The controller arrives as an SDL_CONTROLLERDEVICEADDED event and is opened there when no pad is open
	// (UNSDLViewport::TickInput).
	debugf( NAME_Init, "NSDLClient: on-screen pad attached (virtual SDL controller, device %i)", Index );
	unguard;
}

void UNSDLClient::ExitTouchPad()
{
	guard(UNSDLClient::ExitTouchPad);
	if( !TouchPadJoystick )
		return;
	SDL_Joystick* Joystick = (SDL_Joystick*)TouchPadJoystick;
	SDL_JoystickID Id = SDL_JoystickInstanceID( Joystick );
	SDL_JoystickClose( Joystick );
	TouchPadJoystick = NULL;
	for( INT i=0; i<SDL_NumJoysticks(); i++ )
		if( SDL_JoystickGetDeviceInstanceID(i)==Id && SDL_JoystickIsVirtual(i) )
		{
			SDL_JoystickDetachVirtual( i );
			break;
		}
	GTouchState.store( 0 );
	unguard;
}

//
// The overlay's state into the virtual controller. SDL applies it when it next updates its joysticks (every
// event poll), so this runs right before TickInput polls.
//
void UNSDLClient::FeedTouchPad()
{
	if( !TouchPadJoystick )
		return;
	SDL_Joystick* Joystick = (SDL_Joystick*)TouchPadJoystick;
	DWORD Buttons = GTouchButtons.load();
	for( INT b=0; b<SDL_CONTROLLER_BUTTON_MAX; b++ )
		SDL_JoystickSetVirtualButton( Joystick, b, (Buttons >> b) & 1 );
	for( INT a=0; a<SDL_CONTROLLER_AXIS_MAX; a++ )
	{
		FLOAT Value = GTouchAxes[a].load();
		INT Raw;
		if( a==SDL_CONTROLLER_AXIS_TRIGGERLEFT || a==SDL_CONTROLLER_AXIS_TRIGGERRIGHT )
			Raw = appRound( -32768.f + Clamp( Value, 0.f, 1.f ) * 65535.f );	// a trigger's whole range is released..pressed
		else
			Raw = appRound( Clamp( Value, -1.f, 1.f ) * 32767.f );
		SDL_JoystickSetVirtualAxis( Joystick, a, (Sint16)Clamp( Raw, -32768, 32767 ) );
	}
}

UBOOL UNSDLClient::IsTouchPad( INT InstanceId )
{
	return TouchPadJoystick && SDL_JoystickInstanceID( (SDL_Joystick*)TouchPadJoystick )==InstanceId;
}

void UNSDLClient::PublishTouchPadState()
{
	INT State = 0;
	if( TouchPadJoystick )
	{
		State |= TOUCHPAD_Attached;
		if( Controller && IsTouchPad( SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( Controller ) ) ) )
			State |= TOUCHPAD_InUse;
	}
	if( MenuActive() )
		State |= TOUCHPAD_Menu;
	GTouchState.store( State );
}

//
// The overlay's last request (GTouchPadWant) into the pad the game reads: the on-screen pad, or the first real pad
// present. Returns whether the pad in use was closed, so the viewport releases what it held.
//
UBOOL UNSDLClient::SwitchTouchPad()
{
	guard(UNSDLClient::SwitchTouchPad);
	const INT Want = GTouchPadWant.exchange( TOUCHPAD_WantNone );
	if( Want == TOUCHPAD_WantNone || !TouchPadJoystick || !UseJoystick )
		return 0;
	const SDL_JoystickID TouchId = SDL_JoystickInstanceID( (SDL_Joystick*)TouchPadJoystick );
	const UBOOL TouchInUse = Controller && SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( Controller ) ) == TouchId;
	if( (Want == TOUCHPAD_WantTouch) == TouchInUse )
		return 0;
	INT Target = -1;
	for( INT i=0; i<SDL_NumJoysticks() && Target<0; i++ )
	{
		const UBOOL IsTouch = SDL_JoystickGetDeviceInstanceID( i ) == TouchId;
		if( Want == TOUCHPAD_WantTouch ? IsTouch : (!IsTouch && SDL_IsGameController( i )) )
			Target = i;
	}
	if( Target < 0 )
		return 0;
	const UBOOL Closed = Controller && ClosePad( SDL_JoystickInstanceID( SDL_GameControllerGetJoystick( Controller ) ) );
	if( !OpenPad( Target, NULL ) )
		for( INT i=0; i<SDL_NumJoysticks() && !Controller; i++ )
			OpenPad( i, NULL );
	debugf( NAME_Init, "NSDLClient: %s", Want == TOUCHPAD_WantTouch ? "the on-screen pad takes the place of the real pad (a touch)" : "a real pad takes the place of the on-screen pad (its input)" );
	return Closed;
	unguard;
}

// The gyro's fractions of a rotation unit not applied yet (yaw, pitch), and with -UFRONTLOG its totals in play.
static FLOAT	GGyroCarry[2] = { 0.f, 0.f };
static DOUBLE	GGyroDegrees[2] = { 0.0, 0.0 };
static INT		GGyroUnits[2] = { 0, 0 };
static DOUBLE	GGyroLastTime = 0.0;
static UBOOL	GGyroUnlogged = 0;
// The last tick's time: the turn sent while the game did not tick for half a second (a load) is not aim.
static DOUBLE	GGyroTickTime = 0.0;

// Degrees as whole rotation units (65536 a turn, toward zero), the fraction carried to the next tick.
static INT GyroUnits( FLOAT Degrees, FLOAT& Carry )
{
	const FLOAT Units = Degrees * ( 65536.f / 360.f ) + Carry;
	const INT Whole = (INT)Units;
	Carry = Units - (FLOAT)Whole;
	return Whole;
}

// The axis value that turns exactly Units when the script turns UnitsPerAxis times the axis, cut to whole units: half
// a unit past it, so the cut lands on it.
static FLOAT GyroAxis( INT Units, FLOAT UnitsPerAxis )
{
	if( Units == 0 )
		return 0.f;
	return ( (FLOAT)Units + ( Units > 0 ? 0.5f : -0.5f ) ) / UnitsPerAxis;
}

//
// Gyro aiming, in play: whole rotation units as the player's turn and look-up axes, which no mouse setting scales.
// The player turns 32 * DeltaTime * aTurn units a tick after ReadInput's 20 / DeltaTime and the player state's 0.24,
// aLookUp the same: 153.6 units per unit of the axis. While a menu is up, or what came while the game did not tick
// for half a second (a load), the turn is dropped. With -UFRONTLOG the totals are logged once the gyro has rested for
// a quarter of a second.
//
void UNSDLClient::ApplyGyro( UViewport* Viewport )
{
	guard(UNSDLClient::ApplyGyro);
	const FLOAT Yaw = GGyroYaw.exchange( 0.f ), Pitch = GGyroPitch.exchange( 0.f );
	APlayerPawn* Actor = Viewport ? Viewport->Actor : NULL;
	const DOUBLE Now = appSeconds();
	const UBOOL Ticking = Now - GGyroTickTime < 0.5;
	GGyroTickTime = Now;
	const UBOOL InPlay = Ticking && Actor && !Actor->bShowMenu && !MenuActive();
	if( !InPlay )
	{
		GGyroCarry[0] = GGyroCarry[1] = 0.f;
	}
	else if( Yaw != 0.f || Pitch != 0.f )
	{
		const FLOAT UnitsPerAxis = 32.f * 20.f * 0.24f;
		const INT YawUnits = GyroUnits( Yaw, GGyroCarry[0] ), PitchUnits = GyroUnits( Pitch, GGyroCarry[1] );
		Actor->aTurn += GyroAxis( YawUnits, UnitsPerAxis );
		Actor->aLookUp += GyroAxis( PitchUnits, UnitsPerAxis );
		GGyroDegrees[0] += Yaw;
		GGyroDegrees[1] += Pitch;
		GGyroUnits[0] += YawUnits;
		GGyroUnits[1] += PitchUnits;
		GGyroLastTime = Now;
		GGyroUnlogged = 1;
		return;
	}
	if( GGyroUnlogged && Now - GGyroLastTime >= 0.25 )
	{
		GGyroUnlogged = 0;
		if( GUfrontLog )
			debugf( NAME_Log, "Gyro: %.3f degrees right and %.3f up in play in all, %i yaw and %i pitch units",
				GGyroDegrees[0], GGyroDegrees[1], GGyroUnits[0], GGyroUnits[1] );
	}
	unguard;
}

//
// Whether Back has nothing left to go back to: no game running (the intro map, [URL] LocalMap, or the entry level)
// with the menu up on its first page (the HUD's main menu with no parent menu). At the flyby with the menu closed,
// Back is Escape and opens it.
//
UBOOL UNSDLClient::BackLeavesGame()
{
	guard(UNSDLClient::BackLeavesGame);
	UGameEngine* GameEngine = Cast<UGameEngine>( Engine );
	ULevel* Level = GameEngine ? GameEngine->GLevel : NULL;
	if( !Level || !MenuActive() )
		return 0;
	if( Level!=GameEngine->GEntry && appStricmp( *Level->URL.Map, *FURL::DefaultLocalMap )!=0 )
		return 0;
	AMenu* Menu = NULL;
	for( INT i=0; i<Viewports.Num() && !Menu; i++ )
		if( Viewports(i)->Actor && Viewports(i)->Actor->myHUD )
			Menu = Viewports(i)->Actor->myHUD->MainMenu;
	const UBOOL Leaves = Menu && !Menu->ParentMenu;
	debugf( NAME_Log, "NSDLClient: Back on the entry level's menu (%s)%s", Menu ? Menu->GetClass()->GetName() : "no menu", Leaves ? ": asking to exit" : "" );
	return Leaves;
	unguard;
}

//
// GameActivity.showExitDialog on the activity: its dialog quits through SDL (SDL_QUIT, then the engine's own exit).
//
void UNSDLClient::AskToExit()
{
	guard(UNSDLClient::AskToExit);
	JNIEnv* Env = (JNIEnv*)SDL_AndroidGetJNIEnv();
	jobject Activity = (jobject)SDL_AndroidGetActivity();
	if( !Env || !Activity )
	{
		debugf( NAME_Warning, "NSDLClient: no activity to ask whether to exit" );
		return;
	}
	jclass Class = Env->GetObjectClass( Activity );
	jmethodID Show = Env->GetMethodID( Class, "showExitDialog", "()V" );
	if( Show )
		Env->CallVoidMethod( Activity, Show );
	if( Env->ExceptionCheck() )
	{
		Env->ExceptionClear();
		debugf( NAME_Warning, "NSDLClient: GameActivity.showExitDialog failed" );
	}
	Env->DeleteLocalRef( Class );
	Env->DeleteLocalRef( Activity );
	unguard;
}

#else

void UNSDLClient::InitTouchPad() {}
void UNSDLClient::ExitTouchPad() {}
void UNSDLClient::FeedTouchPad() {}
UBOOL UNSDLClient::IsTouchPad( INT InstanceId ) { return 0; }
void UNSDLClient::PublishTouchPadState() {}
UBOOL UNSDLClient::SwitchTouchPad() { return 0; }
void UNSDLClient::ApplyGyro( UViewport* Viewport ) {}
UBOOL UNSDLClient::BackLeavesGame() { return 0; }
void UNSDLClient::AskToExit() {}

#endif
