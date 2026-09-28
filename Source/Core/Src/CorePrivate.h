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

// The last None-context zero-fill: where it wrote and how many bytes (ufront 2.84, UnCorSc.cpp; see
// UProperty::ExecLet).
extern BYTE* GNullContextResult;
extern INT   GNullContextSize;

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
