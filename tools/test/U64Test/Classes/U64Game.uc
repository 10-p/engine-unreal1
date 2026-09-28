//=============================================================================
// U64Game -- ufront 2.84: the script VM checks for Unreal 1's 64-bit (LP64) port (engine-ut99's U64Mutator,
// for v200: no mutators, so a game type runs them).
//
//     ./Unreal.bin "DmDeck16?game=U64Test.U64Game" -server        then read the log:
//     U64 PASS <case> / U64 FAIL <case>, then U64 DONE passed=<n> failed=<n>.
//     ... "?game=U64Test.U64Game?bench=9000000"                  also times the object-index cost (U64 BENCH).
//
// Compiled by the 32-bit `Unreal.bin -MAKE`; every case must pass on the 32-bit build first (the control), then
// on 64-bit. The cases are what 2.84 changed: script object references are 4-byte object indices; a None
// context zero-fills with the compiler's (32-bit) size, which the VM must widen; and -- v200 only -- a call lays
// parameters out from the linked properties, and a function's result offset follows the running layout.
//=============================================================================
class U64Game expands GameInfo;

var Actor           InstObj;
var Actor           InstArr[4];
var PointRegion     InstRegion;
var string[32]      InstStr;
var bool            bFlagA, bFlagB;
var int             Passed, Failed, BenchN, BenchField;
var U64StateActor   Stater;

function Report( bool bOk, string[64] Case )
{
	if( bOk )	{ Passed++; Log( "U64 PASS "$Case ); }
	else		{ Failed++; Log( "U64 FAIL "$Case ); }
}

function Actor GetSelf()				{ return self; }
final function Actor FinalGetSelf()		{ return self; }
function Actor Echo( Actor A )			{ return A; }
function bool IsNone( Actor A )			{ return A == None; }
function Actor ReturnNone( Actor Nobody )	{ return Nobody.Owner; }
function Actor AssignInsideThenSelf( Actor Nobody )
{
	InstObj = Nobody.Owner;		// a None-context fill of InstObj inside the callee...
	return self;				// ...must not clear the caller's `InstObj = <this>` afterwards.
}

// v200: CallFunction lays parameters out; these mix every size and alignment.
function int MixParms( byte B1, Actor A1, int I1, Actor A2, string[16] S1, vector V1, byte B2 )
{
	if( B1==3 && A1==self && I1==77 && A2==Level && S1=="mix" && V1.X==1 && V1.Y==2 && V1.Z==3 && B2==9 )
		return 1;
	return 0;
}
function OutParms( byte B, out Actor OA, out int OI, out byte OB )
{
	OA = Level;
	OI = 1234;
	OB = B + 1;
}
function OutByteThenInt( out byte OB, int X )	{ OB = X; }
function Actor RetAfterObjs( Actor A1, Actor A2, int I )	{ return A2; }
function int RetIntAfterObjs( Actor A1, Actor A2, int I )	{ return I; }

event InitGame( string[120] Options, out string[80] Error )
{
	Super.InitGame( Options, Error );
	BenchN = int( ParseOption( Options, "bench" ) );
}

function PostBeginPlay()
{
	Super.PostBeginPlay();
	RunTests();
	if( BenchN > 0 )
		RunBench();
	Stater = Spawn( class'U64StateActor' );
}

// Called by the state actor when its state code has run.
function StateDone( bool bOk )
{
	Report( bOk, "state_labels_sleep_gotostate" );
	Log( "U64 DONE passed="$Passed$" failed="$Failed );
}

final function int Add( int X, int Y ) { return X + Y; }
function RunBench()
{
	local int i, a, b, c;
	a = 1;	b = 2;
	for( i=0; i<BenchN; i++ )
	{
		c = a + b;
		BenchField = Add( c, BenchField );
	}
	Log( "U64 BENCH iterations="$BenchN$" field="$BenchField );
}

