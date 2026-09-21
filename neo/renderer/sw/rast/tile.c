/* rast/tile.c -- job_tile: one 64x64 tile replays the view's segments, in submission order,
   into tile-local depth, stencil and colour, then streams its colour out once.
   Skeleton from CRenderer's frame/tile.c (tile-local buffers, bin walk through per-tile job
   bits, one streamed store). DOOM 3 blends many passes per pixel, so colour is swizzled like
   depth: a block is one aligned 64-byte load and one store. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

/* I6: every pixel of a tile is defined before it is stored. Views draw over each other (the 3D
   view over a mirror's, the HUD over the 3D view), so the tile starts from the framebuffer. */
static void tile_load_color(uint32_t *tc, int tx0, int ty0)
{
    const uint32_t *src = g_fb + (size_t)ty0 * g_fbPitch + tx0;
    const size_t p = (size_t)g_fbPitch;
    for (int by = 0; by < SPAN_BY; by++, src += 4 * p)
        for (int bx = 0; bx < SPAN_BX; bx++) {
            const uint32_t *s = src + bx * 4;
            VI v = _mm512_castsi128_si512(_mm_loadu_si128((const __m128i *)s));
            v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i *)(s + p)), 1);
            v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i *)(s + 2 * p)), 2);
            v = _mm512_inserti32x4(v, _mm_loadu_si128((const __m128i *)(s + 3 * p)), 3);
            _mm512_store_si512((void *)(tc + SPAN_OFF(bx, by)), v);
        }
}

/* De-swizzle and stream: whole 64-byte-aligned rows leave the tile without a read-for-ownership
   (CRenderer: a plain store turned +2.2% into -5.5%). The framebuffer is padded to whole tiles
   and its pitch is a multiple of 16 pixels, so there are no edge cases. */
static void tile_store_color(const uint32_t *tc, int tx0, int ty0)
{
    uint32_t *dst = g_fb + (size_t)ty0 * g_fbPitch + tx0;
    const size_t p = (size_t)g_fbPitch;
    for (int by = 0; by < SPAN_BY; by++, dst += 4 * p)
        for (int q = 0; q < SPAN_BX; q += 4) {              /* four blocks = 16 pixels of each of 4 rows */
            const VI b0 = _mm512_load_si512((const void *)(tc + SPAN_OFF(q, by)));
            const VI b1 = _mm512_load_si512((const void *)(tc + SPAN_OFF(q + 1, by)));
            const VI b2 = _mm512_load_si512((const void *)(tc + SPAN_OFF(q + 2, by)));
            const VI b3 = _mm512_load_si512((const void *)(tc + SPAN_OFF(q + 3, by)));
/* the extract index must be a literal */
#define STORE_ROW(r) do {                                                                       \
                VI row = _mm512_castsi128_si512(_mm512_extracti32x4_epi32(b0, r));                  \
                row = _mm512_inserti32x4(row, _mm512_extracti32x4_epi32(b1, r), 1);                 \
                row = _mm512_inserti32x4(row, _mm512_extracti32x4_epi32(b2, r), 2);                 \
                row = _mm512_inserti32x4(row, _mm512_extracti32x4_epi32(b3, r), 3);                 \
                _mm512_stream_si512((__m512i *)(dst + (size_t)(r) * p + q * 4), row);               \
            } while (0)
            STORE_ROW(0); STORE_ROW(1); STORE_ROW(2); STORE_ROW(3);
#undef STORE_ROW
        }
    _mm_sfence();
}

/* debug capture: scalar, unswizzle depth and stencil into the full-frame buffers */
static void tile_debug_capture(const float *tz, const uint8_t *ts, int tx0, int ty0)
{
    for (int y = 0; y < TILE_SIZE && ty0 + y < g_height; y++)
        for (int x = 0; x < TILE_SIZE && tx0 + x < g_width; x++) {
            const size_t o = SPAN_OFF(x >> 2, y >> 2) + (size_t)((y & 3) * 4 + (x & 3));
            const size_t f = (size_t)(ty0 + y) * g_width + (tx0 + x);
            if (g_dbgDepth) g_dbgDepth[f] = tz[o];
            if (g_dbgStencil) g_dbgStencil[f] = ts[o];
        }
}

