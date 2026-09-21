import sys
sys.path.insert(0, 'C:/Users/samue/AppData/Local/Temp/claude/C--Source-DOOM-3/5faf42c1-1026-4cc6-85bf-861d18de5b31/scratchpad')
from px import patch

for g in sys.argv[1:] or ['game']:
    patch(g + '/script/Script_Program.cpp', [
    # (int &)size on a size_t read half of it
    ("""	idStr typeName;
	size_t size;

	savefile->ReadString( typeName );""", """	idStr typeName;
	int size;		// was size_t read through (int &): half of it on x64

	savefile->ReadString( typeName );"""),
    ])
    patch(g + '/gamesys/Event.cpp', [
    ("	savefile->ReadInt( (int&)trace.c.material );", "	{ int isSet = 0; savefile->ReadInt( isSet ); trace.c.material = (const idMaterial *)(size_t)(unsigned int)isSet; }	// a flag, not a pointer"),
    ("	savefile->WriteInt( (int&)trace.c.material );", "	savefile->WriteInt( trace.c.material != NULL );"),
    ])
print('done')
