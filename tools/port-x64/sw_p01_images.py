"""Stage 3: idImage gets its software twin. Every hook is inside ID_SW_RENDERER (x64 projects only)."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('renderer/Image.h', [
("""	GLuint				texnum;					// gl texture binding, will be TEXTURE_NOT_LOADED if not loaded
""", """	GLuint				texnum;					// gl texture binding, will be TEXTURE_NOT_LOADED if not loaded
	struct SwImage *	swImage;				// the software renderer's copy (renderer/sw/sw_image.cpp), NULL if none
"""),
("""	texnum = TEXTURE_NOT_LOADED;
	partialImage = NULL;""", """	texnum = TEXTURE_NOT_LOADED;
	swImage = NULL;
	partialImage = NULL;"""),
])

patch('renderer/Image_load.cpp', [
# GenerateImage: level 0 is final here (borders set, red moved to alpha)
("""	// upload the main image level
	Bind();


	if ( internalFormat == GL_COLOR_INDEX8_EXT ) {
		/*
		if ( depth == TD_BUMP ) {""", """#ifdef ID_SW_RENDERER
	if ( SW_Active() ) {
		SW_ImageGenerate2D( this, scaledBuffer, scaled_width, scaled_height, preserveBorder );
	}
#endif

	// upload the main image level
	Bind();


	if ( internalFormat == GL_COLOR_INDEX8_EXT ) {
		/*
		if ( depth == TD_BUMP ) {"""),
# GenerateCubeImage
("""	uploadHeight = scaled_height;
	uploadWidth = scaled_width;

	Bind();

	// no other clamp mode makes sense""", """	uploadHeight = scaled_height;
	uploadWidth = scaled_width;

#ifdef ID_SW_RENDERER
	if ( SW_Active() ) {
		SW_ImageGenerateCube( this, pic, size );
	}
#endif

	Bind();

	// no other clamp mode makes sense"""),
# precompressed images have no RGBA on the CPU
("""bool idImage::CheckPrecompressedImage( bool fullLoad ) {
	if ( !glConfig.isInitialized || !glConfig.textureCompressionAvailable ) {
		return false;
	}
""", """bool idImage::CheckPrecompressedImage( bool fullLoad ) {
	if ( !glConfig.isInitialized || !glConfig.textureCompressionAvailable ) {
		return false;
	}

#ifdef ID_SW_RENDERER
	// the software renderer needs the texels: only the TGA path has them on the CPU
	if ( SW_Active() ) {
		return false;
	}
#endif
"""),
("""void idImage::PurgeImage() {
	if ( texnum != TEXTURE_NOT_LOADED ) {""", """void idImage::PurgeImage() {
#ifdef ID_SW_RENDERER
	SW_ImagePurge( this );
#endif
	if ( texnum != TEXTURE_NOT_LOADED ) {"""),
("""void idImage::UploadScratch( const byte *data, int cols, int rows ) {
	int			i;
""", """void idImage::UploadScratch( const byte *data, int cols, int rows ) {
	int			i;

#ifdef ID_SW_RENDERER
	if ( SW_Active() ) {
		SW_ImageUploadScratch( this, data, cols, rows );
	}
#endif
"""),
])

# tr_local.h: the glue's declarations for the engine files that carry hooks
patch('renderer/tr_local.h', [
("""void R_CensusCommands(""", """#ifdef ID_SW_RENDERER
#include "sw/sw_local.h"
#endif

void R_CensusCommands("""),
])
print('done')
