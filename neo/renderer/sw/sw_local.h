/* sw_local.h -- what the glue files (C++, see the engine) share. The core is behind sw_api.h. */
#ifndef SW_LOCAL_H
#define SW_LOCAL_H

#include "sw_api.h"

extern idCVar r_swRenderer;		// latched at start-up: images get their software twin only when it is set
extern idCVar r_swCompare;		// while GL is alive: 0 software, 1 GL (same process, same images), 2 split, 3 difference, 4 numbers
extern idCVar r_swThreads;
extern idCVar r_swShowStats;

// ---- sw_backend.cpp
bool		SW_Init( void );			// after GL is up; false = GL keeps rendering
void		SW_Shutdown( void );
bool		SW_Active( void );			// the software back end exists (images must feed it)
// r_swRenderer 2: no OpenGL at all. Valid before SW_Init (the window is created first): no WGL
// context, every qgl pointer a counted no-op (win_qgl.cpp), the frame presented natively.
bool		SW_GLFree( void );
// r_swRenderScale: the render size for a window of this size (unchanged unless r_swRenderer 2 and a scale below 1)
void		SW_ApplyRenderScale( int *width, int *height );
bool		SW_Presenting( void );		// ...and what is on screen this frame comes from it
void		SW_ExecuteBackEndCommands( const emptyCommand_t *cmds );
void		SW_RunFrame( const emptyCommand_t *cmds );	// what R_IssueRenderCommands calls: software, GL or both (r_swCompare)
bool		SW_SuppressGLSwap( void );					// RB_SwapBuffers: leave the frame in the back buffer
bool		SW_Executing( void );						// inside SW_ExecuteBackEndCommands: a CopyFramebuffer means the software frame
// what qglReadPixels( x, y, w, h, GL_RGB, GL_UNSIGNED_BYTE ) would deliver: bottom row first, rows padded to 4 bytes
void		SW_ReadPixels( int x, int y, int width, int height, byte *rgb );

// ---- sw_submit.cpp
void		SW_DrawView( const viewDef_t *viewDef );
void		SW_EndFrameImages( void );

// ---- sw_image.cpp
// level 0 exactly as it goes to qglTexImage2D (borders zeroed, red moved to alpha)
void		SW_ImageGenerate2D( idImage *image, const byte *pic, int width, int height, bool preserveBorder );
void		SW_ImageGenerateCube( idImage *image, const byte *pic[6], int size );
void		SW_ImageUploadScratch( idImage *image, const byte *pic, int cols, int rows );
void		SW_ImagePurge( idImage *image );
// idImage::CopyFramebuffer on the software frame; x, y as GL gives them (lower left corner, y up)
void		SW_ImageCopyFramebuffer( idImage *image, int x, int y, int width, int height );
// the image a draw samples; loads it on demand as idImage::Bind does. NULL = draw nothing.
const SwImage *SW_ImageForDraw( idImage *image );
// a cinematic frame: lives until SW_EndFrameImages
const SwImage *SW_ImageTransient( const byte *pic, int cols, int rows );

// ---- sw_present.cpp
void		SW_Present( const uint32_t *pixels, int pitch, int width, int height );	// row 0 on top; swaps
void		SW_PresentBeforeResize( int width, int height );	// before sw_resize: the presenter may hold the framebuffers' pages
void		SW_PresentBeginFrame( void );	// after sw_resize, before a frame's first tile: picks the framebuffer to render into
void		SW_PresentAsync( void );			// r_swRenderer 2: ends the core's frame on its coordinator thread, which presents it
void		SW_PresentShutdown( void );

// ---- sys/win32/win_qgl.cpp (GL-free mode)
void		QGL_NullReport( bool all );		// the stray GL calls so far, by entry point

#endif
