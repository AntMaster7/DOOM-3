import sys
sys.path.insert(0, 'C:/Users/samue/AppData/Local/Temp/claude/C--Source-DOOM-3/5faf42c1-1026-4cc6-85bf-861d18de5b31/scratchpad')
from px import patch

for g in sys.argv[1:] or ['game']:
    patch(g + '/script/Script_Program.cpp', [
    ('idTypeDef	type_entity( ev_entity, &def_entity, "entity", sizeof( int * ), NULL );',
     'idTypeDef	type_entity( ev_entity, &def_entity, "entity", sizeof( int ), NULL );	'),
    ('idTypeDef	type_object( ev_object, &def_object, "object", sizeof( int * ), NULL );',
     'idTypeDef	type_object( ev_object, &def_object, "object", sizeof( int ), NULL );	'),
    ])
print('done')
