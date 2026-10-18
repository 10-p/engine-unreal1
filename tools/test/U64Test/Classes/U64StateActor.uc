//=============================================================================
// U64StateActor -- state code, labels, latent Sleep and GotoState run from code offsets (the label table and the
// state frame) that the 64-bit port must leave where the compiler put them. Reports to U64Game.
//=============================================================================
class U64StateActor expands Info;

var int Stage;

auto state Running
{
Begin:
	Stage = 1;
	Sleep( 0.2 );
	Goto( 'Second' );
Second:
	Stage = 2;
	GotoState( 'Finished' );
}

state Finished
{
Begin:
	if( Stage == 2 )
		Stage = 3;
	if( U64Game(Level.Game) != None )
		U64Game(Level.Game).StateDone( Stage == 3 );
}
