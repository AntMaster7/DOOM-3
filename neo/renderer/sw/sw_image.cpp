/* sw_image.cpp -- the software twin of an idImage.

   idImage builds every mip level on the CPU before it uploads, so the hooks sit where the
   texels go to qglTexImage2D and hand over the same bytes. What the core stores is what a GL
   sampler RETURNS, which differs from what was uploaded in one case that matters:

   - an internal format without alpha (GL_RGB8, RGB5, DXT1: SelectInternalFormat picks them when
     alpha is all 255 OR all 0, and always for TD_SPECULAR) samples alpha = 1. Several hundred
     TGAs carry an all-zero alpha channel; blended as uploaded they would vanish.
   - GL_INTENSITY8 / LUMINANCE8_ALPHA8 are only chosen when the channels already agree.

   Filters: TF_DEFAULT is the mip chain (the core reads GL_LINEAR_MIPMAP_NEAREST's level);
   TF_LINEAR is level 0 alone. TF_NEAREST is treated as TF_LINEAR: every engine image that asks
   for it is a constant colour (white, black, flat normal), a material keyword "nearest" is the
   only other source. TR_CLAMP_TO_BORDER (one experimental image) is clamp to edge.

   Precompressed .dds files are refused while the software back end is active
   (idImage::CheckPrecompressedImage): the TGA path is the only one that has RGBA on the CPU. */
#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../tr_local.h"
#include "sw_local.h"

idCVar r_swNoMips("r_swNoMips", "0", CVAR_RENDERER | CVAR_BOOL | CVAR_INIT, "diagnosis: the software images keep level 0 only. Pair it with image_filter GL_LINEAR: if a difference against GL goes away, it was the mip level choice" );

static bool SW_FormatHasAlpha( int internalFormat ) {
	switch ( internalFormat ) {
	case GL_RGB8:
	case GL_RGB5:
	case GL_RGB:
	case 3:
	case GL_COMPRESSED_RGB_S3TC_DXT1_EXT:
	case GL_LUMINANCE8:
		return false;
	}
	return true;
}

static void SW_ForceOpaque( byte *pic, int texels ) {
	for ( int i = 0; i < texels; i++ ) {
		pic[i * 4 + 3] = 255;
	}
}

static int SW_Wrap( textureRepeat_t repeat ) {
	return repeat == TR_REPEAT ? SW_WRAP_REPEAT : SW_WRAP_CLAMP;
}

void SW_ImagePurge( idImage *image ) {
	if ( image->swImage ) {
		sw_image_destroy( image->swImage );
		image->swImage = NULL;
	}
}

/*
================
SW_ImageGenerate2D

pic is level 0 as it goes to GL: power of two, downsized, borders set, red moved to alpha for
bump maps. The chain is built with R_MipMap exactly as idImage::GenerateImage builds GL's.
================
*/
void SW_ImageGenerate2D( idImage *image, const byte *pic, int width, int height, bool preserveBorder ) {
	SW_ImagePurge( image );

	const byte	*levels[16];
	byte		*owned[16];
	int			numLevels = 0;

	byte *level0 = (byte *)R_StaticAlloc( width * height * 4 );
	memcpy( level0, pic, width * height * 4 );
	if ( !SW_FormatHasAlpha( image->internalFormat ) ) {
		SW_ForceOpaque( level0, width * height );
	}
	owned[0] = level0;
	levels[numLevels++] = level0;

	// r_swNoMips: level 0 only, to compare against GL with image_filter GL_LINEAR (is a difference the LOD choice?)
	if ( image->filter == TF_DEFAULT && !r_swNoMips.GetBool() ) {
		int w = width, h = height;
		while ( ( w > 1 || h > 1 ) && numLevels < 16 ) {
			owned[numLevels] = R_MipMap( owned[numLevels - 1], w, h, preserveBorder );
			levels[numLevels] = owned[numLevels];
			w = w > 1 ? w >> 1 : 1;
			h = h > 1 ? h >> 1 : 1;
			// image_colorMipLevels: idImage::GenerateImage tints each level of a diffuse map (1 red, 2 green,
			// 3 blue, ..) and mips onward from the tinted one. The same here, so that r_swCompare shows where
			// the two renderers pick different levels.
			if ( image->depth == TD_DIFFUSE && globalImages->image_colorMipLevels.GetBool() ) {
				static const byte tint[3][4] = { { 255, 0, 0, 128 }, { 0, 255, 0, 128 }, { 0, 0, 255, 128 } };
				R_BlendOverTexture( owned[numLevels], w * h, tint[ ( numLevels - 1 ) % 3 ] );
			}
			numLevels++;
		}
	}

	image->swImage = sw_image_create( width, height, numLevels, levels, SW_Wrap( image->repeat ) );

	for ( int i = 0; i < numLevels; i++ ) {
		R_StaticFree( owned[i] );
	}
}