/* the per-light stencil clear: the count back to 128 inside (rect ∩ tile) */
static void tile_clear_stencil(uint8_t *ts, const SwRect *r, int tx0, int ty0)
{
    const int minx = r->x0 > tx0 ? r->x0 : tx0, maxx = r->x1 < tx0 + TILE_SIZE - 1 ? r->x1 : tx0 + TILE_SIZE - 1;
    const int miny = r->y0 > ty0 ? r->y0 : ty0, maxy = r->y1 < ty0 + TILE_SIZE - 1 ? r->y1 : ty0 + TILE_SIZE - 1;
    if (minx > maxx || miny > maxy) return;
    if (minx == tx0 && miny == ty0 && maxx == tx0 + TILE_SIZE - 1 && maxy == ty0 + TILE_SIZE - 1) {
        memset(ts, 128, TILE_PIXELS);
        return;
    }
    const int bx0 = (minx - tx0) / SPAN_W, bx1 = (maxx - tx0) / SPAN_W;
    const int by0 = (miny - ty0) / SPAN_H, by1 = (maxy - ty0) / SPAN_H;
    const unsigned xmFirst = SPAN_XREP(SPAN_RUN(minx - (tx0 + bx0 * SPAN_W), SPAN_W - 1));
    const unsigned xmLast  = SPAN_XREP(SPAN_RUN(0, maxx - (tx0 + bx1 * SPAN_W)));
    const __m128i v = _mm_set1_epi8((char)128);
    for (int by = by0; by <= by1; by++) {
        const int py0 = ty0 + by * SPAN_H;
        const int loy = miny > py0 ? miny - py0 : 0;
        const int hiy = maxy < py0 + SPAN_H - 1 ? maxy - py0 : SPAN_H - 1;
        const unsigned ym = SPAN_YEXP[SPAN_RUN(loy, hiy)];
        for (int bx = bx0; bx <= bx1; bx++) {
            unsigned xm = ym;
            if (bx == bx0) xm &= xmFirst;
            if (bx == bx1) xm &= xmLast;
            _mm_mask_storeu_epi8(ts + SPAN_OFF(bx, by), (__mmask16)xm, v);
        }
    }
}

/* per-cell depth range, once depth is final (I4): what the z-range rejects read */
static void tile_build_zrange(TileCtx *c)
{
    for (int cy = 0; cy < CELL_DIM; cy++)
        for (int cx = 0; cx < CELL_DIM; cx++) {
            VF mn = _mm512_load_ps(c->tz + SPAN_OFF(cx * 4, cy * 4)), mx = mn;
            for (int dy = 0; dy < 4; dy++)
                for (int dx = 0; dx < 4; dx++) {
                    const VF v = _mm512_load_ps(c->tz + SPAN_OFF(cx * 4 + dx, cy * 4 + dy));
                    mn = _mm512_min_ps(mn, v); mx = _mm512_max_ps(mx, v);
                }
            c->cellZmin[cy * CELL_DIM + cx] = vmin16(mn);
            c->cellZmax[cy * CELL_DIM + cx] = vmax16(mx);
        }
    c->zrange = 1;
}

/* One bin with one draw state. Every (op, depth test, kernel) combination is its own copy of
   raster_tri IN ITS OWN FUNCTION. Inlined into one dispatcher they made a single 33,000-line
   function, and MSVC's optimizer degrades on those: a few extra branches in k_interaction doubled
   the cost of every light pass (harness, 2026-09-21: 8 local lights at 4K, 11.0 -> 24.6 ms) while
   the stage kernel, compiled into the same function, did not move. */
#ifndef SW_BIN_INLINE
#define SW_BIN_INLINE 0
#endif
#if SW_BIN_INLINE
#define BIN_LINKAGE __forceinline
#else
#define BIN_LINKAGE __declspec(noinline)
#endif
#define BIN_FN(name, op, dt, kn)                                                                        \
    static BIN_LINKAGE void name(const int *items, int n, const SwDraw *d, TileCtx *c, int fl) \
    { for (int i = 0; i < n; i++) raster_tri(&g_tris[items[i]], items[i], d, c, op, dt, kn, fl); }
