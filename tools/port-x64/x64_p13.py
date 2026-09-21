import sys
sys.path.insert(0, 'C:/Users/samue/AppData/Local/Temp/claude/C--Source-DOOM-3/5faf42c1-1026-4cc6-85bf-861d18de5b31/scratchpad')
from px import patch

for g in sys.argv[1:] or ['game']:
    patch(g + '/script/Script_Program.cpp', [
    ("""		if ( type->Inherits( &type_object ) ) {
			// objects only have their entity number on the stack, not the entire object
			scope->value.functionPtr->locals += type_object.Size();
		} else {
			scope->value.functionPtr->locals += type->Size();
		}""", """		if ( type->Inherits( &type_object ) ) {
			// objects only have their entity number on the stack, not the entire object
			scope->value.functionPtr->locals += type_object.Size();
		} else if ( !strcmp( name, RESULT_STRING ) && type->Size() < (int)sizeof( void * ) ) {
			// The compiler turns the OP_INDIRECT_x before an assignment into OP_ADDRESS and retypes
			// its result temporary as a pointer AFTER that temporary was allocated with the field's
			// size. A float is as big as a pointer on x86; on x64 the 8-byte pointer ran over the
			// next local. A result temporary therefore always has room for a pointer.
			scope->value.functionPtr->locals += sizeof( void * );
		} else {
			scope->value.functionPtr->locals += type->Size();
		}"""),
    ])
print('done')
