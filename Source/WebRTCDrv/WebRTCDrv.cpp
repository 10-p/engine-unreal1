/*=============================================================================
	WebRTCDrv.cpp: WebRTC network driver for UE1 (Emscripten/browser multiplayer).

	Structured after IpDrv.cpp (UTcpNetDriver/UTcpipConnection), with the glue's
	`webrtc_*` C ABI (the baked shim, forwarding to the host-injected
	Module.UE.net) as the transport instead of BSD sockets.
=============================================================================*/

#include "Engine.h"
#include "UnNet.h"
#include "WebRTCLan.h"
#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

// Package.
IMPLEMENT_PACKAGE(WebRTCDrv);

// WebRTC connection state (matches the glue ABI + webrtc_get_connection_state).
#define WEBRTC_STATE_INVALID    0
#define WEBRTC_STATE_CONNECTING 1
#define WEBRTC_STATE_OPEN       2
#define WEBRTC_STATE_CLOSED     3

// This game's peer-ID namespace prefix. It must match the host's network.peerPrefix (the glue registers the
// listen peer as "<prefix>-<room>-srv").
#define WEBRTC_PEER_PREFIX      "u1"

/*-----------------------------------------------------------------------------
	extern "C" transport bridge (WASM <-> JS).
	On Emscripten these resolve to the glue's baked shim, which forwards to
	Module.UE.net.*. On native they are inline no-op stubs.
	UE1 does not use the deferred-connect pair (webrtc_deferred_open /
	webrtc_check_pending_connect) — the guest joins via the startup-URL flow
	(the frontend passes "<room>?connect" as the engine's first argument), so no
	post-boot poll is needed.
-----------------------------------------------------------------------------*/
#ifdef __EMSCRIPTEN__
extern "C"
{
	void webrtc_init_listen( const char* roomName );
	void webrtc_connect( const char* peerId );
	int  webrtc_send( int connId, const void* data, int len );
	int  webrtc_recv( int connId, void* buffer, int maxLen );
	int  webrtc_accept_connection();
	void webrtc_close( int connId );
	void webrtc_destroy();
	int  webrtc_get_connection_state( int connId );
	void webrtc_set_listen_status( const char* name, const char* map, int players, int maxPlayers, const char* gametype );
}
#else
inline void webrtc_init_listen( const char* ) {}
inline void webrtc_connect( const char* ) {}
inline int  webrtc_send( int, const void*, int ) { return 0; }
inline int  webrtc_recv( int, void*, int ) { return 0; }
inline int  webrtc_accept_connection() { return 0; }
inline void webrtc_close( int ) {}
inline void webrtc_destroy() {}
inline int  webrtc_get_connection_state( int ) { return WEBRTC_STATE_INVALID; }
inline void webrtc_set_listen_status( const char*, const char*, int, int, const char* ) {}
#endif

// Engine pointer for the listen-server status push (set in SDLLaunch.cpp after InitEngine).
extern UEngine* GWebRTCEngine;