BIN_FN(bin_depth_flat,   SW_OP_DEPTH_FILL,    SW_DEPTH_LEQUAL, SW_KERN_FLAT)
BIN_FN(bin_depth_stage,  SW_OP_DEPTH_FILL,    SW_DEPTH_LEQUAL, SW_KERN_STAGE)
BIN_FN(bin_zpass,        SW_OP_STENCIL_ZPASS, SW_DEPTH_LEQUAL, SW_KERN_FLAT)
BIN_FN(bin_zfail,        SW_OP_STENCIL_ZFAIL, SW_DEPTH_LEQUAL, SW_KERN_FLAT)
BIN_FN(bin_light_eq,     SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_INTERACTION)
BIN_FN(bin_light_le,     SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_INTERACTION)
BIN_FN(bin_light_al,     SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_INTERACTION)
BIN_FN(bin_stage_eq,     SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_STAGE)
BIN_FN(bin_stage_le,     SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_STAGE)
BIN_FN(bin_stage_al,     SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_STAGE)
BIN_FN(bin_flat_eq,      SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_FLAT)
BIN_FN(bin_flat_le,      SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_FLAT)
BIN_FN(bin_flat_al,      SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_FLAT)
/* the rare kernels. (A run-time depth test in ONE copy per kernel was an internal compiler error
   in raster_tri, toolset 14.51: every copy keeps its three constants.) */
BIN_FN(bin_dual_eq,      SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_DUAL)
BIN_FN(bin_dual_le,      SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_DUAL)
BIN_FN(bin_dual_al,      SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_DUAL)
BIN_FN(bin_env_eq,       SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_ENV)
BIN_FN(bin_env_le,       SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_ENV)
BIN_FN(bin_env_al,       SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_ENV)
BIN_FN(bin_screen_eq,    SW_OP_COLOR, SW_DEPTH_EQUAL,  SW_KERN_SCREEN)
BIN_FN(bin_screen_le,    SW_OP_COLOR, SW_DEPTH_LEQUAL, SW_KERN_SCREEN)
BIN_FN(bin_screen_al,    SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_SCREEN)
#undef BIN_FN

