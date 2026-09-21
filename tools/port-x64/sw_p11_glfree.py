# Stage 9: r_swRenderer 2 = no OpenGL. The window is created without a pixel format or a WGL
# context, QGL_InitNull (generated: gen_qgl_null.py) replaces QGL_Init, and nothing swaps.
import os
ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), '..', '..', 'neo'))

def patch(rel, pairs):
    path = os.path.join(ROOT, rel)
    s = open(path, encoding='latin-1').read()
    for old, new in pairs:
        if new in s:
            continue
        assert s.count(old) == 1, (rel, s.count(old), old[:60])
        s = s.replace(old, new)
    open(path, 'w', encoding='latin-1', newline='').write(s)
    print('patched', rel)

patch('sys/win32/win_qgl.cpp', [
("""void GLimp_EnableLogging( bool enable ) {
	static bool		isEnabled;""",
"""#ifdef ID_SW_RENDERER
#include "win_qgl_null.inl"
#endif

void GLimp_EnableLogging( bool enable ) {
	static bool		isEnabled;
#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		return;		// the log wrappers call through the dll pointers: nothing to log
	}
#endif"""),
])

patch('sys/win32/win_glimp.cpp', [
# the window without a pixel format and without a context
("""	if ( !GLW_InitDriver( parms ) ) {
		ShowWindow( win32.hWnd, SW_HIDE );""",
"""#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		// the presenter attaches its swap chain to the window; the DC is only for the gamma ramp
		win32.hDC = GetDC( win32.hWnd );
		glConfig.colorBits = 32;
		glConfig.depthBits = 24;		// what the front end believes; the rasterizer's depth is float32
		glConfig.stencilBits = 8;
	} else
#endif
	if ( !GLW_InitDriver( parms ) ) {
		ShowWindow( win32.hWnd, SW_HIDE );"""),
("""	driverName = r_glDriver.GetString()[0] ? r_glDriver.GetString() : "opengl32";
	if ( !QGL_Init( driverName ) ) {""",
"""	driverName = r_glDriver.GetString()[0] ? r_glDriver.GetString() : "opengl32";
#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		QGL_InitNull();
		if ( parms.fullScreen && !GLW_SetFullScreen( parms ) ) {
			GLimp_Shutdown();
			return false;
		}
		if ( !GLW_CreateWindow( parms ) ) {
			GLimp_Shutdown();
			return false;
		}
		return true;
	}
#endif
	if ( !QGL_Init( driverName ) ) {"""),
("""bool QGL_Init( const char *dllname );
void     QGL_Shutdown( void );""",
"""bool QGL_Init( const char *dllname );
void     QGL_Shutdown( void );
#ifdef ID_SW_RENDERER
bool QGL_InitNull( void );		// win_qgl_null.inl: r_swRenderer 2
#endif"""),
("""	// set current context to NULL
	if ( qwglMakeCurrent ) {""",
"""	// set current context to NULL
#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		// no context was made
	} else
#endif
	if ( qwglMakeCurrent ) {"""),
("""void GLimp_SwapBuffers( void ) {
""",
"""void GLimp_SwapBuffers( void ) {
#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		return;		// the software presenter owns the window
	}
#endif
"""),
])