/*-----------------------------------------------------------------------------
	UWebRTCConnection — a UNetConnection over one WebRTC data channel (by connId).
-----------------------------------------------------------------------------*/
class DLL_EXPORT UWebRTCConnection : public UNetConnection
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UWebRTCConnection,UNetConnection,CLASS_Config|CLASS_Transient)
	NO_DEFAULT_CONSTRUCTOR(UWebRTCConnection)

	// Variables.
	INT		WebRTCConnId;
	UBOOL	OpenedLocally;

	// Constructor.
	UWebRTCConnection( INT InWebRTCConnId, UNetDriver* InDriver, EConnectionState InState, UBOOL InOpenedLocally )
	:	UNetConnection	( InDriver )
	,	WebRTCConnId	( InWebRTCConnId )
	,	OpenedLocally	( InOpenedLocally )
	{
		guard(UWebRTCConnection::UWebRTCConnection);
		State     = InState;
		MaxPacket = UNetConnection::MAX_PACKET_SIZE;
		unguard;
	}
	void Destroy()
	{
		guard(UWebRTCConnection::Destroy);
		if( Channels[0] )
		{
			Channels[0]->Close();
			FlushNet();
		}
		if( WebRTCConnId > 0 )
			webrtc_close( WebRTCConnId );
		Super::Destroy();
		unguard;
	}

	// UNetConnection interface.
	void ReadInput( FLOAT ) {}
	void FlushNet( UBOOL Duplicate=0 )
	{
		guard(UWebRTCConnection::FlushNet);
		Super::FlushNet( Duplicate );
		if( OutNum>0 || Driver->Time-LastSendTime>Driver->KeepAliveTime )
		{
			LastSendTime = Driver->Time;
			// WebRTCConnId < 0 means this connection has been detached from the transport (a newer
			// client driver took the one client slot; see UWebRTCNetDriver::Init). Drop its traffic
			// rather than posting it into somebody else's session.
			if( OutNum > 0 && WebRTCConnId >= 0 )
				webrtc_send( WebRTCConnId, OutData, OutNum );
			QueuedBytes += OutNum + 28; // ~UDP header, for the rate limiter (parity with UTcpipConnection)
			OutNum = 0;
		}
		unguard;
	}
	char* Describe( char* String256 )
	{
		guard(UWebRTCConnection::Describe);
		const char* S = (State==USOCK_Open) ? "Open" : (State==USOCK_Pending) ? "Pending" : (State==USOCK_Closed) ? "Closed" : "Unknown";
		appSprintf( String256, "webrtc:%i state: %s", WebRTCConnId, S );
		return String256;
		unguard;
	}
	void AssertValid()
	{
		guard(UWebRTCConnection::AssertValid);
		check( State==USOCK_Closed || State==USOCK_Pending || State==USOCK_Open );
		unguard;
	}
};
IMPLEMENT_CLASS(UWebRTCConnection);

/*-----------------------------------------------------------------------------
	UWebRTCNetDriver.
-----------------------------------------------------------------------------*/
class UWebRTCNetDriver;

// The client has exactly one transport slot: the glue hands every client connect the same conn id (0),
// so only one client driver can own the wire at a time. Tracked here so a second Init(connect) can take
// the slot cleanly instead of silently sharing it. Server-side ids are allocated per peer and unaffected.
static UWebRTCNetDriver* GClientDriver = NULL;

class DLL_EXPORT UWebRTCNetDriver : public UNetDriver
{
	DECLARE_CLASS(UWebRTCNetDriver,UNetDriver,CLASS_Transient|CLASS_Config)

	// Variables.
	FString	RoomName;
	FString	ServerName;      // ?ServerName= for browser discovery
	UBOOL	bIsListenServer;
	// Set when a newer client driver has taken the one client transport slot from us. See Init/Destroy.
	UBOOL	bDetached;
	// Pipes the level told us to ignore (not refuse), re-offered on a later Tick. See Tick().
	TArray<INT>	ParkedAccepts;

	// UObject interface.
	void Destroy()
	{
		guard(UWebRTCNetDriver::Destroy);
		debugf( NAME_Exit, "[WebRTC] Destroy: room='%s'%s", *RoomName, bDetached ? " (detached — transport left alone)" : "" );
		// The parked pipes are about to be closed wholesale by webrtc_destroy(); just drop our list.
		ParkedAccepts.Empty();
		if( GClientDriver==this )
			GClientDriver = NULL;
		// A departing driver must not destroy the arriving driver's transport: webrtc_destroy() is global. On a
		// client the two drivers overlap: Init(connect) for the pending level runs first and the old level's
		// driver is destroyed later, inside LoadMap, when conn 0 already belongs to the new pending connection.
		// Init marks that old driver detached, and it leaves the transport alone. On the host the old driver is
		// destroyed before Init(listen).
		if( !bDetached )
			webrtc_destroy();
		Super::Destroy();
		unguard;
	}

	// UNetDriver interface.
	UBOOL Init( UBOOL Connect, FNetworkNotify* InNotify, FURL& URL, char* Error256 );
	void Tick();
	UBOOL IsInternet() { return 1; }
	UBOOL Exec( const char* Cmd, FOutputDevice* Out=GSystem ) { return 0; }

