/*=============================================================================
	IpDrv.cpp: Unreal TCP/IP driver.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

Revision history:
	* Created by Tim Sweeney.
=============================================================================*/

#include "IpDrvPrivate.h"
// ufront 2.38 — the synthetic LAN (engine-local WebRTCDrv). Header only: the call is resolved at the
// final --whole-archive link, so IpDrv does not link WebRTCDrv and there is no module cycle.
#include "WebRTCLan.h"

/*-----------------------------------------------------------------------------
	AUdpLink implementation.
-----------------------------------------------------------------------------*/

IMPLEMENT_CLASS(AUdpLink);

//
// Constructor.
//
AUdpLink::AUdpLink()
{
	guard(AUdpLink::AUdpLink);
	char Error[256];
	InitSockets( Error );
	unguard;
}

//
// Destroy.
//
void AUdpLink::Destroy()
{
	guard(AUdpLink::Destroy);
#ifdef __EMSCRIPTEN__
	WebRTCLan_Unbind( this );   // ufront 2.38 — drop any queued synthetic packets for this link
#endif
	if( GetSocket() )
		closesocket(GetSocket());
	Super::Destroy();
	unguard;
}

//
// Resolve a string URL and return its IP address.
//
void AUdpLink::execResolve( FFrame& Stack, BYTE*& Result )
{
	guard(AUdpLink::execResolve);
	P_GET_STRING(URL);
	P_FINISH;

	((FIpAddr*)Result)->Addr = 0;
	((FIpAddr*)Result)->Port = 0;

	unguard;
}
AUTOREGISTER_INTRINSIC( AUdpLink, INDEX_NONE, execResolve );

//
// Listen for packets on a port.
//
void AUdpLink::execBindPort( FFrame& Stack, BYTE*& Result )
{
	guard(AUdpLink::execBindPort);
	P_GET_INT_OPT(Port,0);
	P_FINISH;
#ifdef __EMSCRIPTEN__
	// ★ ufront 2.38 — a browser has no UDP, so there is no socket to bind and no point pretending
	// otherwise halfway. Bind is answered by the synthetic LAN instead: report success and register the
	// link, and Tick is served from WebRTCLan.
	//
	// ⚠ This REPLACES the bind rather than falling back after it, because ClientBeaconReceiver treats a
	// zero return as fatal ("beacon port in use") and never starts — and under Emscripten's virtual
	// socket layer the real result is not reliably one thing or the other.
	{
		static INT GNextSyntheticPort = 40000;
		const INT Bound = Port ? Port : GNextSyntheticPort++;
		WebRTCLan_Bind( this, Bound );
		*(DWORD*)Result = 1;
		return;
	}
#endif
	if( GInitialized )
	{
		if( GetSocket() == 0 )
		{
			GetSocket() = socket( AF_INET, SOCK_DGRAM, IPPROTO_UDP );
			if( GetSocket() != INVALID_SOCKET )
			{
				INT TrueBuffer=1;
				if( setsockopt( GetSocket(), SOL_SOCKET, SO_BROADCAST, (char*)&TrueBuffer, sizeof(TrueBuffer) )==0 )
				{
					sockaddr_in Addr;
					Addr.sin_family      = AF_INET;
					Addr.sin_addr.s_addr = 0;
					Addr.sin_port        = htons(Port);
					if( bind( Socket, (sockaddr*)&Addr, sizeof(Addr) )==0 )
					{
						DWORD NoBlock = 1;
						if( ioctlsocket( Socket, FIONBIO, &NoBlock )==0 )
						{
							// Success.
							*(DWORD*)Result = 1;
							return;
						}
						else Stack.ScriptWarn( 0, "BindPort: ioctlsocket failed" );
					} else Stack.ScriptWarn( 0, "BindPort: bind failed" );
				} else Stack.ScriptWarn( 0, "BindPort: setsockopt failed" );
			} else Stack.ScriptWarn( 0, "BindPort: socket failed" );
			closesocket(GetSocket());
			GetSocket() = 0;
		} else Stack.ScriptWarn( 0, "BindPort: already bound" );
	} else Stack.ScriptWarn( 0, "BindPort: winsock failed" );
	*(DWORD*)Result = 0;
	unguard;
}
AUTOREGISTER_INTRINSIC( AUdpLink, INDEX_NONE, execBindPort );

