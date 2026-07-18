/*=============================================================================
	WebRTCDrv.cpp: WebRTC network driver for UE1 (Emscripten/browser multiplayer).
	ufront step 2.6c.

	Engine-LOCAL (engines are independent — no shared code). Adapted from
	engine-ut2004's WebRTCDrv (UE2) to UE1's UNetDriver/UNetConnection API, and
	structured after this repo's IpDrv.cpp (UTcpNetDriver/UTcpipConnection): the
	transport is the glue `webrtc_*` C-ABI (satisfied by the baked webrtc-shim.js →
	the host-injected Module.UE.net) instead of BSD sockets.

	UE1 (this) vs UE2 (ut2004) differences: one Init(Connect,...) instead of
	InitConnect/InitListen; Tick() instead of TickDispatch(); send via FlushNet()
	(OutData/OutNum) instead of LowLevelSend(); recv via ReceivedPacket().
=============================================================================*/

#include "Engine.h"
#include "UnNet.h"
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

// UE1's peer-ID namespace prefix. MUST match targets/unreal1/target.json
// network.peerPrefix (the glue registers the listen peer as "<prefix>-<room>-srv").
// This is engine-specific by design — engine-unreal1 only ever hosts Unreal, exactly
// as engine-ut2004 hardcodes "ut2k4". (The DATA-not-code rule governs the generic
// frontend/glue, not the per-game engine repos.)
#define WEBRTC_PEER_PREFIX      "u1"

/*-----------------------------------------------------------------------------
	extern "C" transport bridge (WASM <-> JS).
	On Emscripten these resolve to the baked vendor/glue/webrtc-shim.js, which
	forwards to Module.UE.net.*. On native they are inline no-op stubs.
	NOTE: UE1 does NOT use the deferred-connect pair (webrtc_deferred_open /
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
			if( OutNum > 0 )
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
class DLL_EXPORT UWebRTCNetDriver : public UNetDriver
{
	DECLARE_CLASS(UWebRTCNetDriver,UNetDriver,CLASS_Transient|CLASS_Config)

	// Variables.
	FString	RoomName;
	FString	ServerName;      // ?ServerName= for browser discovery
	UBOOL	bIsListenServer;

	// UObject interface.
	void Destroy()
	{
		guard(UWebRTCNetDriver::Destroy);
		debugf( NAME_Exit, "[WebRTC] Destroy: room='%s'", *RoomName );
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

	// WebRTC handshake (PeerJS signaling + ICE) and Emscripten sync-XHR map downloads take far longer
	// than a TCP connect. The engine's sleeps stop while a package streams, so LastReceiveTime goes stale;
	// generous timeouts prevent an immediate disconnect on the first post-download tick (ICE monitors the
	// real link underneath). Mirrors engine-ut2004's WebRTC timeouts.
	ConnectionTimeout     = 300.f;
	InitialConnectTimeout = 600.f;

	if( Connect )
	{
		// Client. URL.Host carries the room (HostID); the server registered as "<prefix>-<room>-srv".
		bIsListenServer = 0;
		RoomName = URL.Host;
		if( RoomName.Length() <= 0 )
			RoomName = "default";
		FString ServerPeerId;
		ServerPeerId.Setf( "%s-%s-srv", WEBRTC_PEER_PREFIX, *RoomName );
		debugf( NAME_DevNet, "[WebRTC] Init(connect): room='%s' -> peer '%s'", *RoomName, *ServerPeerId );
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
		RoomName   = URL.GetOption( "room=", "default" );
		ServerName = URL.GetOption( "ServerName=", "" );
		debugf( NAME_DevNet, "[WebRTC] Init(listen): room='%s' name='%s'", *RoomName, *ServerName );
		webrtc_init_listen( *RoomName );
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
	INT NewConnId;
	while( (NewConnId = webrtc_accept_connection()) > 0 )
	{
		if( Notify->NotifyAcceptingConnection() != ACCEPTC_Accept )
		{
			webrtc_close( NewConnId );
			continue;
		}
		UWebRTCConnection* Conn = new UWebRTCConnection( NewConnId, this, USOCK_Open, 0 );
		Conn->URL.Host.Setf( "webrtc:%i", NewConnId );
		Notify->NotifyAcceptedConnection( Conn );
		Connections.AddItem( Conn );
		debugf( NAME_DevNet, "[WebRTC] Accepted connection connId=%i (clients=%i)", NewConnId, Connections.Num() );
	}

	// Poll receive on the client's server connection + all server-side client connections.
	// (Do NOT promote the pending server connection to Open here — the engine's join flow owns that,
	// like the native driver; we only surface a dead transport as USOCK_Closed.)
	if( GetServerConnection() )
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
		if( webrtc_get_connection_state( Conn->WebRTCConnId ) == WEBRTC_STATE_CLOSED && Conn->State != USOCK_Closed )
			Conn->State = USOCK_Closed;
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
	// Listen server: publish status for browser discovery (~1s throttle). Minimal for now — name + room;
	// full GameInfo introspection (players/gametype) is a discovery follow-up.
	if( bIsListenServer )
	{
		static DOUBLE LastPush = 0.0;
		DOUBLE Now = appSeconds();
		if( Now - LastPush > 1.0 )
		{
			LastPush = Now;
			const char* Name = ServerName.Length() > 0 ? *ServerName : "Unreal Server";
			webrtc_set_listen_status( Name, *RoomName, 0, 0, "" );
		}
	}

	// Yield to the browser event loop while the WebRTC handshake completes (PeerJS signaling + ICE need
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
