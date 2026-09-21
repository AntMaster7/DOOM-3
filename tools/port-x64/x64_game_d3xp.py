# The rest of x64_game.py for neo/d3xp, whose Event.cpp differs from the base game's (a second
# `int args[ D_EVENT_MAXARGS ]`: the fast-event path). x64_game.py stopped there with Class.h, Class.cpp
# and Event.h already written; this applies what was left. One-shot, as all of these scripts are.
import re, sys, os
sys.path.insert(0, os.path.dirname(__file__))
from px import patch, NEO

g = 'd3xp/'
patch(g + 'gamesys/Event.cpp', [
("va_list args, int data[ D_EVENT_MAXARGS ] ) {", "va_list args, intptr_t data[ D_EVENT_MAXARGS ] ) {"),
("	int			args[ D_EVENT_MAXARGS ];", "	intptr_t	args[ D_EVENT_MAXARGS ];", 2),
("			*reinterpret_cast<int *>( dataPtr ) = arg->value;", "			*reinterpret_cast<int *>( dataPtr ) = (int)arg->value;"),
])
patch(g + 'script/Script_Interpreter.cpp', [
("	int					data[ D_EVENT_MAXARGS ];", "	intptr_t			data[ D_EVENT_MAXARGS ];", 2),
])
patch(g + 'gamesys/TypeInfo.cpp', [
("varPtr == (void *)0xcdcdcdcd", "varPtr == (void *)(size_t)0xcdcdcdcd"),
])
full = NEO + g + 'gamesys/Callbacks.cpp'
raw = open(full, 'rb').read().decode('latin-1')
raw2, n = re.subn(r'\bconst int\b', 'const intptr_t', raw)
open(full, 'wb').write(raw2.encode('latin-1'))
print(g + 'gamesys/Callbacks.cpp', n, 'typedef arguments')
print('done')