	UWebRTCConnection* GetServerConnection() { return (UWebRTCConnection*)ServerConnection; }
};
IMPLEMENT_CLASS(UWebRTCNetDriver);

/*-----------------------------------------------------------------------------
	UWebRTCNetDriver::Init.
-----------------------------------------------------------------------------*/
UBOOL UWebRTCNetDriver::Init( UBOOL Connect, FNetworkNotify* InNotify, FURL& URL, char* Error256 )
{
	guard(UWebRTCNetDriver::Init);

	// Init base.
	if( !UNetDriver::Init( Connect, InNotify, URL, Error256 ) )
		return 0;

	// The WebRTC handshake (signaling + ICE) and map downloads take far longer than a TCP connect. The
	// engine's sleeps stop while a package streams, so LastReceiveTime goes stale; generous timeouts prevent
	// an immediate disconnect on the first post-download tick (ICE monitors the real link underneath).
	ConnectionTimeout     = 300.f;
	InitialConnectTimeout = 600.f;

	if( Connect )
	{
		// Client. URL.Host carries the room (HostID); the server registered as "<prefix>-<room>-srv".
		bIsListenServer = 0;
		bDetached       = 0;
		RoomName = URL.Host;
		// A join from "Find Local Servers" arrives as a dotted address, not a room: the
		// stock menu can only carry an IPv4 string through to the travel URL. The synthetic LAN handed
		// that address out, so it can hand the room back. A real address that is not one of ours
		// translates to "" and is left alone.
		{
			FString FromBrowser = WebRTCLan_RoomForHost( *RoomName );
			if( FromBrowser.Length() )
			{
				debugf( NAME_DevNet, "[WebRTC] Init(connect): browser address %s -> room '%s'", *RoomName, *FromBrowser );
				RoomName = FromBrowser;
			}
		}
		if( RoomName.Length() <= 0 )
			RoomName = "default";
		FString ServerPeerId;
		ServerPeerId.Setf( "%s-%s-srv", WEBRTC_PEER_PREFIX, *RoomName );
		debugf( NAME_DevNet, "[WebRTC] Init(connect): room='%s' -> peer '%s'", *RoomName, *ServerPeerId );
		// Take the one client slot. A server travel calls Init(connect) again while the old level is still
		// ticking and flushing (UE1 keeps you playing until the new level has loaded). On UDP the two
		// connections have different local ports and coexist; here they would share glue conn 0, and the
		// departing level's driver would write its actor bunches into the new session, which refuses them.
		if( GClientDriver && GClientDriver!=this && GClientDriver->GetServerConnection() )
		{
			debugf( NAME_DevNet, "[WebRTC] Init(connect): detaching the previous client connection (one transport slot)" );
			// Detached, not closed: marking it USOCK_Closed makes the engine treat the departing level as
			// "connection lost" and abort the travel. It must go quiet (no sends, no receives, no transport
			// polling); the level change destroys it a moment later.
			GClientDriver->GetServerConnection()->WebRTCConnId = -1;
			// When that destroy comes, it must not call webrtc_destroy(): the transport it would tear down
			// is now ours. See Destroy().
			GClientDriver->bDetached = 1;
		}
		GClientDriver = this;
		webrtc_connect( *ServerPeerId );

		// Pending server connection + control channel (channel 0) — UE1's UTcpNetDriver creates it here
		// too (IpDrv.cpp: GetServerConnection()->CreateChannel(CHTYPE_Control,1,0)).
		ServerConnection = new UWebRTCConnection( 0, this, USOCK_Pending, 1 );
		ServerConnection->URL = URL;
		GetServerConnection()->CreateChannel( CHTYPE_Control, 1, 0 );
	}
	else
	{
		// Listen server. Room + display name come from the travel URL options.
		bIsListenServer = 1;
		bDetached       = 0;
		RoomName   = URL.GetOption( "room=", "default" );
		ServerName = URL.GetOption( "ServerName=", "" );

		// When the URL does not name the server, read this driver's own config. A room can hold several
		// servers, and the transport identifies one as `<room>_<slug of the server name>`, so the name is half
		// of the address other players dial. The stock in-game listen path sends a travel URL with neither
		// option; without this, every menu-hosted game would register under a bare room and two of them in one
		// room would collide on the broker. This engine has no GameReplicationInfo to hold a `ServerName`, so
		// it lives in this CLASS_Config driver's own section (`-ini:` can seed it) and is read at listen time.
		if( !ServerName.Length() )
		{
			char Cfg[256] = "";
			if( GetConfigString( "WebRTCDrv.WebRTCNetDriver", "ServerName", Cfg, ARRAY_COUNT(Cfg) ) && Cfg[0] )
				ServerName = Cfg;
		}

		debugf( NAME_DevNet, "[WebRTC] Init(listen): room='%s' name='%s'", *RoomName, *ServerName );
		// Tell the synthetic LAN which room is ours, so a host browsing its own server
		// list does not find itself and try to connect a peer to itself.
		WebRTCLan_SetLocalRoom( *RoomName );
		// `room/serverName` — the transport slugs the name and builds `<prefix>-<room>_<slug>-srv`. The
		// separator is a `/` because a server name may contain anything, including the `_` the peer id uses.
		FString ListenKey = ServerName.Length() ? (RoomName + FString("/") + ServerName) : RoomName;
		webrtc_init_listen( *ListenKey );
	}

	return 1;
	unguard;
}

