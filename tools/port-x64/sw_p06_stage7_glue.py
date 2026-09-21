import re
R = 'C:/Source/DOOM-3/neo/renderer/sw/'

def patch(path, pairs):
    s = open(path, encoding='utf-8').read()
    for old, new in pairs:
        assert s.count(old) == 1, (path, old[:70])
        s = s.replace(old, new)
    open(path, 'w', encoding='utf-8', newline='').write(s)
    print('patched', path)

# ------------------------------------------------------------------ sw_local.h
patch(R + 'sw_local.h', [
("""bool		SW_SuppressGLSwap( void );					// RB_SwapBuffers: leave the frame in the back buffer""",
"""bool		SW_SuppressGLSwap( void );					// RB_SwapBuffers: leave the frame in the back buffer
bool		SW_Executing( void );						// inside SW_ExecuteBackEndCommands: a CopyFramebuffer means the software frame"""),
("""void		SW_ImagePurge( idImage *image );""",
"""void		SW_ImagePurge( idImage *image );
// idImage::CopyFramebuffer on the software frame; x, y as GL gives them (lower left corner, y up)
void		SW_ImageCopyFramebuffer( idImage *image, int x, int y, int width, int height );"""),
])

# ------------------------------------------------------------------ sw_image.cpp
patch(R + 'sw_image.cpp', [
("""/*
================
SW_ImageTransient
""", """/*
================
SW_ImageCopyFramebuffer

idImage::CopyFramebuffer: GL_RGB8 (alpha reads 1), GL_LINEAR, clamp, padded to a power of two,
and stored THE GL WAY: the first row of the image is the BOTTOM row of the rectangle, which is
what every material that reads a capture expects (conventions table). The extra row and column
GL duplicates for the bilerp are duplicated here too.
================
*/
void SW_ImageCopyFramebuffer( idImage *image, int x, int y, int width, int height ) {
	if ( cvarSystem->GetCVarBool( "g_lowresFullscreenFX" ) ) {
		width = 512;
		height = 512;
	}
	if ( width <= 0 || height <= 0 ) {
		return;
	}
	const int potWidth = MakePowerOfTwo( width ), potHeight = MakePowerOfTwo( height );

	static idList<uint32_t> rows;
	rows.SetNum( width * height, false );
	SwRect rect;
	rect.x0 = x;
	rect.x1 = x + width - 1;
	rect.y0 = glConfig.vidHeight - ( y + height );		// the rectangle's top row on screen
	rect.y1 = rect.y0 + height - 1;
	sw_read_pixels( &rect, rows.Ptr(), width );

	byte *pic = (byte *)R_StaticAlloc( potWidth * potHeight * 4 );
	memset( pic, 0, potWidth * potHeight * 4 );
	for ( int r = 0; r < height; r++ ) {
		// image row r = screen row (bottom + r), counted upwards
		const uint32_t *src = rows.Ptr() + (size_t)( height - 1 - r ) * width;
		uint32_t *dst = (uint32_t *)pic + (size_t)r * potWidth;
		memcpy( dst, src, (size_t)width * 4 );
		if ( width < potWidth ) {
			dst[width] = dst[width - 1];
		}
	}
	if ( height < potHeight ) {
		memcpy( pic + (size_t)height * potWidth * 4, pic + (size_t)( height - 1 ) * potWidth * 4, (size_t)potWidth * 4 );
	}
	SW_ForceOpaque( pic, potWidth * potHeight );

	SW_ImagePurge( image );
	const byte *level = pic;
	image->swImage = sw_image_create( potWidth, potHeight, 1, &level, SW_WRAP_CLAMP );
	image->uploadWidth = potWidth;
	image->uploadHeight = potHeight;
	image->type = TT_2D;
	R_StaticFree( pic );
}

/*
================
SW_ImageTransient
"""),
])

# ------------------------------------------------------------------ sw_backend.cpp
patch(R + 'sw_backend.cpp', [
("""static bool	sw_suppressGLSwap;
""", """static bool	sw_suppressGLSwap;
static bool	sw_executing;
"""),
("""bool SW_SuppressGLSwap( void ) {
	return sw_suppressGLSwap;
}""", """bool SW_SuppressGLSwap( void ) {
	return sw_suppressGLSwap;
}

bool SW_Executing( void ) {
	return sw_executing;
}"""),
("""		case RC_COPY_RENDER:
			// Stage 7: _scratch / _currentRender captures
			break;""", """		case RC_COPY_RENDER: {
			// RB_CopyRender; the image's hook reads the software frame while sw_executing is set (I5: the
			// views before this command have ended, so the frame is whole)
			const copyRenderCommand_t *copy = (const copyRenderCommand_t *)cmds;
			if ( copy->image && !r_skipCopyTexture.GetBool() ) {
				copy->image->CopyFramebuffer( copy->x, copy->y, copy->imageWidth, copy->imageHeight, false );
			}
			break;
		}"""),
("""	sw_resize( glConfig.vidWidth, glConfig.vidHeight );
	memset( &sw_frame, 0, sizeof( sw_frame ) );
""", """	sw_resize( glConfig.vidWidth, glConfig.vidHeight );
	memset( &sw_frame, 0, sizeof( sw_frame ) );
	sw_executing = true;
"""),
("""	SW_EndFrameImages();

	backEnd.pc.msec""", """	SW_EndFrameImages();
	sw_executing = false;

	backEnd.pc.msec"""),
])
print('ok')
