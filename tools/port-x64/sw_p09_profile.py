"""Stage 8: r_swProfile and the per-category columns of the swstats report."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

B = 'C:/Source/DOOM-3/neo/renderer/sw/sw_backend.cpp'
patch(B, [
("""idCVar r_swShowStats(""", """idCVar r_swProfile( "r_swProfile", "0", CVAR_RENDERER | CVAR_BOOL, "time the tile phase by category (load, depth, zrange, shadow, light, colour, store): thread-summed ms / threads, in swReportStats" );
idCVar r_swShowStats("""),
("""	double	equalFailures, equalInFront, pxColor, pxStencil, pxDepth;
};""", """	double	equalFailures, equalInFront, pxColor, pxStencil, pxDepth;
	double	tileMs[SW_PROF_COUNT];
	double	mpxDepth, mpxStencil, mpxColor;		// lanes written, in megapixels
};"""),
("""	sw_frame.pxDepth += (double)s.pxDepth;
}""", """	sw_frame.pxDepth += (double)s.pxDepth;
	for ( int k = 0; k < SW_PROF_COUNT; k++ ) {
		sw_frame.tileMs[k] += s.tileMs[k];
	}
	sw_frame.mpxDepth += (double)s.pxDepth * 1e-6;
	sw_frame.mpxStencil += (double)s.pxStencil * 1e-6;
	sw_frame.mpxColor += (double)s.pxColor * 1e-6;
}"""),
("""	sw_debug_count_equal_failures( r_swDebugEqual.GetBool() );""", """	sw_debug_count_equal_failures( r_swDebugEqual.GetBool() );
	sw_set_option( SW_OPT_PROFILE, r_swProfile.GetBool() );"""),
("""	SW_ReportColumn( "present", offsetof( swFrameStats_t, presentMs ), frames );
}""", """	SW_ReportColumn( "present", offsetof( swFrameStats_t, presentMs ), frames );
	if ( r_swProfile.GetBool() ) {
		static const char *names[SW_PROF_COUNT] = { "t.load", "t.depth", "t.zrange", "t.shadow", "t.light", "t.colour", "t.store" };
		for ( int k = 0; k < SW_PROF_COUNT; k++ ) {
			SW_ReportColumn( names[k], offsetof( swFrameStats_t, tileMs ) + k * sizeof( double ), frames );
		}
	}
	SW_ReportColumn( "MpxDepth", offsetof( swFrameStats_t, mpxDepth ), frames );
	SW_ReportColumn( "MpxStenc", offsetof( swFrameStats_t, mpxStencil ), frames );
	SW_ReportColumn( "MpxColor", offsetof( swFrameStats_t, mpxColor ), frames );
}"""),
])
print('done')
