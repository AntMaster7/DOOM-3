# Stage 9: the load-time OpenGL calls that r_swRenderer 2 still made (the first stray-call report:
# glTexImage2D 9574, glTexParameterf 5675, glGetError 1490, glBindTexture 1138, glDeleteTextures 1138,
# glProgramStringARB 20, ...). Each gate also saves work: the second mip chain that was built only
# to be handed to a no-op.
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

patch('renderer/Image_load.cpp', [
("""		SW_ImageGenerate2D( this, scaledBuffer, scaled_width, scaled_height, preserveBorder );
	}
#endif
""",
"""		SW_ImageGenerate2D( this, scaledBuffer, scaled_width, scaled_height, preserveBorder );
	}
	if ( SW_GLFree() ) {
		// the twin IS the image; texnum (from qglGenTextures' stand-in) says "loaded"
		if ( scaledBuffer != 0 ) {
			R_StaticFree( scaledBuffer );
		}
		return;
	}
#endif
"""),
("""		SW_ImageGenerateCube( this, pic, size );
	}
#endif
""",
"""		SW_ImageGenerateCube( this, pic, size );
	}
	if ( SW_GLFree() ) {
		return;
	}
#endif
"""),
("""		SW_ImageUploadScratch( this, data, cols, rows );
	}
#endif
""",
"""		SW_ImageUploadScratch( this, data, cols, rows );
	}
	if ( SW_GLFree() ) {
		return;
	}
#endif
"""),
("""		if ( qglDeleteTextures ) {
			qglDeleteTextures( 1, &texnum );	// this should be the ONLY place it is ever called!
		}""",
"""#ifdef ID_SW_RENDERER
		if ( SW_GLFree() ) {
			// nothing was created
		} else
#endif
		if ( qglDeleteTextures ) {
			qglDeleteTextures( 1, &texnum );	// this should be the ONLY place it is ever called!
		}"""),
])

patch('renderer/draw_arb2.cpp', [
("""		progs[progIndex].ident = PROG_USER + progIndex;
	}
""",
"""		progs[progIndex].ident = PROG_USER + progIndex;
	}
#ifdef ID_SW_RENDERER
	if ( SW_GLFree() ) {
		// the programs are kernels here, found by file name through the identifier just given
		// (R_ARBProgramName); the file was still read, so fs_copyfiles keeps working
		common->Printf( "\\n" );
		return;
	}
#endif
"""),
])
