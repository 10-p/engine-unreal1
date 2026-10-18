/*=============================================================================
	UnCheats.cpp: the position cheats WHEREAMI and TELEPORT.

	Console commands (the console, a key binding, the web's ue_exec_command):
	  WHEREAMI                         the local player's position and view:
	                                   "<x> <y> <z> <yaw> <pitch>" (x y z: world units, one
	                                   decimal, the pawn's location; yaw 0..65535, pitch
	                                   -32768..32767: Unreal rotation units, the view
	                                   rotation), on the console, on the screen
	                                   (ClientMessage) and in the log as
	                                   "Cheat: WHEREAMI <x> <y> <z> <yaw> <pitch>"
	  TELEPORT <x> <y> <z> [<yaw> [<pitch>]]
	                                   puts the local player's pawn there (WHEREAMI's output
	                                   pasted back gives the same place and view); without yaw
	                                   and pitch the view is kept.

	The guard: allowed for the local player of a standalone game and of a listen server
	(the host is the server); every network client and a dedicated server refuse:
	"Cheat: <COMMAND> refused: ..." in the log and on the console. Test harnesses set
	[Engine.GameEngine] AllowNetCheats=True in that process's ini (-ini:System:...); it
	must never be in a shipped ini. Every command that runs logs one "Cheat: ..." line.

	Called first by UGameEngine::Exec: the web's ue_exec_command reaches the engine
	only, and the typed console reaches it before the scripts (UViewport::Exec calls
	Client->Engine->Exec ahead of the console's and the pawn's script exec functions).
=============================================================================*/

#include "EnginePrivate.h"

// "Cheat: " is part of the text, on NAME_Log (never suppressed). The console (Out) gets the short form, unless it
// is the log itself.
static void CheatLog( FOutputDevice* Out, const char* Text )
{
	debugf( NAME_Log, "Cheat: %s", Text );
	if( Out && Out != GSystem )
		Out->Log( Text );
}

static APlayerPawn* FirstLocalPlayer( UEngine* Engine )
{
	if( Engine && Engine->Client && Engine->Client->Viewports.Num() && Engine->Client->Viewports(0) )
		return Engine->Client->Viewports(0)->Actor;
	return NULL;
}

static void WhereAmI( APlayerPawn* P, char* Out )
{
	INT Pitch = P->ViewRotation.Pitch & 65535;
	if( Pitch >= 32768 )
		Pitch -= 65536;
	appSprintf( Out, "%.1f %.1f %.1f %i %i", P->Location.X, P->Location.Y, P->Location.Z, P->ViewRotation.Yaw & 65535, Pitch );
}

// May cheats run for this player now? Logs the refusal, or the "Cheat:" line of the command that runs.
static UBOOL CheatsAllowed( APlayerPawn* P, const char* Command, const char* Args, FOutputDevice* Out )
{
	INT NetMode = ( P && P->Level ) ? P->Level->NetMode : NM_DedicatedServer;
	UBOOL ByAllowNetCheats = 0;
	UBOOL Allowed = P && ( NetMode==NM_Standalone || NetMode==NM_ListenServer );
	if( !Allowed )
	{
		UBOOL NetCheats = 0;
		GetConfigBool( "Engine.GameEngine", "AllowNetCheats", NetCheats );
		Allowed = ByAllowNetCheats = NetCheats;
	}
	char Line[512];
	if( !Allowed )
	{
		appSprintf( Line, "%s refused: cheats are only for a standalone game or the host of a listen server", Command );
		CheatLog( Out, Line );
		return 0;
	}
	while( Args && *Args==' ' )
		Args++;
	debugf( NAME_Log, "Cheat: %s %s%s", Command, Args ? Args : "", ByAllowNetCheats ? " (AllowNetCheats: a test setting)" : "" );
	return 1;
}

ENGINE_API UBOOL UfrontCheatExec( UEngine* Engine, const char* Cmd, FOutputDevice* Out )
{
	guard(UfrontCheatExec);
	const char* Str = Cmd;
	char Pos[256], Line[512];
	if( ParseCommand( &Str, "WHEREAMI" ) )
	{
		APlayerPawn* P = FirstLocalPlayer( Engine );
		if( !P || P->bDeleteMe )
		{
			if( CheatsAllowed( P, "WHEREAMI", "", Out ) )
				CheatLog( Out, "WHEREAMI failed: no local player" );
			return 1;
		}
		WhereAmI( P, Pos );
		if( !CheatsAllowed( P, "WHEREAMI", Pos, Out ) )
			return 1;
		if( Out && Out != GSystem )
			Out->Log( Pos );
		P->eventClientMessage( Pos );
		return 1;
	}
	if( ParseCommand( &Str, "TELEPORT" ) )
	{
		char Tok[5][64];
		INT N = 0;
		const char* Args = Str;
		while( N<5 && ParseToken( Str, Tok[N], ARRAY_COUNT(Tok[N]), 0 ) )
			N++;
		if( N==0 )
			return 0;
		APlayerPawn* P = FirstLocalPlayer( Engine );
		if( !CheatsAllowed( P, "TELEPORT", Args, Out ) )
			return 1;
		if( N<3 )
		{
			CheatLog( Out, "TELEPORT failed: usage TELEPORT <x> <y> <z> [<yaw> [<pitch>]]" );
			return 1;
		}
		if( !P || P->bDeleteMe || !P->XLevel || P->Level->NetMode==NM_Client )
		{
			CheatLog( Out, "TELEPORT failed: no local pawn this process moves (a network client cannot)" );
			return 1;
		}
		FVector To( appAtof(Tok[0]), appAtof(Tok[1]), appAtof(Tok[2]) );
		UBOOL Moved = P->XLevel->FarMoveActor( P, To, 0, 0 );
		P->Velocity = FVector(0,0,0);
		P->Acceleration = FVector(0,0,0);
		if( N>=4 )
		{
			INT Yaw = appAtoi( Tok[3] );
			INT Pitch = N>=5 ? appAtoi( Tok[4] ) : 0;
			P->Rotation = FRotator( 0, Yaw, 0 );
			P->ViewRotation = FRotator( Pitch, Yaw, 0 );
		}
		WhereAmI( P, Pos );
		appSprintf( Line, "TELEPORT %s: %s", Moved ? "ok" : "failed (no room there)", Pos );
		CheatLog( Out, Line );
		return 1;
	}
	return 0;
	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
