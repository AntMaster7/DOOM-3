"""Stage 3: the seam. Start-up / shutdown, the back-end dispatch, screenshots, CPU vertex memory."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

patch('renderer/RenderSystem_init.cpp', [
("""	cmdSystem->AddCommand( "reloadARBprograms", R_ReloadARBPrograms_f, CMD_FL_RENDERER, "reloads ARB programs" );
	R_ReloadARBPrograms_f( idCmdArgs() );

	// allocate the vertex array range or vertex objects
	vertexCache.Init();
""", """	cmdSystem->AddCommand( "reloadARBprograms", R_ReloadARBPrograms_f, CMD_FL_RENDERER, "reloads ARB programs" );
	R_ReloadARBPrograms_f( idCmdArgs() );

#ifdef ID_SW_RENDERER
	// before the vertex cache (it must stay in CPU memory) and before the images reload
	// (each one gets its software twin as it is generated)
	SW_Init();
#endif

	// allocate the vertex array range or vertex objects
	vertexCache.Init();
"""),
("""void idRenderSystemLocal::ShutdownOpenGL( void ) {
	// free the context and close the window
	R_ShutdownFrameData();
""", """void idRenderSystemLocal::ShutdownOpenGL( void ) {
#ifdef ID_SW_RENDERER
	SW_Shutdown();
#endif
	// free the context and close the window
	R_ShutdownFrameData();
"""),
# (the source line ends in a blank, which editors strip: anchor on the text before it)
("""			qglReadBuffer( GL_FRONT );
			qglReadPixels( 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, temp );""", """#ifdef ID_SW_RENDERER
			if ( SW_Presenting() ) {
				SW_ReadPixels( 0, 0, w, h, temp );		// the frame itself, not what GL made of it
			} else
#endif
			{
				qglReadBuffer( GL_FRONT );
				qglReadPixels( 0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, temp );
			}"""),
])

patch('renderer/RenderSystem.cpp', [
("""		const double start = Sys_GetClockTicks();
		RB_ExecuteBackEndCommands( frameData->cmdHead );
""", """		const double start = Sys_GetClockTicks();
#ifdef ID_SW_RENDERER
		if ( SW_Presenting() ) {
			SW_ExecuteBackEndCommands( frameData->cmdHead );
		} else
#endif
		RB_ExecuteBackEndCommands( frameData->cmdHead );
"""),
])

patch('renderer/VertexCache.cpp', [
("""	// use ARB_vertex_buffer_object unless explicitly disabled
	if( r_useVertexBuffers.GetInteger() && glConfig.ARBVertexBufferObjectAvailable ) {
		common->Printf( "using ARB_vertex_buffer_object memory\\n" );
	} else {""", """	// use ARB_vertex_buffer_object unless explicitly disabled
	bool useVBO = r_useVertexBuffers.GetInteger() && glConfig.ARBVertexBufferObjectAvailable;
#ifdef ID_SW_RENDERER
	if ( SW_Active() ) {
		useVBO = false;		// the software rasterizer reads the vertices: Position() must be a pointer
	}
#endif
	if( useVBO ) {
		common->Printf( "using ARB_vertex_buffer_object memory\\n" );
	} else {"""),
])
print('done')
