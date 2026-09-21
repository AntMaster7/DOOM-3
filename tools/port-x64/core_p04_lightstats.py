"""Stage 8: how much of the light work is wasted. Lanes the interaction kernel was asked for."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'
patch(SW + 'core/types.h', [
("""    uint64_t    tsc[8];             /* SW_PROF_*: cycles inside the tile phase, by category */
    char        pad[48];""", """    uint64_t    tsc[8];             /* SW_PROF_*: cycles inside the tile phase, by category */
    uint64_t    lightLanes;         /* lanes k_interaction was asked for */
    char        pad[40];"""),
])
patch(SW + 'sw_api.h', [
("""    uint64_t    lightBlocksDark, lightLanesDark; /* k_interaction: blocks / lanes dropped by its exact early-outs */""",
 """    uint64_t    lightBlocksDark, lightLanesDark; /* k_interaction: blocks / lanes dropped by its exact early-outs */
    uint64_t    lightLanes;                     /* lanes k_interaction was asked for (after depth and stencil tests) */"""),
])
patch(SW + 'rast/raster.c', [
("""                    else if (kern == SW_KERN_INTERACTION) cov = k_interaction(""",
 """                    else if (kern == SW_KERN_INTERACTION) st->lightLanes += (unsigned)_mm_popcnt_u32(cov), cov = k_interaction("""),
])
patch(SW + 'sw_frame.c', [
("""        o->lightBlocksDark += s->lightBlocksDark;""", """        o->lightLanes += s->lightLanes;
        o->lightBlocksDark += s->lightBlocksDark;"""),
])

B = SW + 'sw_backend.cpp'
patch(B, [
("""	double	mpxDepth, mpxStencil, mpxColor;		// lanes written, in megapixels""",
 """	double	mpxDepth, mpxStencil, mpxColor;		// lanes written, in megapixels
	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected;"""),
("""	sw_frame.mpxColor += (double)s.pxColor * 1e-6;
}""", """	sw_frame.mpxColor += (double)s.pxColor * 1e-6;
	sw_frame.mpxLightAsked += (double)s.lightLanes * 1e-6;
	sw_frame.mpxLightDark += (double)s.lightLanesDark * 1e-6;
	sw_frame.kLightCellsDark += (double)s.lightCellsDark * 1e-3;
	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;
}"""),
("""	SW_ReportColumn( "MpxColor", offsetof( swFrameStats_t, mpxColor ), frames );""",
 """	SW_ReportColumn( "MpxColor", offsetof( swFrameStats_t, mpxColor ), frames );
	SW_ReportColumn( "MpxLtAsk", offsetof( swFrameStats_t, mpxLightAsked ), frames );
	SW_ReportColumn( "MpxLtDrk", offsetof( swFrameStats_t, mpxLightDark ), frames );
	SW_ReportColumn( "kLtCells", offsetof( swFrameStats_t, kLightCellsDark ), frames );
	SW_ReportColumn( "kZCells", offsetof( swFrameStats_t, kCellsRejected ), frames );"""),
])
print('done')