/*-----------------------------------------------------------------------------
	UWebRTCNetDriver::Tick.
-----------------------------------------------------------------------------*/
void UWebRTCNetDriver::Tick()
{
	guard(UWebRTCNetDriver::Tick);
	Time = appSeconds();

	BYTE Data[UNetConnection::MAX_PACKET_SIZE];

	// Server: accept newly-opened peer connections.
	//
	// ACCEPTC_Ignore is not a refusal. ULevel::NotifyAcceptingConnection (UnLevel.cpp) returns it for every
	// incoming connection while a server travel is pending (`NextURL` set), i.e. for the whole
	// ServerTravelPause window. On UDP that means "drop this connect packet": nothing is torn down, and the
	// client keeps retrying and gets in once the new level is listening.
	//
	// This transport is a pipe, and closing it is visible: the client's glue sees the data channel die, tears
	// the peer down, reconnects, is ignored again, and the two sides livelock. So park the pipe and re-offer it
	// on a later Tick, leaving the transport untouched. The parked ids are re-offered before new arrivals are
	// drained, or a client that has been waiting would lose its place to a newer one.
	TArray<INT> Incoming = ParkedAccepts;
	ParkedAccepts.Empty();
	INT NewConnId;
	while( (NewConnId = webrtc_accept_connection()) > 0 )
		Incoming.AddItem( NewConnId );

	for( INT i=0; i<Incoming.Num(); i++ )
	{
		INT ConnId = Incoming(i);
		// The client may have given up (or the page closed) while we were ignoring it.
		if( webrtc_get_connection_state( ConnId ) == WEBRTC_STATE_CLOSED )
			continue;
		EAcceptConnection Acc = Notify->NotifyAcceptingConnection();
		if( Acc == ACCEPTC_Ignore )
		{
			ParkedAccepts.AddItem( ConnId );
			continue;
		}
		if( Acc != ACCEPTC_Accept )
		{
			webrtc_close( ConnId );
			continue;
		}
		UWebRTCConnection* Conn = new UWebRTCConnection( ConnId, this, USOCK_Open, 0 );
		Conn->URL.Host.Setf( "webrtc:%i", ConnId );
		Notify->NotifyAcceptedConnection( Conn );
		Connections.AddItem( Conn );
		debugf( NAME_DevNet, "[WebRTC] Accepted connection connId=%i (clients=%i)", ConnId, Connections.Num() );
	}

	// Poll receive on the client's server connection + all server-side client connections.
	// (The pending server connection is not promoted to Open here: the engine's join flow owns that, as with
	// the native driver; only a dead transport is surfaced, as USOCK_Closed.)
	// A detached connection (WebRTCConnId < 0) has had the client transport slot taken from it by a newer
	// driver (see Init). Nothing is polled for it: the glue would report a connId it no longer owns as closed,
	// and the engine would treat the departing level as "connection lost".
	if( GetServerConnection() && GetServerConnection()->WebRTCConnId >= 0 )
	{
		UWebRTCConnection* SC = GetServerConnection();
		if( webrtc_get_connection_state( SC->WebRTCConnId ) == WEBRTC_STATE_CLOSED && SC->State != USOCK_Closed )
		{
			debugf( NAME_DevNet, "[WebRTC] Server connection transport closed" );
			SC->State = USOCK_Closed;
		}
		if( SC->State == USOCK_Open || SC->State == USOCK_Pending )
		{
			INT Size;
			while( (Size = webrtc_recv( SC->WebRTCConnId, Data, sizeof(Data) )) > 0 )
			{
				SC->LastReceiveTime = Time;
				SC->ReceivedPacket( Data, Size );
			}
		}
	}
	for( INT i=0; i<Connections.Num(); i++ )
	{
		UWebRTCConnection* Conn = (UWebRTCConnection*)Connections(i);
		// Log a remote close, so the log shows the engine acting on it and not only the glue's transport
		// giving up.
		if( webrtc_get_connection_state( Conn->WebRTCConnId ) == WEBRTC_STATE_CLOSED && Conn->State != USOCK_Closed )
		{
			debugf( NAME_DevNet, "[WebRTC] TickDispatch: Client connId=%i connection CLOSED by remote", Conn->WebRTCConnId );
			Conn->State = USOCK_Closed;
		}
		if( Conn->State == USOCK_Open || Conn->State == USOCK_Pending )
		{
			INT Size;
			while( (Size = webrtc_recv( Conn->WebRTCConnId, Data, sizeof(Data) )) > 0 )
			{
				Conn->LastReceiveTime = Time;
				Conn->ReceivedPacket( Data, Size );
			}
		}
	}

	// Poll all connections.
	if( GetServerConnection() )
		GetServerConnection()->Tick();
	for( INT i=0; i<Connections.Num(); i++ )
		Connections(i)->Tick();

#ifdef __EMSCRIPTEN__
	// Listen server: publish current status for browser discovery (~1s throttle). The synthetic LAN feeds
	// these fields into the beacon text the stock UnrealListenMenu lists.
	//
	// Players comes from the transport (connections + the local listen player), not the GameInfo: v200's
	// native AGameInfo exports neither NumPlayers nor MaxPlayers (they are script-side properties, so there
	// is no C++ field to read). MaxPlayers has no native source here, so the row shows a fixed 16.
	if( bIsListenServer )
	{
		static DOUBLE LastPush = 0.0;
		DOUBLE Now = appSeconds();
		if( Now - LastPush > 1.0 )
		{
			LastPush = Now;
			FString SName = ServerName, MapName = "?", GType = "";
			INT Players = 1 + Connections.Num();   // the listen host counts
			const INT MaxPl = 16;
			UGameEngine* GE = Cast<UGameEngine>( GWebRTCEngine );
			if( GE && GE->GLevel )
			{
				if( GE->GLevel->URL.Map.Length() )
					MapName = GE->GLevel->URL.Map;
				ALevelInfo* LI = GE->GLevel->GetLevelInfo();
				AGameInfo* GI = LI ? LI->Game : NULL;
				if( GI )
					GType = GI->GetClass()->GetName();
			}
			if( !SName.Length() )
				SName = "Unreal Server";
			// TCHAR is char on v200, so these are already ANSI: no conversion, and no shared static
			// buffer to clobber.
			webrtc_set_listen_status( *SName, *MapName, Players, MaxPl, *GType );
		}
	}

	// Yield to the event loop while the WebRTC handshake completes (signaling + ICE need
	// several event-loop turns). Only sleep while waiting to establish — never during the data phase, so
	// asset streaming isn't throttled.
	if( bIsListenServer && Connections.Num()==0 )
		emscripten_sleep( 50 );
	else if( GetServerConnection() && webrtc_get_connection_state( GetServerConnection()->WebRTCConnId ) != WEBRTC_STATE_OPEN )
		emscripten_sleep( 50 );
#endif

	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
