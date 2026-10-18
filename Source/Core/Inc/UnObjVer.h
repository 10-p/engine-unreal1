/*=============================================================================
	UnObjVer.h: Unreal object version.
	Copyright 1997 Epic MegaGames, Inc. This software is a trade secret.
=============================================================================*/

/*-----------------------------------------------------------------------------
	Unrealfile versions.
-----------------------------------------------------------------------------*/

// Package file information.
// Prevents incorrect files from being loaded.
#define PACKAGE_FILE_TAG 0x9E2A83C1

// The current Unrealfile version.
#define PACKAGE_FILE_VERSION 61

// The earliest file version which we can load with complete
// backwards compatibility. Must be at least PACKAGE_FILE_VERSION.
#define PACKAGE_MIN_VERSION 34

// Engine version: Unreal (1998) is v200. No engine code reads it; it is there for code that keys per-engine
// adaptations off it (`#if ENGINE_VERSION==200`).
#define ENGINE_VERSION 200

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
