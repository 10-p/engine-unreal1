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

// Engine version. Unreal 1998 (UE1) is v200. Reserved so a future render-driver port that keys its
// per-engine adaptations off ENGINE_VERSION (e.g. the OldUnreal XOpenGLDrv / Chris Dohnal UTGLR forks,
// which use `#if ENGINE_VERSION==200`) has the macro available. No other engine code references it
// (verified) — it is a no-op today. A 2.9b attempt to port those drivers was abandoned (v200 is far more
// divergent than ut99's v400: no TCHAR/TEXT, a 1998 P8-only texture-format model, a different URenderDevice
// interface); see the ufront migration STATE.md "2.9b — ABANDONED" section. ut99 (engine-ut99) defines 400.
#define ENGINE_VERSION 200

/*-----------------------------------------------------------------------------
	The End.
-----------------------------------------------------------------------------*/
