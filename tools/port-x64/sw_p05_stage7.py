"""Stage 7 glue, engine side: the name of an ARB program (new-style stages map to kernels by
name) and idImage::CopyFramebuffer for the software frame."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('renderer/draw_arb2.cpp', [
("""/*
==================
R_ReloadARBPrograms_f
==================
*/""", """#ifdef ID_SW_RENDERER
/*
==================
R_ARBProgramName

The software renderer has a kernel per shipped program and finds it by the file name.
==================
*/
const char *R_ARBProgramName( int ident ) {
	for ( int i = 0 ; i < MAX_GLPROGS && progs[i].name[0] ; i++ ) {
		if ( progs[i].ident == ident ) {
			return progs[i].name;
		}
	}
	return "";
}
#endif

/*
==================
R_ReloadARBPrograms_f
==================
*/"""),
])

patch('renderer/Image_load.cpp', [
("""void idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, bool useOversizedBuffer ) {
	Bind();
""", """void idImage::CopyFramebuffer( int x, int y, int imageWidth, int imageHeight, bool useOversizedBuffer ) {
#ifdef ID_SW_RENDERER
	// the software back end is executing: the frame to copy is its own
	if ( SW_Executing() ) {
		SW_ImageCopyFramebuffer( this, x, y, imageWidth, imageHeight );
		return;
	}
#endif
	Bind();
"""),
])
print('done')
