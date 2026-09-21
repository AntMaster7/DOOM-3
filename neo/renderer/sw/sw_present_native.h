/* sw_present_native.h -- the GL-free presenter (sw_present_native.cpp). Plain types only: the
   implementation sees no engine header and the engine sees no D3D12 header. */
#ifndef SW_PRESENT_NATIVE_H
#define SW_PRESENT_NATIVE_H

#include <stddef.h>
#include <stdint.h>

/* (re)builds the presenter when anything changed; true = it did, SWN_Message() says how.
   way: 0 = best available, 2 = D3D12 without zero copy, 3 = GDI */
bool			SWN_Setup( void *hwnd, int width, int height, int pitchPixels, size_t fbBytes, int way, bool vsync );
/* before the first tile of a frame is stored into the core's framebuffer `index` */
void			SWN_BeginFrame( int index, int swapInterval );
/* `pixels` = the core's framebuffer `index`, finished */
void			SWN_Present( const uint32_t *pixels, int index, int swapInterval );
/* before the core frees or moves its framebuffers */
void			SWN_Shutdown( void );
const char *	SWN_ModeName( void );
const char *	SWN_Timing( void );							// where present time went, since start-up
const char *	SWN_Message( void );						// what failed on the way to the mode in use

#endif
