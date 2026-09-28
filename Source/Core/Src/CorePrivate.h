/*=============================================================================
	CorePrivate.h: Unreal core private header file.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

/*----------------------------------------------------------------------------
	Core public includes.
----------------------------------------------------------------------------*/

#include "Core.h"

/*-----------------------------------------------------------------------------
	UTextBufferFactory.
-----------------------------------------------------------------------------*/

//
// Imports UTextBuffer objects.
//
class CORE_API UTextBufferFactory : public UFactory
{
	DECLARE_CLASS_WITHOUT_CONSTRUCT(UTextBufferFactory,UFactory,0)

	// Constructor.
	UTextBufferFactory();

	// UFactory interface.
	UObject* Create( UClass* Class, UObject* InParent, FName Name, UObject* Context, const char* Type, const char*& Buffer, const char* BufferEnd, FFeedbackContext* Warn=NULL );
};

// The last None-context zero-fill: where it wrote and how many bytes (ufront 2.84, UnCorSc.cpp).
extern BYTE* GNullContextResult;
extern INT   GNullContextSize;

// ufront 2.84 -- call after stepping a value RuntimeSize bytes long into Addr, having cleared GNullContextResult
// before the step. `X = None.Y` zero-fills only as many bytes as the compiler recorded: the value's size in the
// 32-bit retail format (4 for any object). Where the value is larger in memory (an 8-byte pointer, a struct
// holding one) the rest of the reader's uninitialised buffer would read as a live-looking garbage reference, so
// when the fill landed here and fell short, zero the rest. Never on 32-bit.
inline void ZeroNullContextTail( BYTE* Addr, INT RuntimeSize )
{
	if( Addr && Addr==GNullContextResult && GNullContextSize<RuntimeSize )
		appMemset( Addr+GNullContextSize, 0, RuntimeSize-GNullContextSize );
	GNullContextResult = NULL;
}

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
