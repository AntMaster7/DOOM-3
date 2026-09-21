/* sw_present.cpp -- gets the finished frame onto the screen. Until Stage 9 removes GL the window
   still belongs to it, so the frame is uploaded into one texture and drawn as one quad, texel
   for pixel. This is the ONLY file of the software path that calls GL; it borrows the GL back
   end's state reset so that r_swCompare can hand the context back and forth. */
#include "../../idlib/precompiled.h"
#pragma hdrstop

#include "../tr_local.h"
#include "sw_local.h"
#include "../../sys/win32/win_local.h"		// win32.hWnd for the GL-free presenter

#include "sw_present_native.h"

idCVar r_swPresent( "r_swPresent", "0", CVAR_RENDERER | CVAR_INTEGER, "with r_swRenderer 2, how the frame reaches the window: 0 = best available (D3D12 reading the framebuffer in place), 2 = D3D12 through an upload buffer, 3 = GDI", 0, 3 );

idCVar r_swPresentBuffers( "r_swPresentBuffers", "2", CVAR_RENDERER | CVAR_INTEGER, "with r_swRenderer 2, the framebuffers rendered into in turn while the GPU still reads the last frame: 1 = wait for the GPU before every frame", 1, SW_MAX_FRAMEBUFFERS );

static GLuint	sw_presentTexture;
static int		sw_presentTexWidth, sw_presentTexHeight;

/*
================
SW_PresentNative

Stage 9: no GL. The window is the engine's, the swap chain is ours.
================
*/
static int	sw_fbIndex;			// the core's framebuffer this frame is rendered into
static const uint32_t *sw_fbPixels;	// ...its pixels, as sw_framebuffer_select returned them (asking again would be a sync point)
static int	sw_fbPitch;
static int	sw_asyncInterval;
static bool	sw_fbFlip;			// the last command list ended in a present: the next one takes the next framebuffer
static int	sw_fbWidth, sw_fbHeight;

static void SW_PresentNative( const uint32_t *pixels, int pitch, int width, int height ) {
	int fbPitch;
	const uint32_t *fb = sw_framebuffer( &fbPitch );
	if ( pixels != fb || pitch != fbPitch ) {
		return;		// every GL-free caller presents the core's own frame
	}
	if ( SWN_Setup( win32.hWnd, width, height, fbPitch, sw_framebuffer_bytes(), r_swPresent.GetInteger(), r_swapInterval.GetInteger() > 0 ) ) {
		common->Printf( "%ssoftware renderer presents through %s\n", SWN_Message(), SWN_ModeName() );
	}
	SWN_Present( pixels, sw_fbIndex, r_swapInterval.GetInteger() );
	sw_fbFlip = true;
}

/*
================
SW_PresentBeforeResize

The core is about to free its framebuffers if the size changed, and the presenter's heaps stand
for those very pages.
================
*/
void SW_PresentBeforeResize( int width, int height ) {
	if ( SW_GLFree() && ( width != sw_fbWidth || height != sw_fbHeight ) ) {
		sw_wait();		// the coordinator may be presenting
		SWN_Shutdown();
		sw_fbIndex = 0;
		sw_fbFlip = false;
	}
	sw_fbWidth = width;
	sw_fbHeight = height;
}

/*
================
SW_PresentBeginFrame

The GPU reads a presented frame in place, some time after Present returns, so the next frame is
rendered into the next framebuffer (r_swPresentBuffers of them, in turn), and what is waited for
is only the last copy out of THAT one. The flip happens at the start of the command list that
follows a present, not at the present: a screenshot or a savegame picture taken in between
still reads the frame that is on screen.
================
*/
void SW_PresentBeginFrame( void ) {
	if ( !SW_GLFree() ) {
		return;
	}
	if ( sw_fbFlip ) {
		sw_fbFlip = false;
		sw_fbIndex = ( sw_fbIndex + 1 ) % idMath::ClampInt( 1, SW_MAX_FRAMEBUFFERS, r_swPresentBuffers.GetInteger() );
	}
	sw_fbPixels = sw_framebuffer_select( sw_fbIndex );
	sw_framebuffer( &sw_fbPitch );
	// here, on the engine's thread, because it prints; the coordinator only ever presents
	if ( SWN_Setup( win32.hWnd, glConfig.vidWidth, glConfig.vidHeight, sw_fbPitch, sw_framebuffer_bytes(), r_swPresent.GetInteger(), r_swapInterval.GetInteger() > 0 ) ) {
		common->Printf( "%ssoftware renderer presents through %s\n", SWN_Message(), SWN_ModeName() );
	}
	SWN_BeginFrame( sw_fbIndex, r_swapInterval.GetInteger() );
}

