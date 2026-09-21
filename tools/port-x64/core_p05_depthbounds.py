"""Stage 8: the depth bounds test of RB_T_Shadow (GL_EXT_depth_bounds_test), exact per pixel, and
used per tile cell to skip shadow volume work where the stored depth lies outside the light's range."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'

patch(SW + 'sw_api.h', [
("""    float           depthRangeMax;  /* glDepthRange(0, this); 0 means 1 */
""", """    float           depthRangeMax;  /* glDepthRange(0, this); 0 means 1 */
    /* the stencil ops: GL_EXT_depth_bounds_test. A pixel whose STORED depth lies outside
       [min, max] is left alone. max <= min means off. */
    float           depthBoundsMin, depthBoundsMax;
"""),
("""    SW_OPT_PROFILE,         /* time the tile phase by category (two rdtsc per bin); default 0 */""",
 """    SW_OPT_PROFILE,         /* time the tile phase by category (two rdtsc per bin); default 0 */
    SW_OPT_DEPTH_BOUNDS,    /* honour SwDraw.depthBoundsMin/Max; default 1 */"""),
])

patch(SW + 'core/types.h', [
("""static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0 };""",
 """static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0, 1 };"""),
])

patch(SW + 'rast/raster.c', [
("""    int             optProfile;""", """    int             optProfile, optDepthBounds;
    /* the depth bounds of the stencil draw being replayed (job_tile sets them per bin) */
    int             hasBounds;
    float           boundsMin, boundsMax;
    unsigned        boundsCells;        /* cells whose stored z range reaches into the bounds */
    unsigned        boundsFull;         /* cells whose stored z range lies inside them: no per-pixel test needed for the fast path */"""),
("""        const __mmask16 cells = (__mmask16)rect_cells(minx, miny, maxx, maxy, tx0, ty0);
        if (op == SW_OP_STENCIL_ZFAIL) {""", """        const __mmask16 cells = (__mmask16)rect_cells(minx, miny, maxx, maxy, tx0, ty0);
        if ((op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL) && c->hasBounds && !(cells & c->boundsCells)) {
            st->triTileRejected++; return;                  /* every touched cell is outside the depth bounds */
        }
        if (op == SW_OP_STENCIL_ZFAIL) {"""),
("""                if (zr) {
                    /* the plane's extremes over the cell's pixel centres sit at its corners */""",
 """                const int stencilOp = op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL;
                if (stencilOp && zr && c->hasBounds && !((c->boundsCells >> ci) & 1)) { cellSkip |= bit; st->cellsRejected++; continue; }
                const int cellInBounds = !stencilOp || !c->hasBounds || ((c->boundsFull >> ci) & 1);
                if (zr) {
                    /* the plane's extremes over the cell's pixel centres sit at its corners */"""),
("""                        if (full && inWalk && zhi <= czmin && c->optCellFast) {                  /* passes everywhere */""",
 """                        if (full && inWalk && zhi <= czmin && c->optCellFast && cellInBounds) {  /* passes everywhere */"""),
("""                        if (full && inWalk && zlo > czmax && c->optCellFast) {                   /* fails everywhere */""",
 """                        if (full && inWalk && zlo > czmax && c->optCellFast && cellInBounds) {   /* fails everywhere */"""),
("""                const __mmask16 cov = _mm512_mask_cmp_ps_mask(mk, zs, _mm512_load_ps(tz + blk),
                                                              op == SW_OP_STENCIL_ZPASS ? _CMP_LE_OQ : _CMP_NLE_UQ);
                if (!cov) continue;""", """                const VF stored = _mm512_load_ps(tz + blk);
                __mmask16 cov = _mm512_mask_cmp_ps_mask(mk, zs, stored, op == SW_OP_STENCIL_ZPASS ? _CMP_LE_OQ : _CMP_NLE_UQ);
                if (hasBounds) {                            /* GL_EXT_depth_bounds_test: on the STORED depth, inclusive */
                    cov = _mm512_mask_cmp_ps_mask(cov, stored, vBoundsMin, _CMP_GE_OQ);
                    cov = _mm512_mask_cmp_ps_mask(cov, stored, vBoundsMax, _CMP_LE_OQ);
                }
                if (!cov) continue;"""),
("""    const int fillColor = d->writeMask != SW_WRITE_NONE;   /* a subview's depth fill leaves the colour alone */""",
 """    const int fillColor = d->writeMask != SW_WRITE_NONE;   /* a subview's depth fill leaves the colour alone */
    const int hasBounds = c->hasBounds;
    const VF vBoundsMin = _mm512_set1_ps(c->boundsMin), vBoundsMax = _mm512_set1_ps(c->boundsMax);"""),
])

patch(SW + 'rast/tile.c', [
("""    c.optProfile = g_opt[SW_OPT_PROFILE];""", """    c.optProfile = g_opt[SW_OPT_PROFILE]; c.optDepthBounds = g_opt[SW_OPT_DEPTH_BOUNDS];
    c.hasBounds = 0; c.boundsMin = 0.0f; c.boundsMax = 1.0f; c.boundsCells = 0xFFFFu; c.boundsFull = 0;"""),
("""            const SwDraw *d = &g_draws[job->draw];
            raster_bin(&g_bins[(size_t)j * g_ntiles + tile], d, &c);""", """            const SwDraw *d = &g_draws[job->draw];
            c.hasBounds = 0;
            if ((d->op == SW_OP_STENCIL_ZPASS || d->op == SW_OP_STENCIL_ZFAIL) && c.optDepthBounds &&
                d->depthBoundsMax > d->depthBoundsMin && (d->depthBoundsMin > 0.0f || d->depthBoundsMax < 1.0f)) {
                c.hasBounds = 1; c.boundsMin = d->depthBoundsMin; c.boundsMax = d->depthBoundsMax;
                c.boundsCells = 0xFFFFu; c.boundsFull = 0;
                if (c.zrange) {
                    const VF zmn = _mm512_load_ps(c.cellZmin), zmx = _mm512_load_ps(c.cellZmax);
                    const VF bmn = _mm512_set1_ps(c.boundsMin), bmx = _mm512_set1_ps(c.boundsMax);
                    c.boundsCells = (unsigned)(_mm512_cmp_ps_mask(zmx, bmn, _CMP_GE_OQ) & _mm512_cmp_ps_mask(zmn, bmx, _CMP_LE_OQ));
                    c.boundsFull = (unsigned)(_mm512_cmp_ps_mask(zmn, bmn, _CMP_GE_OQ) & _mm512_cmp_ps_mask(zmx, bmx, _CMP_LE_OQ));
                    if (!c.boundsCells) continue;           /* the whole tile is outside the light's depth range */
                }
            }
            raster_bin(&g_bins[(size_t)j * g_ntiles + tile], d, &c);"""),
])
print('done')
