//=============================================================================
// U64Decl -- ufront 2.84: the declarations the other fixture classes never compile -- an enum, a struct with
// object, class, string, static-array, vector and rotator members, consts, ArrayCount, vect()/rot() constants.
// Compile-only: its test is the compiler round-trip (a 64-bit `Unreal.bin -MAKE` must write the same bytes as
// the 32-bit one, but for the package GUID).
//=============================================================================
class U64Decl expands Info;

const DeclCount = 3;
const DeclName = "u64";

enum EDeclKind
{
	DK_None,
	DK_Object,
	DK_String
};

struct DeclEntry
{
	var EDeclKind Kind;
	var Actor Target;
	var string[16] Label;
	var int Values[3];
	var class<Actor> TargetClass;
	var vector Where;
	var rotator Facing;
};

var DeclEntry Entries[3];
var DeclEntry Single;
var EDeclKind Kinds[4];

function int Fill( Actor Other )
{
	local DeclEntry E;
	local int i;

	E.Kind = DK_Object;
	E.Target = Other;
	E.Label = DeclName;
	E.Values[0] = DeclCount;
	E.Values[2] = DeclCount * 2;
	E.TargetClass = class'Info';
	E.Where = vect(1,2,3);
	E.Facing = rot(100,200,300);
	for( i=0; i<ArrayCount(Entries); i++ )
		Entries[i] = E;
	Single = Entries[1];
	if( Single == E && Single.Target == Other && Kinds[3] == DK_String )
		return ArrayCount(Entries) + ArrayCount(Kinds);
	return -1;
}

defaultproperties
{
	Kinds(0)=DK_Object
	Kinds(3)=DK_String
}