function RunTests()
{
	local Actor A, Nobody, It, OutA;
	local U64Game NoGame;
	local class<Actor> C;
	local PointRegion R, R2;
	local string[32] S;
	local name NameVal;
	local byte B, TB, N1, N2, N3, OutB;
	local int i, n, OutI;
	local Pawn P;

	// Preconditions: the values the None cases overwrite must be live.
	Report( Region.Zone != None, "precondition_region_zone" );
	Report( Nobody == None && NoGame == None, "precondition_none_locals" );

	// A None context writes zeros straight into the destination, sized by the compiler.
	A = self;			A = Nobody.Owner;			Report( A == None, "none_ctx_local_object" );
	InstObj = self;		InstObj = Nobody.Owner;		Report( InstObj == None, "none_ctx_instance_object" );
	InstArr[2] = self;	InstArr[2] = Nobody.Owner;	Report( InstArr[2] == None && InstArr[1] == None, "none_ctx_array_element" );
	C = Class;			C = Nobody.Class;			Report( C == None, "none_ctx_class" );
	R = Region;			R = Nobody.Region;			Report( R.Zone == None && R.iLeaf == 0 && R.ZoneNumber == 0, "none_ctx_struct_with_pointer" );
	InstRegion = Region; InstRegion = Nobody.Region; Report( InstRegion.Zone == None, "none_ctx_instance_struct" );
	R2 = Region;		R2.Zone = Nobody.Region.Zone; Report( R2.Zone == None && R2.iLeaf == Region.iLeaf, "none_ctx_struct_member_dest" );
	A = self;			A = NoGame.GetSelf();		Report( A == None, "none_ctx_virtual_call_result" );
	A = self;			A = NoGame.FinalGetSelf();	Report( A == None, "none_ctx_final_call_result" );
	Report( IsNone( Nobody.Owner ), "none_ctx_as_parameter" );
	A = self;			A = Echo( Nobody.Owner );	Report( A == None, "none_ctx_nested_argument" );
	A = self;			A = ReturnNone( Nobody );	Report( A == None, "none_ctx_return_value" );
	InstObj = self;		InstObj = AssignInsideThenSelf( Nobody ); Report( InstObj == self, "none_ctx_callee_does_not_clear_caller" );
	S = "abc";			S = NoGame.InstStr;			Report( S == "abc", "none_ctx_string_keeps_value" );
	A = self;			A = Pawn(Nobody.Owner);		Report( A == None, "none_ctx_under_cast" );
	A = Spawn( class'U64StateActor' ); A.SetOwner( self ); A.SetOwner( Nobody.Owner );
	Report( A.Owner == None, "none_ctx_native_object_parameter" );
	A.Destroy();

	// Object references read from the bytecode: variables, constants, casts, calls.
	Report( GetSelf() == self && FinalGetSelf() == self, "call_virtual_and_final" );
	Report( class'GameInfo' != None && class'GameInfo'.Name == 'GameInfo', "object_const_class" );
	Report( class'U64Game'.default.InstStr == "dflt", "class_default_property" );
	C = class<Actor>( DynamicLoadObject( "Engine.Pawn", class'Class' ) );
	Report( C == class'Pawn', "metacast" );
	A = Level.Game;		P = Pawn( A );				Report( P == None && GameInfo( A ) == Level.Game, "dynamic_cast" );
	bFlagA = true;		bFlagB = false;				Report( bFlagA && !bFlagB, "bool_instance_variables" );
	R = Region;			R2 = Region;				Report( R == R2, "struct_compare_equal" );
	R2.iLeaf++;										Report( R != R2, "struct_compare_differ" );

	// v200: parameters where the callee's linked properties read them, and results at the running offset.
	Report( MixParms( 3, self, 77, Level, "mix", vect(1,2,3), 9 ) == 1, "parms_mixed_layout" );
	OutA = None;	OutI = 0;	OutB = 0;
	OutParms( 4, OutA, OutI, OutB );
	Report( OutA == Level && OutI == 1234 && OutB == 5, "parms_out_copy_back" );
	TB = 0;	N1 = 5;	N2 = 6;	N3 = 7;
	OutByteThenInt( TB, 42 );
	Report( TB == 42 && N1 == 5 && N2 == 6 && N3 == 7, "parms_out_byte_keeps_neighbours" );
	Report( RetAfterObjs( self, Level, 5 ) == Level && RetIntAfterObjs( self, Level, 5 ) == 5, "return_after_object_parms" );

	// switch compares the compiled size; an object switch must compare whole pointers.
	A = self;	n = 0;
	switch( A ) { case None: n = 1; break; case Level: n = 2; break; case self: n = 3; break; default: n = 4; }
	Report( n == 3, "switch_object" );
	NameVal = 'Beta';	n = 0;
	switch( NameVal ) { case 'Alpha': n = 1; break; case 'Beta': n = 2; break; }
	Report( n == 2, "switch_name" );
	B = 7;		n = 0;
	switch( B ) { case 3: n = 1; break; case 7: n = 2; break; }
	Report( n == 2, "switch_byte" );
	S = "two";	n = 0;
	switch( S ) { case "one": n = 1; break; case "two": n = 2; break; }
	Report( n == 2, "switch_string" );

	// Iterators jump by code offset.
	n = 0;	foreach AllActors( class'Actor', It ) n++;
	Report( n > 10, "foreach_allactors" );
	n = 0;	foreach AllActors( class'Actor', It ) { n++; if( n == 3 ) break; }
	Report( n == 3, "foreach_break" );
}

defaultproperties
{
	InstStr="dflt"
}
