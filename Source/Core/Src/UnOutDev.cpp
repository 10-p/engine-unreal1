/*=============================================================================
	UnOutDev.cpp: Unreal FOutputDevice implementation
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.

	Revision history:
		* Created by Tim Sweeney
=============================================================================*/

#include "CorePrivate.h"

/*-----------------------------------------------------------------------------
	FOutputDevice implementation.
-----------------------------------------------------------------------------*/

//
// Print a message on the output device using NAME_Log type.
//
void FOutputDevice::Log( const char* Text )
{
	guard(FOutputDevice::Log);
	WriteBinary( Text, appStrlen(Text), NAME_Log );
	unguard;
};

//
// Print a message on the output device using NAME_Log type.
//
void FOutputDevice::Log( EName Event, const char* Text )
{
	guardSlow(FOutputDevice::Log);
	if( !(FName(Event).GetFlags() & RF_Suppress) )
		WriteBinary( Text, appStrlen(Text), Event );
	unguardSlow;
};

//
// Format a message and write it whole: in a stack buffer when it fits, else in a heap buffer grown until it does.
// A C library's vsnprintf returns the length it needed, or -1 when the text was cut.
//
static void WriteFormatted( FOutputDevice* Out, EName Event, const char* Fmt, va_list Args )
{
	char TempStr[4096];
	char* Text = TempStr;
	INT Size = ARRAY_COUNT(TempStr);
	for( ;; )
	{
		va_list ArgsCopy;
		va_copy( ArgsCopy, Args );
		INT Len = vsnprintf( Text, Size, Fmt, ArgsCopy );
		va_end( ArgsCopy );
		Text[Size-1] = 0;
		if( (Len>=0 && Len<Size) || Size>=(1<<24) )
			break;
		Size = Max( Len+1, Size*2 );
		if( Text != TempStr )
			appFree( Text );
		Text = (char*)appMalloc( Size, "Logf" );
	}
	Out->WriteBinary( Text, appStrlen(Text), Event );
	if( Text != TempStr )
		appFree( Text );
}

//
// Print a message on the output device, variable parameters.
//
void VARARGS FOutputDevice::Logf( EName Event, const char* Fmt, ... )
{
	guardSlow(FOutputDevice::Logf);
	if( !(FName(Event).GetFlags() & RF_Suppress) )
	{
		va_list Args;
		va_start( Args, Fmt );
		WriteFormatted( this, Event, Fmt, Args );
		va_end( Args );
	}
	unguardSlow;
}

//
// Print a message on the output device, variable parameters.
//
void VARARGS FOutputDevice::Logf( const char* Fmt, ... )
{
	va_list Args;
	va_start( Args, Fmt );
	WriteFormatted( this, NAME_Log, Fmt, Args );
	va_end( Args );
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
