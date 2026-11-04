/*=============================================================================
	MemCacheTest.cpp: checks of FMemCache (Core/Src/UnCache.cpp) that need
	only Core. Built by the native presets as build-<preset>/MemCacheTest.bin;
	prints one line per check and "MemCacheTest: <n> passed, <m> failed",
	and exits 1 when a check fails.
=============================================================================*/

#include <stdio.h>
#include <stdlib.h>
#include "Core.h"

static INT GPassed = 0, GFailed = 0;

static void Expect( UBOOL Ok, const char* What )
{
	printf( "%s: %s\n", Ok ? "ok  " : "FAIL", What );
	if( Ok )
		GPassed++;
	else
		GFailed++;
}

// An item a Create merged into its new block is gone, also when it was the most recently used one: a Get of
// its id is a miss, never the new item's memory.
static void EvictedMruItemIsGone()
{
	FMemCache* Cache = new FMemCache;
	Cache->Init( 65536, 64 );
	FCacheItem* Item = NULL;
	const QWORD IdA = ((QWORD)7 << 32) + CID_ActorLightCache;
	const QWORD IdB = ((QWORD)9 << 32) + CID_StaticMap;

	BYTE* A = Cache->Create( IdA, Item, 1024 );
	*(INT*)A = 3;
	Item->Unlock();
	Expect( Cache->Get( IdA, Item )==A, "a created item is found again" );
	Item->Unlock();

	// Too big to fit beside A: the cache gives it A's memory.
	BYTE* B = Cache->Create( IdB, Item, 65000 );
	*(INT*)B = 0x3F000000;
	Item->Unlock();
	Expect( B==A, "the second item takes the first one's memory" );

	BYTE* Stale = Cache->Get( IdA, Item );
	Expect( Stale==NULL, "the evicted most-recently-used item is a miss" );
	if( Stale )
		Item->Unlock();

	BYTE* Again = Cache->Get( IdB, Item );
	Expect( Again==B && Item->GetId()==IdB, "the new item is found under its own id" );
	if( Again )
		Item->Unlock();

	Cache->Exit( 1 );
	delete Cache;
}

int main( int argc, char* argv[] )
{
	EvictedMruItemIsGone();
	printf( "MemCacheTest: %i passed, %i failed\n", GPassed, GFailed );
	fflush( stdout );
	// _Exit: the object system was never started, so Core's static classes must not run their destructors.
	_Exit( GFailed ? 1 : 0 );
}