//
// Send text in a UDP packet.
//
void AUdpLink::execSendText( FFrame& Stack, BYTE*& Result )
{
	guard(AUdpLink::execSendText);
	P_GET_STRUCT(FIpAddr,IpAddr);
	P_GET_STRING(Str);
	P_FINISH;
#ifdef __EMSCRIPTEN__
	// ufront 2.38 — hand the datagram to the synthetic LAN. ⚠ Success is ZERO here (v200 inverts the
	// convention its UT99 descendant uses); returning 1 would make every send read as a failure.
	WebRTCLan_Send( this, IpAddr.Addr, IpAddr.Port, Str );
	*(DWORD*)Result = 0;
	return;
#endif
	if( GetSocket() )
	{
		sockaddr_in Addr;
		Addr.sin_family      = AF_INET;
		Addr.sin_port        = htons(IpAddr.Port);
		Addr.sin_addr.s_addr = htonl(IpAddr.Addr);
		if( sendto( Socket, Str, appStrlen(Str), 0, (sockaddr*)&Addr, sizeof(Addr) )==0 )
		{
			Stack.ScriptWarn( 0, "SentText: sendto failed" );
			*(DWORD*)Result = 1;
			return;
		}
	}
	*(DWORD*)Result = 0;
	unguard;
}
AUTOREGISTER_INTRINSIC( AUdpLink, INDEX_NONE, execSendText );

//
// Convert an address to a string URL.
//
void AUdpLink::execIpAddrToURL( FFrame& Stack, BYTE*& Result )
{
	guard(AUdpLink::execIpAddrToURL);
	P_GET_STRUCT(FIpAddr,Addr);
	P_FINISH;

	DWORD D=Addr.Addr;
	appSprintf( (char*)Result, "%i.%i.%i.%i:%i", (BYTE)(D>>24), (BYTE)(D>>16), (BYTE)(D>>8), (BYTE)(D>>0), Addr.Port );

	unguard;
}
AUTOREGISTER_INTRINSIC( AUdpLink, INDEX_NONE, execIpAddrToURL );

//
// Send binary data.
//
void AUdpLink::execSendBinary( FFrame& Stack, BYTE*& Result )
{
	guard(AUdpLink::execSendBinary);
	P_GET_STRUCT(FIpAddr,Addr);
	P_GET_INT(Count);
	//P_GET_BYTE_ARRAY(b)!!
	P_FINISH;

	unguard;
}
AUTOREGISTER_INTRINSIC( AUdpLink, INDEX_NONE, execSendBinary );

//
// Time passes...
//
UBOOL AUdpLink::Tick( FLOAT DeltaTime, enum ELevelTick TickType )
{
	guard(AUdpLink::Tick);

#ifdef __EMSCRIPTEN__
	// ufront 2.38 — deliver synthetic LAN packets through the STOCK receive path, so
	// ClientBeaconReceiver's own handler, its beacon table and the menu that reads it all run
	// unmodified. Outside the GetSocket() guard on purpose: on the web there is no socket at all
	// (see execBindPort).
	{
		FIpAddr From;
		FString Text;
		DWORD Addr = 0;
		INT   Port = 0;
		while( WebRTCLan_NextReply( this, &Addr, &Port, Text ) )
		{
			From.Addr = Addr;
			From.Port = Port;
			if( UdpMode==UDP_Text )
				eventReceivedText( From, *Text );
		}
	}
#endif

	// Dispatch received data.
	if( GetSocket() )
	{
		char Buffer[512];
		sockaddr_in FromAddr;
		socklen_t FromSize = sizeof(FromAddr);
		INT Count = recvfrom( GetSocket(), Buffer, ARRAY_COUNT(Buffer)-1, 0, (sockaddr*)&FromAddr, &FromSize );
		if( Count!=SOCKET_ERROR )
		{
			if( UdpMode==UDP_Text )
			{
				Buffer[Count]=0;
				FIpAddr Addr;
				Addr.Addr = ntohl( FromAddr.sin_addr.s_addr );
				Addr.Port = ntohs( FromAddr.sin_port );
				if( Count>0 )
					eventReceivedText( Addr, Buffer );
			}
			else
			{
				//!!eventReceivedBinary
			}
		}
	}

	// Update actor.
	return AActor::Tick( DeltaTime, TickType );

	unguard;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