static void raster_bin(const SwBin *b, const SwDraw *d, TileCtx *c)
{
    const int *items = b->items;
    const int n = b->count;
    switch (d->op) {
    case SW_OP_DEPTH_FILL:
        if (d->kernel == SW_KERN_STAGE) bin_depth_stage(items, n, d, c, 0); else bin_depth_flat(items, n, d, c, 0);
        break;
    case SW_OP_STENCIL_ZPASS: bin_zpass(items, n, d, c, 0); break;
    case SW_OP_STENCIL_ZFAIL: bin_zfail(items, n, d, c, 0); break;
    default: {
        const int dt = d->depthTest;
        if (d->kernel == SW_KERN_INTERACTION) {
            const int fl = interaction_flags((const SwInteractionParms *)d->kernelParms);
            if (dt == SW_DEPTH_EQUAL) bin_light_eq(items, n, d, c, fl); else if (dt == SW_DEPTH_LEQUAL) bin_light_le(items, n, d, c, fl); else bin_light_al(items, n, d, c, fl);
        } else if (d->kernel == SW_KERN_STAGE) {
            if (dt == SW_DEPTH_EQUAL) bin_stage_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_stage_le(items, n, d, c, 0); else bin_stage_al(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_DUAL) {
            if (dt == SW_DEPTH_EQUAL) bin_dual_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_dual_le(items, n, d, c, 0); else bin_dual_al(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_ENV) {
            if (dt == SW_DEPTH_EQUAL) bin_env_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_env_le(items, n, d, c, 0); else bin_env_al(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_SCREEN) {
            if (dt == SW_DEPTH_EQUAL) bin_screen_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_screen_le(items, n, d, c, 0); else bin_screen_al(items, n, d, c, 0);
        } else {
            if (dt == SW_DEPTH_EQUAL) bin_flat_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_flat_le(items, n, d, c, 0); else bin_flat_al(items, n, d, c, 0);
        }
        break; }
    }
}

/* ----------------------------------------------------------------------------
   Tile order: costliest first. Workers claim tiles off one atomic counter, so a heavy tile
   claimed LAST is the phase's tail: every other thread idles while it finishes. Sorting by an
   estimate moves the heavy tiles to the front, where the cheap ones fill in behind them.

   sw_bin_cost() is the estimate. Decided 2026-09-21 (the user handed the decision over):
   a bin's triangle count weighted by what its op costs per 4x4 block, from the harness on the
   9950X: a light pass about 110 ns lit and 35 ns dark, a textured stage 20 - 35 ns, stencil and
   depth about 10 ns. It reads nothing but what is already in cache: the triangles themselves
   (areas, bboxes) would cost misses, and a count cannot see pixels either way, so the estimate
   stays crude on purpose. The keys are computed IN THE POOL (one job per tile); only the sort of
   a few thousand keys is serial. Serial, it cost 0.2 - 0.4 ms at 4K with 45k triangles.
   To be re-judged on real frames (Stage 3+), where tiles differ; in the synthetic benches every
   tile holds the same bins. The order never changes a tile's output.
---------------------------------------------------------------------------- */
static __forceinline unsigned sw_bin_cost(const SwDraw *d, int binCount)
{
    unsigned w = 1;
    if (d->op == SW_OP_COLOR) w = d->kernel == SW_KERN_INTERACTION ? 8 : (d->kernel == SW_KERN_FLAT ? 1 : (d->kernel == SW_KERN_STAGE ? 3 : 5));
    else if (d->op == SW_OP_DEPTH_FILL && d->kernel == SW_KERN_STAGE) w = 3;
    return w * (unsigned)binCount;
}

/* one tile's sort key */
static void tile_cost(int tile)
{
    const volatile LONG64 *bits = &g_tileJobBits[(size_t)tile * g_jobStride];
    uint64_t cost = 0;
    for (int w = 0; w < g_jobWords; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        while (word) {
            unsigned long bi;
            _BitScanForward64(&bi, word);
            word &= word - 1;
            const int j = w * 64 + (int)bi;
            cost += sw_bin_cost(&g_draws[g_jobs[j].draw], g_bins[(size_t)j * g_ntiles + tile].count);
        }
    }
    if (cost > 0xFFFFFFFFull) cost = 0xFFFFFFFFull;
    g_tileSortKeys[tile] = (cost << 32) | (uint64_t)(0xFFFFFFFFu - (unsigned)tile);   /* ties: lower tile first */
}

/* PH_TILECOST: 64 tiles per job. One tile per job was 2,040 jobs of a microsecond at 4K, and the
   pool's job counter ping-ponged between 32 cores: no faster than doing it serially (0.2 ms). */
#define TILECOST_CHUNK 64
static void job_tile_cost(int chunk, int tid)
{
    (void)tid;
    const int first = chunk * TILECOST_CHUNK, last = first + TILECOST_CHUNK < g_ntiles ? first + TILECOST_CHUNK : g_ntiles;
    for (int tile = first; tile < last; tile++) tile_cost(tile);
}

/* Costliest first needs a coarse order only, so: a counting sort on a logarithmic 8-bit key
   (exponent and the next three bits of the cost), one linear pass, stable in tile order.
   qsort with its comparison callback took 0.17 ms for 4K's 2,040 tiles; this is a few
   microseconds. The serial section between the phases is the most expensive time there is. */
static __forceinline unsigned cost_bucket(uint32_t cost)
{
    if (cost < 16) return cost;                             /* 0 = nothing binned */
    unsigned long e;
    _BitScanReverse(&e, cost);                              /* e >= 4 */
    return (unsigned)((e - 3) << 3 | ((cost >> (e - 3)) & 7)) + 8;   /* 16.. maps to 16..: monotonic */
}
static void tiles_sort_by_cost(void)
{
    dispatch(job_tile_cost, (g_ntiles + TILECOST_CHUNK - 1) / TILECOST_CHUNK);
    int count[256] = { 0 }, first[256];
    for (int t = 0; t < g_ntiles; t++) count[cost_bucket((uint32_t)(g_tileSortKeys[t] >> 32))]++;
    int n = 0;
    for (int b = 255; b >= 1; b--) { first[b] = n; n += count[b]; }     /* bucket 0 (empty tiles) is left out */
    g_numTileJobs = n;
    for (int t = 0; t < g_ntiles; t++) {
        const unsigned b = cost_bucket((uint32_t)(g_tileSortKeys[t] >> 32));
        if (b) g_tileOrder[first[b]++] = t;
    }
}

/* any job bit of this tile in [lo, hi)? */
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

/* PH_TILES: one tile */
static void job_tile_body(int order, int tid);
static void job_tile(int order, int tid)
{
    const uint64_t t0 = g_opt[SW_OPT_PROFILE] ? __rdtsc() : 0;
    job_tile_body(order, tid);
    if (t0) {
        SwThreadStats *st = &g_stats[tid];
        const uint64_t t1 = __rdtsc();
        st->tsc[SW_PROF_BUSY] += t1 - t0;
        if (!st->tscFirst) st->tscFirst = t0;
        st->tscLast = t1;
    }
}

static void job_tile_body(int order, int tid)
{
    const int tile = g_tileOrder[order];
    const volatile LONG64 *bits = &g_tileJobBits[(size_t)tile * g_jobStride];
    /* this phase replays jobs [lo, hi); a view without a capture point has one phase with all of them */
    const int lo = g_phaseLo, hi = g_phaseHi;
    const int wLo = lo >> 6, wHi = (hi + 63) >> 6;
    const unsigned long long maskLo = ~0ull << (lo & 63);
    const unsigned long long maskHi = (hi & 63) ? ~(~0ull << (hi & 63)) : ~0ull;
    int any = 0, later = 0;
    for (int w = wLo; w < wHi; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        if (w == wLo) word &= maskLo;
        if (w == wHi - 1) word &= maskHi;
        if (word) { any = 1; break; }
    }
    if (!any) return;                                       /* nothing binned here: the framebuffer stands */
    if (g_phaseKeepsDepth)                                  /* does this tile come back after the capture? */
        for (int w = hi >> 6; w < g_jobWords; w++) {
            unsigned long long word = (unsigned long long)bits[w];
            if (w == (hi >> 6)) word &= ~0ull << (hi & 63);
            if (word) { later = 1; break; }
        }

    __declspec(align(64)) float    tz[TILE_PIXELS];
    __declspec(align(64)) uint8_t  ts[TILE_PIXELS];
    __declspec(align(64)) uint32_t tcol[TILE_PIXELS];
    TileCtx c;
    c.tx0 = (tile % g_tilesX) << TILE_SHIFT;
    c.ty0 = (tile / g_tilesX) << TILE_SHIFT;
    c.tz = tz; c.ts = ts; c.tc = tcol;
    c.zrange = 0;
    c.st = &g_stats[tid];
    c.optHier = g_opt[SW_OPT_HIER]; c.optZrange = g_opt[SW_OPT_ZRANGE]; c.optCellFast = g_opt[SW_OPT_CELL_FAST];
    c.kernelCut = g_opt[SW_OPT_KERNEL_CUT]; c.dbgEqual = g_dbgEqual; c.optLightCells = g_opt[SW_OPT_LIGHT_CELLS];

    c.capture = g_capture; c.captureRect = g_captureRect;
    c.optProfile = g_opt[SW_OPT_PROFILE]; c.optDepthBounds = g_opt[SW_OPT_DEPTH_BOUNDS];
    c.hasBounds = 0; c.boundsMin = 0.0f; c.boundsMax = 1.0f; c.boundsCells = 0xFFFFu; c.boundsFull = 0;
    uint64_t tsc0 = c.optProfile ? __rdtsc() : 0;

    tile_load_color(tcol, c.tx0, c.ty0);
    if (lo > 0 && g_savedDepth && g_savedDepth[tile] && g_savedDepthStamp[tile] == g_viewStamp) {
        memcpy(tz, g_savedDepth[tile], sizeof(tz));         /* the phase before the capture left its depth here */
    } else {
        const VF far1 = _mm512_set1_ps(1.0f);
        for (int i = 0; i < TILE_PIXELS; i += 16) _mm512_store_ps(tz + i, far1);
    }
    memset(ts, 128, TILE_PIXELS);                           /* RB_BeginDrawingView clears stencil to 128 */
    if (c.optProfile) { const uint64_t t = __rdtsc(); c.st->tsc[SW_PROF_LOAD] += t - tsc0; tsc0 = t; }

    int curSeg = -1, lastClear = -1, skipSeg = 0;
    int curView = g_phaseView;                              /* the view the tile's depth and stencil belong to */
    /* not while the stencil itself is the output (sw_debug_capture): then the counts ARE read, by the caller */
    const int optShadowCull = g_opt[SW_OPT_SHADOW_CULL] && !g_dbgStencil;
    for (int w = wLo; w < wHi; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        if (w == wLo) word &= maskLo;
        if (w == wHi - 1) word &= maskHi;
        while (word) {
            unsigned long bi;
            _BitScanForward64(&bi, word);
            word &= word - 1;
            const int j = w * 64 + (int)bi;
            const SwJob *job = &g_jobs[j];
            if (job->seg != curSeg) {
                const SwSegment *seg = &g_segs[job->seg];
                if (seg->view != curView) {
                    /* the next view of the frame starts here, as RB_BeginDrawingView does: depth far,
                       stencil 128, nothing known about z ranges. The colour stays: it is what the
                       view draws over. */
                    const VF far1 = _mm512_set1_ps(1.0f);
                    for (int i = 0; i < TILE_PIXELS; i += 16) _mm512_store_ps(tz + i, far1);
                    memset(ts, 128, TILE_PIXELS);
                    c.zrange = 0;
                    curView = seg->view;
                }
                /* Depth is final once the depth fill is over (I4). A later depth-writing draw
                   (post-process) drops the ranges and they rebuild at the next segment. */
                if (seg->kind != SW_SEG_DEPTH && !c.zrange) {
                    tile_build_zrange(&c);
                    if (c.optProfile) { const uint64_t t = __rdtsc(); c.st->tsc[SW_PROF_ZRANGE] += t - tsc0; tsc0 = t; }
                }
                /* Only the LATEST stencil clear at or before this segment is applied. Skipping
                   the ones in between is valid because every stencil draw and every stencil-
                   tested draw of a light is scissored inside that light's clear rectangle. */
                if (seg->clearSeg > lastClear) {
                    tile_clear_stencil(ts, &g_segs[seg->clearSeg].clear, c.tx0, c.ty0);
                    lastClear = seg->clearSeg;
                }
                curSeg = job->seg;
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
            }
            const SwDraw *d = &g_draws[job->draw];
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
            raster_bin(&g_bins[(size_t)j * g_ntiles + tile], d, &c);
            if (c.optProfile) {
                const uint64_t t = __rdtsc();
                const int kind = g_segs[job->seg].kind;
                c.st->tsc[kind == SW_SEG_DEPTH ? SW_PROF_DEPTH : (kind == SW_SEG_SHADOW ? SW_PROF_SHADOW : (kind == SW_SEG_LIGHT ? SW_PROF_LIGHT : SW_PROF_COLOR))] += t - tsc0;
                tsc0 = t;
            }
            if (d->op == SW_OP_DEPTH_FILL || (d->op == SW_OP_COLOR && d->depthWrite)) c.zrange = 0;
        }
    }

    tile_store_color(tcol, c.tx0, c.ty0);
    if (c.optProfile) c.st->tsc[SW_PROF_STORE] += __rdtsc() - tsc0;
    if (later) {
        if (!g_savedDepth[tile]) g_savedDepth[tile] = (float *)_aligned_malloc(sizeof(tz), 64);
        memcpy(g_savedDepth[tile], tz, sizeof(tz));
        g_savedDepthStamp[tile] = g_viewStamp;              /* one writer per tile per phase */
    }
    if (g_dbgDepth || g_dbgStencil) tile_debug_capture(tz, ts, c.tx0, c.ty0);
}

/* What of a capture its readers can reach. The readers are the SW_KERN_SCREEN draws between this
   capture point and the next (the views that follow included: a 2D view's post-process reads the
   3D view's capture). colorProcess reads its own pixel. The heat haze family moves the lookup by
   normal * deform, |normal| <= 1, and vertex_screen caps the deform at 0.02 * parm1 of the
   capture's size PROVIDED the projection is centred (then the term it caps is positive); if it is
   not, the draw may read anything. Tiles with reader jobs, grown by that reach: everything else of
   the 33 MB a 4K capture weighs is never looked at (measured: the full copy was 1.5 - 1.8 ms in
   more than half of demo1's frames). Returns 0 when nobody reads the capture at all. */
static int capture_region(int ci, SwRect *out)
{
    const int jlo = g_captureJobs[ci], jhi = ci + 1 < g_numCaptures ? g_captureJobs[ci + 1] : g_numJobs;
    const SwRect full = g_captureRects[ci];
    const float cw = (float)(full.x1 - full.x0 + 1), ch = (float)(full.y1 - full.y0 + 1);
    unsigned long long mask[64];
    int wlo = jlo >> 6, whi = (jhi + 63) >> 6, readers = 0, marginX = 0, marginY = 0, anything = 0;
    if (whi - wlo > 64) { *out = full; return 1; }          /* more jobs than the mask holds: do not bother */
    memset(mask, 0, sizeof(mask));
    for (int j = jlo; j < jhi; j++) {
        const SwDraw *d = &g_draws[g_jobs[j].draw];
        if (d->op != SW_OP_COLOR || d->kernel != SW_KERN_SCREEN) continue;
        const SwScreenParms *p = (const SwScreenParms *)d->kernelParms;
        mask[(j >> 6) - wlo] |= 1ull << (j & 63);
        readers++;
        if (p->program == SW_SCREEN_COLORPROCESS) continue;
        if (p->projRow0[0] < 0.0f || p->projRow0[2] != 0.0f || p->projRow0[3] != 0.0f) anything = 1;
        const int mx = (int)(0.02f * fabsf(p->parm1[0]) * cw) + 1, my = (int)(0.02f * fabsf(p->parm1[1]) * ch) + 1;
        if (mx > marginX) marginX = mx;
        if (my > marginY) marginY = my;
    }
    if (!readers) return 0;
    if (anything || !g_opt[SW_OPT_CAPTURE_REGION]) { *out = full; return 1; }
    int tx0 = g_tilesX, ty0 = g_tilesY, tx1 = -1, ty1 = -1;
    for (int t = 0; t < g_ntiles; t++) {
        const volatile LONG64 *bits = &g_tileJobBits[(size_t)t * g_jobStride];
        unsigned long long any = 0;
        for (int w = wlo; w < whi; w++) any |= (unsigned long long)bits[w] & mask[w - wlo];
        if (!any) continue;
        const int tx = t % g_tilesX, ty = t / g_tilesX;
        if (tx < tx0) tx0 = tx;
        if (tx > tx1) tx1 = tx;
        if (ty < ty0) ty0 = ty;
        if (ty > ty1) ty1 = ty;
    }
    if (tx1 < 0) return 0;                                  /* the readers were clipped away */
    /* + 4: the bilinear footprint, the rounding of the reach, a pixel centre just outside a snapped edge */
    out->x0 = (tx0 << TILE_SHIFT) - marginX - 4;  out->y0 = (ty0 << TILE_SHIFT) - marginY - 4;
    out->x1 = ((tx1 + 1) << TILE_SHIFT) - 1 + marginX + 4;  out->y1 = ((ty1 + 1) << TILE_SHIFT) - 1 + marginY + 4;
    if (out->x0 < full.x0) out->x0 = full.x0;
    if (out->y0 < full.y0) out->y0 = full.y0;
    if (out->x1 > full.x1) out->x1 = full.x1;
    if (out->y1 > full.y1) out->y1 = full.y1;
    return out->x1 >= out->x0 && out->y1 >= out->y0;
}

/* the capture copy: 64 rows of g_captureCopy per job, into their place in the capture */
static void job_capture(int chunk, int tid)
{
    (void)tid;
    const int cw = g_captureRect.x1 - g_captureRect.x0 + 1;
    const int w = g_captureCopy.x1 - g_captureCopy.x0 + 1, h = g_captureCopy.y1 - g_captureCopy.y0 + 1;
    const int r0 = chunk * 64, r1 = r0 + 64 < h ? r0 + 64 : h;
    for (int r = r0; r < r1; r++) {
        const int y = g_captureCopy.y0 + r;
        memcpy(g_capture->texels + (size_t)(y - g_captureRect.y0) * cw + (g_captureCopy.x0 - g_captureRect.x0),
               g_fb + (size_t)y * g_fbPitch + g_captureCopy.x0, (size_t)w * 4);
    }
}
