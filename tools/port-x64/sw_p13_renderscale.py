# Stage 10: r_swRenderScale. The window keeps its size; glConfig.vidWidth / vidHeight, which is what the
# whole engine renders to, becomes the scaled size, and the GL-free presenter's swap chain (made at
# that size) is stretched to the window by DXGI. Three places set the video size.
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

patch('renderer/RenderSystem_init.cpp', [
("""	// input and sound systems need to be tied to the new window
	Sys_InitInput();""",
"""#ifdef ID_SW_RENDERER
	// the window exists at its full size; from here on the engine renders to the scaled one
	SW_ApplyRenderScale( &glConfig.vidWidth, &glConfig.vidHeight );
#endif

	// input and sound systems need to be tied to the new window
	Sys_InitInput();"""),
("""		parms.stereo = false;
		GLimp_SetScreenParms( parms );
	}""",
"""		parms.stereo = false;
#ifdef ID_SW_RENDERER
		if ( SW_GLFree() ) {
			// glConfig holds the RENDER size (r_swRenderScale); the window wants the mode's
			R_GetModeInfo( &parms.width, &parms.height, r_mode.GetInteger() );
		}
#endif
		GLimp_SetScreenParms( parms );
	}"""),
])

patch('sys/win32/win_wndproc.cpp', [
("""					glConfig.vidWidth = rect.right - rect.left;
					glConfig.vidHeight = rect.bottom - rect.top;""",
"""					glConfig.vidWidth = rect.right - rect.left;
					glConfig.vidHeight = rect.bottom - rect.top;
#ifdef ID_SW_RENDERER
					SW_ApplyRenderScale( &glConfig.vidWidth, &glConfig.vidHeight );
#endif"""),
])
