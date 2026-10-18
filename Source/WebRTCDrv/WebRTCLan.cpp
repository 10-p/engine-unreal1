/*=============================================================================
	WebRTCLan.cpp: makes the stock Unreal 1 server browser work over WebRTC.

	It simulates UDP rather than patching the menu: the menu is script in the retail
	`.u` packages, which are not recompiled, so this meets the stock code where it is.

	  `UnrealI.UnrealListenMenu` ("Find Local Servers") spawns `IpDrv.ClientBeaconReceiver`,
	  which binds the beacon port and listens; it never sends anything. Servers broadcast
	  "<Product> <address> <text>"; the receiver stores everything after the product, the
	  menu lists `GetBeaconText()` (after the first space) and travels to
	  `GetBeaconAddress()` (before it).

	So binding the beacon port starts discovery through the glue, and its results come
	back as manufactured beacons.

	A join gets back to its room through the beacon's synthetic address, 10.199.0.N, with
	N a stable index into a room table. `FURL` parses a dotted token as a host (UnURL.cpp),
	so the travel takes the network path, and `UWebRTCNetDriver::Init` asks this file to
	translate it back. The room name survives a round trip through code that only
	understands IPv4.

	Layering: this file lives in WebRTCDrv and uses no IpDrv types; IpDrv calls in through
	WebRTCLan.h. One-way, resolved at the final --whole-archive link, no module cycle.
=============================================================================*/

#include "Engine.h"
#include "UnNet.h"
#include "WebRTCLan.h"

// Discovery bridge: the glue's discovery imports, resolved at the wasm link.
extern "C"
{
	void webrtc_discover_servers( const char* room );
	int  webrtc_get_server_count();
	int  webrtc_is_discovery_done();
	int  webrtc_get_server_info( int index, char* name, int nameMax, char* joinKey, int joinKeyMax );
	int  webrtc_get_server_map( int index, char* buf, int maxLen );
	int  webrtc_get_server_gametype( int index, char* buf, int maxLen );
	int  webrtc_get_server_players( int index );
	int  webrtc_get_server_maxplayers( int index );
	int  webrtc_get_server_ping( int index );
}

// 10.199.0.0/16 — private, and far from anything a real LAN hands out. The low 16 bits index GRooms,
// which is why that table is append-only.
#define LAN_ADDR_BASE     0x0AC70000u
#define LAN_ADDR_MASK     0xFFFF0000u
#define LAN_GAME_PORT     7777
#define LAN_DISCOVERY_MIN_INTERVAL  3.0
// ClientBeaconReceiver ages rows out after BeaconTimeout (5 s default), so re-beacon well inside that
// or the list blinks empty between refreshes.
#define LAN_BEACON_REPEAT_INTERVAL  2.0

struct FLanServer
{
	FString Room, Name, Map;
	INT     Players, MaxPlayers;
	FLanServer() : Players(0), MaxPlayers(0) {}
};

static TArray<FString>    GRooms;      // append-only: index -> room, stable for the session
static TArray<FLanServer> GServers;
static UBOOL              GDiscoveryPending = 0;
static DOUBLE             GLastDiscoveryAt  = 0.0;
static FString            GLocalRoom;   // our own listen room, never advertised back to us

struct FLanLink
{
	const void* Id;
	INT         BoundPort;
	UBOOL       WantsBeacons;
	DOUBLE      NextBeaconAt;
};
static TArray<FLanLink> GLinks;

struct FLanReply
{
	const void* LinkId;
	DWORD       Addr;
	INT         Port;
	FString     Text;
	FLanReply() : LinkId(NULL), Addr(0), Port(0) {}
	FLanReply( const void* InLinkId, DWORD InAddr, INT InPort, const FString& InText )
	: LinkId(InLinkId), Addr(InAddr), Port(InPort), Text(InText) {}
};
static TArray<FLanReply> GReplies;

static FLanLink* FindLink( const void* Id )
{
	for( INT i=0; i<GLinks.Num(); i++ )
		if( GLinks(i).Id == Id )
			return &GLinks(i);
	return NULL;
}

static FLanLink& TouchLink( const void* Id )
{
	FLanLink* L = FindLink( Id );
	if( L )
		return *L;
	FLanLink& N = GLinks( GLinks.AddZeroed() );
	N.Id           = Id;
	N.BoundPort    = 0;
	N.WantsBeacons = 0;
	N.NextBeaconAt = 0.0;
	return N;
}

/**
 * Stable index for a room. Append-only (see GRooms).
 *
 * `new( Array )T( Item )`, not `Array.AddItem( Item )`: see the note in PumpDiscovery.
 */
static INT RoomIndex( const FString& Room )
{
	for( INT i=0; i<GRooms.Num(); i++ )
		if( appStricmp( *GRooms(i), *Room )==0 )
			return i;
	new( GRooms )FString( Room );
	return GRooms.Num() - 1;
}

