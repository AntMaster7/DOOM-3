import sys
sys.path.insert(0, 'C:/Users/samue/AppData/Local/Temp/claude/C--Source-DOOM-3/5faf42c1-1026-4cc6-85bf-861d18de5b31/scratchpad')
from px import patch

for g in sys.argv[1:] or ['game']:
    # back to pointer-sized entity / object slots: the compiler retypes pointer temporaries as
    # entity temporaries, so the two must have one size
    patch(g + '/script/Script_Program.cpp', [
    ('idTypeDef	type_entity( ev_entity, &def_entity, "entity", sizeof( int ), NULL );	',
     'idTypeDef	type_entity( ev_entity, &def_entity, "entity", sizeof( int * ), NULL );'),
    ('idTypeDef	type_object( ev_object, &def_object, "object", sizeof( int ), NULL );	',
     'idTypeDef	type_object( ev_object, &def_object, "object", sizeof( int * ), NULL );'),
    ])
    patch(g + '/script/Script_Interpreter.h', [
    ("	void				Push( int value );", "	void				Push( int value );\n	void				PushEntity( int entityNumber );"),
    ("""ID_INLINE void idInterpreter::Push( int value ) {
	if ( localstackUsed + sizeof( int ) > LOCALSTACK_SIZE ) {
		Error( "Push: locals stack overflow\\n" );
	}
	*( int * )&localstack[ localstackUsed ]	= value;
	localstackUsed += sizeof( int );
}
""", """ID_INLINE void idInterpreter::Push( int value ) {
	if ( localstackUsed + sizeof( int ) > LOCALSTACK_SIZE ) {
		Error( "Push: locals stack overflow\\n" );
	}
	*( int * )&localstack[ localstackUsed ]	= value;
	localstackUsed += sizeof( int );
}

/*
====================
idInterpreter::PushEntity

An entity or object parameter is an entity number in a slot of type_entity's size, which is
sizeof( int * ): 4 bytes on x86, where a plain Push did, and 8 on x64, where it left the callee's
parameter offsets and the pop count 4 bytes out per entity ("locals stack underflow").
====================
*/
ID_INLINE void idInterpreter::PushEntity( int entityNumber ) {
	if ( localstackUsed + sizeof( int * ) > LOCALSTACK_SIZE ) {
		Error( "Push: locals stack overflow\\n" );
	}
	*( intptr_t * )&localstack[ localstackUsed ] = entityNumber;
	localstackUsed += sizeof( int * );
}
"""),
    ])
    patch(g + '/script/Script_Interpreter.cpp', [
    ("	Push( self->entityNumber + 1 );", "	PushEntity( self->entityNumber + 1 );"),
    ("			Push( *var_a.entityNumberPtr );", "			PushEntity( *var_a.entityNumberPtr );", 3),
    ])
print('done')
