"""Stage 8: how full are the blocks the light kernel is called for? (lanes per call decides whether lane compaction pays)"""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'
patch(SW + 'core/types.h', [
("""    uint64_t    lightLanes;         /* lanes k_interaction was asked for */
    char        pad[40];""", """    uint64_t    lightLanes;         /* lanes k_interaction was asked for */
    uint64_t    lightCalls;         /* ... in this many calls */
    char        pad[32];"""),
])
patch(SW + 'sw_api.h', [
("""    uint64_t    lightLanes;                     /* lanes k_interaction was asked for (after depth and stencil tests) */""",
 """    uint64_t    lightLanes;                     /* lanes k_interaction was asked for (after depth and stencil tests) */
    uint64_t    lightCalls;                     /* ... in this many calls: lanes / calls of 16 is how full a block is */"""),
])
patch(SW + 'rast/raster.c', [
("""st->lightLanes += (unsigned)_mm_popcnt_u32(cov), cov = k_interaction(""",
 """st->lightCalls++, st->lightLanes += (unsigned)_mm_popcnt_u32(cov), cov = k_interaction("""),
])
patch(SW + 'sw_frame.c', [
("""        o->lightLanes += s->lightLanes;""", """        o->lightLanes += s->lightLanes; o->lightCalls += s->lightCalls;"""),
])
patch(SW + 'sw_backend.cpp', [
("""	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected;""",
 """	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected, lightLanesPerCall;"""),
("""	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;""",
 """	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;
	if ( s.lightCalls ) {
		sw_frame.lightLanesPerCall = (double)s.lightLanes / (double)s.lightCalls;		// of the frame's last 3D view
	}"""),
("""	SW_ReportColumn( "kZCells", offsetof( swFrameStats_t, kCellsRejected ), frames );""",
 """	SW_ReportColumn( "kZCells", offsetof( swFrameStats_t, kCellsRejected ), frames );
	SW_ReportColumn( "LtLanes", offsetof( swFrameStats_t, lightLanesPerCall ), frames );"""),
])
print('done')