static DWORD AddrForIndex( INT Index )
{
	return LAN_ADDR_BASE | (DWORD)(Index & 0xFFFF);
}

static UBOOL IsSyntheticAddr( DWORD Addr )
{
	return (Addr & LAN_ADDR_MASK) == LAN_ADDR_BASE;
}

/** ClientBeaconReceiver compares the product with `==`, which is case-sensitive in UnrealScript, so
 *  this is read from the ini rather than guessed: one wrong capital and every beacon is silently
 *  dropped with no error anywhere. */
static FString BeaconProduct()
{
	char Buf[64] = {0};
	if( GetConfigString( "IpDrv.UdpBeacon", "BeaconProduct", Buf, sizeof(Buf) ) && Buf[0] )
		return FString( Buf );
	return FString( "Unreal" );
}

static void KickDiscovery()
{
	DOUBLE Now = appSeconds();
	if( GDiscoveryPending || Now - GLastDiscoveryAt < LAN_DISCOVERY_MIN_INTERVAL )
		return;
	GLastDiscoveryAt  = Now;
	GDiscoveryPending = 1;
	webrtc_discover_servers( "" );   // "" = every room on this signalling server
	debugf( NAME_DevNet, "[WebRTC-LAN] discovery started" );
}

// Throttled, and the throttle is required, not an optimisation. Every webrtc_* call here is `__proxy: 'sync'`,
// a cross-thread round trip from the engine pthread to the main thread, and this is reached from
// AUdpLink::Tick once per link per frame. Under load that overflows Emscripten's proxy queue and the module
// aborts ("emscripten_proxy_async failed"). Ask four times a second instead.
static DOUBLE GLastPumpAt = 0.0;

static void PumpDiscovery()
{
	if( !GDiscoveryPending )
		return;
	const DOUBLE PumpNow = appSeconds();
	if( PumpNow - GLastPumpAt < 0.25 )
		return;
	GLastPumpAt = PumpNow;
	if( !webrtc_is_discovery_done() )
		return;
	GDiscoveryPending = 0;

	GServers.Empty();
	const INT Count = webrtc_get_server_count();
	char nameBuf[256], roomBuf[256], mapBuf[128], gtBuf[128];
	for( INT i=0; i<Count; i++ )
	{
		if( !webrtc_get_server_info( i, nameBuf, sizeof(nameBuf), roomBuf, sizeof(roomBuf) ) )
			continue;
		FString Room( roomBuf );
		if( GLocalRoom.Length() && appStricmp( *Room, *GLocalRoom )==0 )
			continue;   // never advertise ourselves to ourselves
		webrtc_get_server_map( i, mapBuf, sizeof(mapBuf) );
		webrtc_get_server_gametype( i, gtBuf, sizeof(gtBuf) );

		// Copy-constructed in place. `TArray<T>` is a POD container: `AddItem` is
		//     `INT Index=Add(); (*this)(Index)=Item;` and `FArray::Add` neither zeroes nor constructs, so
		// assigning an FString into that slot reallocs whatever bit pattern the allocator left behind.
		// FLanServer holds three FStrings, so one AddItem is three reallocs of three garbage pointers. Fresh
		// heap pages come back zeroed and a zeroed FString survives the assignment, so the failure depends on
		// the heap and looks like flakiness. Placement new is the only spelling that copy-constructs;
		// `TArray::operator=` uses it.
		//
		// In v200, FString's default ctor allocates a 1-byte "", so a zeroed FString is not even a valid
		// empty one: its Length() is ArrayNum-1 = -1.
		FLanServer S;
		S.Room       = Room;
		S.Name       = nameBuf[0] ? FString(nameBuf) : Room;
		S.Map        = mapBuf[0]  ? FString(mapBuf)  : FString("?");
		S.Players    = webrtc_get_server_players( i );
		S.MaxPlayers = webrtc_get_server_maxplayers( i );
		if( S.MaxPlayers <= 0 )
			S.MaxPlayers = 16;
		const FString Host = WebRTCLan_HostForRoom( *S.Room );   // also reserves the room's index
		debugf( NAME_DevNet, "[WebRTC-LAN] '%s' room=%s map=%s %i/%i -> %s", *S.Name, *S.Room, *S.Map, S.Players, S.MaxPlayers, *Host );
		new( GServers )FLanServer( S );
	}
	debugf( NAME_DevNet, "[WebRTC-LAN] discovery done: %i server(s)", GServers.Num() );
}

static void QueueBeacons( const void* LinkId )
{
	const FString Product = BeaconProduct();
	for( INT i=0; i<GServers.Num(); i++ )
	{
		const FLanServer& S = GServers(i);
		// The receiver splits on the first space after the product: everything before it is the join
		// address and the rest is the row label. So the address must contain no space, and the label
		// must not be empty or the row is unselectable.
		FString Text;
		Text.Setf( "%s %s %s (%s) %i/%i",
			*Product, *WebRTCLan_HostForRoom( *S.Room ), *S.Name, *S.Map, S.Players, S.MaxPlayers );
		// Placement new, like every append of an FString-holding struct in this file (see PumpDiscovery). A
		// zeroed FString is not a valid empty one in v200 (its default ctor allocates a 1-byte ""), so
		// AddZeroed-then-fill is unsafe here even where it happens to work.
		new( GReplies )FLanReply( LinkId, AddrForIndex( RoomIndex( S.Room ) ), LAN_GAME_PORT, Text );
	}
}

