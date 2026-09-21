import re
import sys
sys.path.insert(0, 'C:/Users/samue/AppData/Local/Temp/claude/C--Source-DOOM-3/5faf42c1-1026-4cc6-85bf-861d18de5b31/scratchpad')
from px import patch, NEO

for game in sys.argv[1:] or ['game']:
    g = game + '/'
    patch(g + 'gamesys/Class.h', [
    ("""	int			type;
	int			value;
""", """	int			type;
	intptr_t	value;				// an int, a float's bits, or a pointer: event arguments travel in pointer-sized slots
"""),
    ("value = reinterpret_cast<int>( &data ); };", "value = reinterpret_cast<intptr_t>( &data ); };"),
    ("value = reinterpret_cast<int>( data.c_str() ); };", "value = reinterpret_cast<intptr_t>( data.c_str() ); };"),
    ("value = reinterpret_cast<int>( data ); };", "value = reinterpret_cast<intptr_t>( data ); };", 3),
    ("	bool						ProcessEventArgPtr( const idEventDef *ev, int *data );", "	bool						ProcessEventArgPtr( const idEventDef *ev, intptr_t *data );"),
    ])
    patch(g + 'gamesys/Class.cpp', [
    ("	int			data[ D_EVENT_MAXARGS ];\n	va_list		args;", "	intptr_t	data[ D_EVENT_MAXARGS ];\n	va_list		args;"),
    ("bool idClass::ProcessEventArgPtr( const idEventDef *ev, int *data ) {", "bool idClass::ProcessEventArgPtr( const idEventDef *ev, intptr_t *data ) {"),
    ])
    patch(g + 'gamesys/Event.h', [
    ("va_list args, int data[ D_EVENT_MAXARGS ]  );", "va_list args, intptr_t data[ D_EVENT_MAXARGS ]  );"),
    ])
    patch(g + 'gamesys/Event.cpp', [
    ("va_list args, int data[ D_EVENT_MAXARGS ] ) {", "va_list args, intptr_t data[ D_EVENT_MAXARGS ] ) {"),
    ("	int			args[ D_EVENT_MAXARGS ];", "	intptr_t	args[ D_EVENT_MAXARGS ];"),
    ("			*reinterpret_cast<int *>( dataPtr ) = arg->value;", "			*reinterpret_cast<int *>( dataPtr ) = (int)arg->value;"),
    ])
    patch(g + 'script/Script_Interpreter.cpp', [
    ("	int					data[ D_EVENT_MAXARGS ];", "	intptr_t			data[ D_EVENT_MAXARGS ];", 2),
    ])
    patch(g + 'gamesys/TypeInfo.cpp', [
    ("varPtr == (void *)0xcdcdcdcd", "varPtr == (void *)(size_t)0xcdcdcdcd"),
    ])
    # the generated callback switch: every non-float argument is an integer-class register or
    # stack slot on x64, int and pointer alike, so the typedefs take intptr_t
    full = NEO + g + 'gamesys/Callbacks.cpp'
    raw = open(full, 'rb').read().decode('latin-1')
    raw2, n = re.subn(r'\bconst int\b', 'const intptr_t', raw)
    open(full, 'wb').write(raw2.encode('latin-1'))
    print(g + 'gamesys/Callbacks.cpp', n, 'typedef arguments')
print('done')
