"""Stage 8 glue: shadow draws carry the depth bounds of their scissor rectangle; r_swDepthBounds is the A/B switch."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'
patch(SW + 'sw_submit.cpp', [
("""		d.depthRangeMax = ( surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) ? 0.5f : 1.0f;
		d.depthTest = SW_DEPTH_LEQUAL;
		sw_draw( &d );""", """		d.depthRangeMax = ( surf->space->weaponDepthHack && surf->space->modelDepthHack == 0.0f ) ? 0.5f : 1.0f;
		d.depthTest = SW_DEPTH_LEQUAL;
		// qglDepthBoundsEXT( surf->scissorRect.zmin, surf->scissorRect.zmax ): pixels whose stored depth is
		// outside the light's range keep their count. They cannot be lit by this light, so the picture does
		// not change; the rasterizer skips whole cells and tiles with it.
		if ( r_useDepthBoundsTest.GetBool() ) {
			d.depthBoundsMin = surf->scissorRect.zmin;
			d.depthBoundsMax = surf->scissorRect.zmax;
		}
		sw_draw( &d );"""),
])
patch(SW + 'sw_backend.cpp', [
("""idCVar r_swShowStats(""", """idCVar r_swDepthBounds( "r_swDepthBounds", "1", CVAR_RENDERER | CVAR_BOOL, "shadow volumes honour the light's depth bounds (r_useDepthBoundsTest) and skip cells and tiles outside them" );
idCVar r_swShowStats("""),
("""	sw_set_option( SW_OPT_PROFILE, r_swProfile.GetBool() );""", """	sw_set_option( SW_OPT_PROFILE, r_swProfile.GetBool() );
	sw_set_option( SW_OPT_DEPTH_BOUNDS, r_swDepthBounds.GetBool() );"""),
])
print('done')
