"""Stage 8: a shadow segment's stencil counts are read only by the stencil-tested draws that follow it
before the next stencil clear. A tile that holds none of those skips the segment's volumes. Exact."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'
patch(SW + 'core/types.h', [
("""    int         clearSeg;           /* latest segment at or before this one that clears stencil, -1 = none */""",
 """    int         clearSeg;           /* latest segment at or before this one that clears stencil, -1 = none */
    int         testsStencil;       /* a draw of this segment reads the stencil count */
    /* SHADOW segments: the job ranges [lo, hi) of the segments that read what this one counts (those
       after it, up to the next stencil clear, that test the stencil). DOOM 3's pattern needs two
       (local interactions, global interactions); more fall back to "always needed". */
    int         numConsumers;
    int         consumerLo[4], consumerHi[4];"""),
("""static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0, 1 };""",
 """static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0, 1, 1 };"""),
])
patch(SW + 'sw_api.h', [
("""    SW_OPT_DEPTH_BOUNDS,    /* honour SwDraw.depthBoundsMin/Max; default 1 */""",
 """    SW_OPT_DEPTH_BOUNDS,    /* honour SwDraw.depthBoundsMin/Max; default 1 */
    SW_OPT_SHADOW_CULL,     /* a tile skips the shadow volumes whose counts nothing in it reads; default 1 */"""),
("""    uint64_t    cellsFast;                      /* 16x16 cells settled without a per-block walk */""",
 """    uint64_t    cellsFast;                      /* 16x16 cells settled without a per-block walk */
    uint64_t    shadowBinsCulled, shadowBinsDrawn; /* SW_OPT_SHADOW_CULL: (shadow job, tile) bins skipped / replayed */"""),
])
patch(SW + 'core/types.h', [
("""    uint64_t    lightCalls;         /* ... in this many calls */
    char        pad[32];""", """    uint64_t    lightCalls;         /* ... in this many calls */
    uint64_t    shadowBinsCulled, shadowBinsDrawn;
    char        pad[16];"""),
])
patch(SW + 'sw_frame.c', [
("""    s->clearSeg = s->hasClear ? g_numSegs : (g_numSegs ? g_segs[g_numSegs - 1].clearSeg : -1);""",
 """    s->clearSeg = s->hasClear ? g_numSegs : (g_numSegs ? g_segs[g_numSegs - 1].clearSeg : -1);
    s->testsStencil = 0; s->numConsumers = 0;"""),
("""    g_draws[g_numDraws] = *draw;""", """    g_draws[g_numDraws] = *draw;
    if (draw->op == SW_OP_COLOR && draw->stencilTest) g_segs[g_numSegs - 1].testsStencil = 1;"""),
("""    const double tx = now_ms();""", """    /* who reads each shadow segment's counts: the stencil-testing segments up to the next clear */
    for (int si = 0; si < g_numSegs; si++) {
        SwSegment *s = &g_segs[si];
        if (s->kind != SW_SEG_SHADOW) continue;
        s->numConsumers = 0;
        for (int k = si + 1; k < g_numSegs && !g_segs[k].hasClear; k++) {
            const SwSegment *c = &g_segs[k];
            if (!c->testsStencil || !c->numJobs) continue;
            if (s->numConsumers == 4) { s->numConsumers = -1; break; }      /* too many: always needed */
            s->consumerLo[s->numConsumers] = c->firstJob; s->consumerHi[s->numConsumers] = c->firstJob + c->numJobs;
            s->numConsumers++;
        }
    }

    const double tx = now_ms();"""),
("""        o->lightLanes += s->lightLanes; o->lightCalls += s->lightCalls;""",
 """        o->lightLanes += s->lightLanes; o->lightCalls += s->lightCalls;
        o->shadowBinsCulled += s->shadowBinsCulled; o->shadowBinsDrawn += s->shadowBinsDrawn;"""),
])
patch(SW + 'rast/tile.c', [
("""/* PH_TILES: one tile */""", """/* any job bit of this tile in [lo, hi)? */
static __forceinline int tile_has_jobs(const volatile LONG64 *bits, int lo, int hi)
{
    if (lo >= hi) return 0;
    const int wLo = lo >> 6, wHi = (hi - 1) >> 6;
    for (int w = wLo; w <= wHi; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        if (w == wLo) word &= ~0ull << (lo & 63);
        if (w == wHi && ((hi & 63) != 0)) word &= ~(~0ull << (hi & 63));
        if (word) return 1;
    }
    return 0;
}

/* PH_TILES: one tile */"""),
("""    int curSeg = -1, lastClear = -1;""", """    int curSeg = -1, lastClear = -1, skipSeg = 0;
    const int optShadowCull = g_opt[SW_OPT_SHADOW_CULL];"""),
("""                curSeg = job->seg;
            }""", """                curSeg = job->seg;
                /* Shadow volumes count into the stencil for the stencil-tested draws that follow, up to
                   the next clear. If this tile holds none of those, nobody reads the counts: skip. Exact.
                   (The job range of this phase does not matter: a consumer in a later phase has its bit set too.) */
                skipSeg = 0;
                if (seg->kind == SW_SEG_SHADOW && optShadowCull && seg->numConsumers >= 0) {
                    skipSeg = 1;
                    for (int k = 0; k < seg->numConsumers; k++)
                        if (tile_has_jobs(bits, seg->consumerLo[k], seg->consumerHi[k])) { skipSeg = 0; break; }
                }
            }
            if (g_segs[job->seg].kind == SW_SEG_SHADOW) {
                if (skipSeg) { c.st->shadowBinsCulled++; continue; }
                c.st->shadowBinsDrawn++;
            }"""),
])
patch(SW + 'sw_backend.cpp', [
("""idCVar r_swShowStats(""", """idCVar r_swShadowCull( "r_swShadowCull", "1", CVAR_RENDERER | CVAR_BOOL, "a tile skips the shadow volumes of a light that draws no stencil-tested surface in it" );
idCVar r_swShowStats("""),
("""	sw_set_option( SW_OPT_DEPTH_BOUNDS, r_swDepthBounds.GetBool() );""",
 """	sw_set_option( SW_OPT_DEPTH_BOUNDS, r_swDepthBounds.GetBool() );
	sw_set_option( SW_OPT_SHADOW_CULL, r_swShadowCull.GetBool() );"""),
("""	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected, lightLanesPerCall;""",
 """	double	mpxLightAsked, mpxLightDark, kLightCellsDark, kCellsRejected, lightLanesPerCall;
	double	kShadowBinsCulled, kShadowBinsDrawn;"""),
("""	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;""",
 """	sw_frame.kCellsRejected += (double)s.cellsRejected * 1e-3;
	sw_frame.kShadowBinsCulled += (double)s.shadowBinsCulled * 1e-3;
	sw_frame.kShadowBinsDrawn += (double)s.shadowBinsDrawn * 1e-3;"""),
("""	SW_ReportColumn( "LtLanes", offsetof( swFrameStats_t, lightLanesPerCall ), frames );
""", """	SW_ReportColumn( "LtLanes", offsetof( swFrameStats_t, lightLanesPerCall ), frames );
	SW_ReportColumn( "kShCull", offsetof( swFrameStats_t, kShadowBinsCulled ), frames );
	SW_ReportColumn( "kShDrawn", offsetof( swFrameStats_t, kShadowBinsDrawn ), frames );
"""),
])
print('done')