/*
================
SW_PresentAsync

The coordinator thread calls SW_PresentAsyncDone when the frame's last tile is stored. No engine
call in there (I7): the swap chain exists (SW_PresentBeginFrame), the values are copies.
================
*/
static void SW_PresentAsyncDone( void *ctx ) {
	(void)ctx;
	SWN_Present( sw_fbPixels, sw_fbIndex, sw_asyncInterval );
}

void SW_PresentAsync( void ) {
	sw_asyncInterval = r_swapInterval.GetInteger();
	sw_fbFlip = true;
	sw_end_frame_async( SW_PresentAsyncDone, NULL );
}

void SW_PresentShutdown( void ) {
	if ( SW_GLFree() ) {
		sw_wait();		// the coordinator may be presenting
		common->Printf( "%s\n", SWN_Timing() );
		SWN_Shutdown();
		sw_fbIndex = 0;
		sw_fbFlip = false;
		sw_fbWidth = sw_fbHeight = 0;
		return;
	}
	if ( sw_presentTexture && qglDeleteTextures ) {
		qglDeleteTextures( 1, &sw_presentTexture );
	}
	sw_presentTexture = 0;
	sw_presentTexWidth = sw_presentTexHeight = 0;
}

void SW_Present( const uint32_t *pixels, int pitch, int width, int height ) {
	if ( !pixels || width <= 0 || height <= 0 ) {
		return;
	}
	if ( SW_GLFree() ) {
		SW_PresentNative( pixels, pitch, width, height );
		return;
	}

	RB_SetDefaultGLState();		// everything off, state cache invalidated

	int texWidth = width, texHeight = height;
	if ( !glConfig.textureNonPowerOfTwoAvailable ) {
		texWidth = MakePowerOfTwo( width );
		texHeight = MakePowerOfTwo( height );
	}

	qglDisable( GL_DEPTH_TEST );
	qglDisable( GL_BLEND );
	qglDisable( GL_CULL_FACE );
	qglDisable( GL_SCISSOR_TEST );
	qglDepthMask( GL_FALSE );
	qglViewport( 0, 0, width, height );
	qglDrawBuffer( GL_BACK );

	qglMatrixMode( GL_PROJECTION );
	qglLoadIdentity();
	qglOrtho( 0, 1, 0, 1, -1, 1 );
	qglMatrixMode( GL_MODELVIEW );
	qglLoadIdentity();

	GL_SelectTexture( 0 );
	qglEnable( GL_TEXTURE_2D );
	if ( !sw_presentTexture ) {
		qglGenTextures( 1, &sw_presentTexture );
	}
	qglBindTexture( GL_TEXTURE_2D, sw_presentTexture );
	if ( texWidth != sw_presentTexWidth || texHeight != sw_presentTexHeight ) {
		qglTexImage2D( GL_TEXTURE_2D, 0, GL_RGB8, texWidth, texHeight, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
		qglTexParameterf( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
		sw_presentTexWidth = texWidth;
		sw_presentTexHeight = texHeight;
	}
	qglPixelStorei( GL_UNPACK_ROW_LENGTH, pitch );
	qglPixelStorei( GL_UNPACK_ALIGNMENT, 4 );
	qglTexSubImage2D( GL_TEXTURE_2D, 0, 0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels );
	qglPixelStorei( GL_UNPACK_ROW_LENGTH, 0 );

	// the frame's row 0 is the top row: it becomes the texture's t = 0, drawn at the top of the window
	const float sMax = (float)width / (float)texWidth, tMax = (float)height / (float)texHeight;
	qglColor4f( 1, 1, 1, 1 );
	qglBegin( GL_QUADS );
	qglTexCoord2f( 0, tMax );		qglVertex2f( 0, 0 );
	qglTexCoord2f( sMax, tMax );	qglVertex2f( 1, 0 );
	qglTexCoord2f( sMax, 0 );		qglVertex2f( 1, 1 );
	qglTexCoord2f( 0, 0 );			qglVertex2f( 0, 1 );
	qglEnd();

	qglBindTexture( GL_TEXTURE_2D, 0 );
	qglDisable( GL_TEXTURE_2D );
	qglEnable( GL_SCISSOR_TEST );
	qglDepthMask( GL_TRUE );

	// the GL back end's next frame starts from RB_SetDefaultGLState as well
	backEnd.glState.forceGlState = true;
	for ( int i = 0; i < MAX_MULTITEXTURE_UNITS; i++ ) {
		backEnd.glState.tmu[i].current2DMap = -1;
	}

	if ( !r_frontBuffer.GetBool() ) {
		GLimp_SwapBuffers();
	}
}