/*-----------------------------------------------------------------------------
	The IpDrv-facing hooks.
-----------------------------------------------------------------------------*/

void WebRTCLan_Bind( const void* LinkId, INT Port )
{
	FLanLink& L = TouchLink( LinkId );
	L.BoundPort = Port;

	// ClientBeaconReceiver is passive: it binds and waits for servers to broadcast. Nothing will ever
	// arrive to trigger discovery, so binding the beacon port is the trigger.
	INT BeaconPort = 7776;
	GetConfigInt( "IpDrv.UdpBeacon", "BeaconPort", BeaconPort );
	if( Port == BeaconPort )
	{
		L.WantsBeacons = 1;
		L.NextBeaconAt = 0.0;
		KickDiscovery();
		debugf( NAME_DevNet, "[WebRTC-LAN] beacon listener bound on port %i", Port );
	}
}

void WebRTCLan_Unbind( const void* LinkId )
{
	for( INT i=GLinks.Num()-1; i>=0; i-- )
		if( GLinks(i).Id == LinkId )
			GLinks.Remove( i );
	for( INT i=GReplies.Num()-1; i>=0; i-- )
		if( GReplies(i).LinkId == LinkId )
			GReplies.Remove( i );
}

UBOOL WebRTCLan_Send( const void* LinkId, DWORD DestAddr, INT DestPort, const char* Text )
{
	if( !Text )
		return 0;
	TouchLink( LinkId );
	// "REPORT" is the beacon request. Unreal 1's own client never sends it, but a server-side UdpBeacon
	// would, and answering costs nothing.
	if( appStrnicmp( Text, "REPORT", 6 )==0 )
	{
		KickDiscovery();
		PumpDiscovery();
		QueueBeacons( LinkId );
		return 1;
	}
	// Anything aimed at a synthetic server is ours to swallow: there is no socket that could carry it,
	// and letting sendto() fail only produces a misleading script warning.
	return IsSyntheticAddr( DestAddr ) ? 1 : 0;
}

UBOOL WebRTCLan_NextReply( const void* LinkId, DWORD* OutAddr, INT* OutPort, FString& OutText )
{
	PumpDiscovery();

	FLanLink* L = FindLink( LinkId );
	if( L && L->WantsBeacons )
	{
		DOUBLE Now = appSeconds();
		if( Now >= L->NextBeaconAt )
		{
			L->NextBeaconAt = Now + LAN_BEACON_REPEAT_INTERVAL;
			KickDiscovery();
			QueueBeacons( LinkId );
		}
	}

	for( INT i=0; i<GReplies.Num(); i++ )
	{
		if( GReplies(i).LinkId != LinkId )
			continue;
		*OutAddr = GReplies(i).Addr;
		*OutPort = GReplies(i).Port;
		OutText  = GReplies(i).Text;
		GReplies.Remove( i );
		return 1;
	}
	return 0;
}

FString WebRTCLan_HostForRoom( const char* Room )
{
	const DWORD A = AddrForIndex( RoomIndex( FString(Room) ) );
	FString S;
	S.Setf( "%i.%i.%i.%i", (A>>24)&0xFF, (A>>16)&0xFF, (A>>8)&0xFF, A&0xFF );
	return S;
}

FString WebRTCLan_RoomForHost( const char* Host )
{
	if( !Host )
		return FString("");
	// Parse a.b.c.d by hand rather than pull in a socket header (this file must not depend on IpDrv).
	INT Oct[4] = {0,0,0,0}, N = 0;
	const char* P = Host;
	while( N < 4 )
	{
		if( *P < '0' || *P > '9' )
			return FString("");
		INT V = 0;
		while( *P >= '0' && *P <= '9' )
			V = V*10 + (*P++ - '0');
		if( V > 255 )
			return FString("");
		Oct[N++] = V;
		if( N < 4 )
		{
			if( *P != '.' )
				return FString("");
			P++;
		}
	}
	if( *P != 0 )
		return FString("");
	const DWORD Addr = ((DWORD)Oct[0]<<24) | ((DWORD)Oct[1]<<16) | ((DWORD)Oct[2]<<8) | (DWORD)Oct[3];
	if( !IsSyntheticAddr( Addr ) )
		return FString("");
	const INT Index = (INT)(Addr & 0xFFFF);
	if( Index < 0 || Index >= GRooms.Num() )
		return FString("");
	return GRooms( Index );
}

void WebRTCLan_SetLocalRoom( const char* Room )
{
	GLocalRoom = Room ? FString(Room) : FString("");
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
