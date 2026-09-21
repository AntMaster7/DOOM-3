"""Stage 3/4: r_swCompare. R_IssueRenderCommands hands the frame to SW_RunFrame, which decides which
back end(s) run; RB_SwapBuffers can be told to leave the back buffer alone so it can be read."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('renderer/RenderSystem.cpp', [
("""#ifdef ID_SW_RENDERER
		if ( SW_Presenting() ) {
			SW_ExecuteBackEndCommands( frameData->cmdHead );
		} else
#endif
		RB_ExecuteBackEndCommands( frameData->cmdHead );
""", """#ifdef ID_SW_RENDERER
		if ( SW_Active() ) {
			SW_RunFrame( frameData->cmdHead );		// software, GL, or both (r_swCompare)
		} else
#endif
		RB_ExecuteBackEndCommands( frameData->cmdHead );
"""),
])

patch('renderer/tr_backend.cpp', [
("""	// don't flip if drawing to front buffer
	if ( !r_frontBuffer.GetBool() ) {
	    GLimp_SwapBuffers();
	}
}""", """#ifdef ID_SW_RENDERER
	// r_swCompare reads the finished GL frame from the back buffer and presents a composite itself
	if ( SW_SuppressGLSwap() ) {
		return;
	}
#endif

	// don't flip if drawing to front buffer
	if ( !r_frontBuffer.GetBool() ) {
	    GLimp_SwapBuffers();
	}
}"""),
])
print('done')