void SW_ImageGenerateCube( idImage *image, const byte *pic[6], int size ) {
	SW_ImagePurge( image );

	byte		*owned[6][16];
	const byte	*levels[6][16];
	const uint8_t *const *faces[6];
	int			numLevels = 0;
	const bool	opaque = !SW_FormatHasAlpha( image->internalFormat );

	for ( int f = 0; f < 6; f++ ) {
		numLevels = 0;
		owned[f][0] = (byte *)R_StaticAlloc( size * size * 4 );
		memcpy( owned[f][0], pic[f], size * size * 4 );
		if ( opaque ) {
			SW_ForceOpaque( owned[f][0], size * size );
		}
		levels[f][numLevels++] = owned[f][0];
		if ( image->filter == TF_DEFAULT ) {
			for ( int w = size; w > 1 && numLevels < 16; w >>= 1 ) {
				owned[f][numLevels] = R_MipMap( owned[f][numLevels - 1], w, w, false );
				levels[f][numLevels] = owned[f][numLevels];
				numLevels++;
			}
		}
		faces[f] = levels[f];
	}

	image->swImage = sw_image_create_cube( size, numLevels, faces );

	for ( int f = 0; f < 6; f++ ) {
		for ( int i = 0; i < numLevels; i++ ) {
			R_StaticFree( owned[f][i] );
		}
	}
}

/*
================
SW_ImageUploadScratch

idImage::UploadScratch: GL_RGB8 (alpha reads 1), GL_LINEAR, GL_REPEAT, no mips.
rows == cols * 6 is a cube map animation.
================
*/
static SwImage *SW_CreateScratch( const byte *pic, int cols, int rows ) {
	SwImage *result;
	if ( rows == cols * 6 ) {
		byte *copy = (byte *)R_StaticAlloc( cols * rows * 4 );
		memcpy( copy, pic, cols * rows * 4 );
		SW_ForceOpaque( copy, cols * rows );
		const byte *level[6];
		const uint8_t *const *faces[6];
		for ( int f = 0; f < 6; f++ ) {
			level[f] = copy + cols * cols * 4 * f;
			faces[f] = &level[f];
		}
		result = sw_image_create_cube( cols, 1, faces );
		R_StaticFree( copy );
	} else {
		byte *copy = (byte *)R_StaticAlloc( cols * rows * 4 );
		memcpy( copy, pic, cols * rows * 4 );
		SW_ForceOpaque( copy, cols * rows );
		const byte *level = copy;
		result = sw_image_create( cols, rows, 1, &level, SW_WRAP_REPEAT );
		R_StaticFree( copy );
	}
	return result;
}

void SW_ImageUploadScratch( idImage *image, const byte *pic, int cols, int rows ) {
	SW_ImagePurge( image );
	image->swImage = SW_CreateScratch( pic, cols, rows );
}

/*
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

The GL back end uploads a cinematic frame into the one cinematicImage and draws at once. Here a
draw is deferred to the end of the view, so every cinematic draw gets an image of its own that
lives until the frame is over.
================
*/
static idList<SwImage *>	sw_transientImages;

const SwImage *SW_ImageTransient( const byte *pic, int cols, int rows ) {
	SwImage *image = SW_CreateScratch( pic, cols, rows );
	if ( image ) {
		sw_transientImages.Append( image );
	}
	return image;
}

void SW_EndFrameImages( void ) {
	for ( int i = 0; i < sw_transientImages.Num(); i++ ) {
		sw_image_destroy( sw_transientImages[i] );
	}
	sw_transientImages.SetNum( 0, false );
}

/*
================
SW_ImageForDraw

idImage::Bind's duties without GL: load on demand, keep the usage statistics.
================
*/
const SwImage *SW_ImageForDraw( idImage *image ) {
	if ( !image ) {
		return NULL;
	}
	if ( !image->swImage ) {
		if ( image->texnum == idImage::TEXTURE_NOT_LOADED ) {
			// not loaded at all: the normal on-demand load, which comes back through the hooks
			image->ActuallyLoadImage( true, true );
		}
		if ( !image->swImage ) {
			// an image GL has and we do not (a 3D texture, a capture that never happened): draw with the default
			image = globalImages->defaultImage;
			if ( !image->swImage ) {
				return NULL;
			}
		}
	}
	image->frameUsed = backEnd.frameCount;
	image->bindCount++;
	return image->swImage;
}
