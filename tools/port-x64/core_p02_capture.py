"""Stage 7 in the core, part 2: bin functions of the new kernels, the capture point, sw_read_pixels."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'

patch(SW + 'sw_core.c', [
('#include "kern/k_interaction.c"\n', '#include "kern/k_interaction.c"\n#include "kern/k_misc.c"\n'),
])

patch(SW + 'core/types.h', [
("""static float *      g_dbgDepth;                 /* sw_debug_capture targets, NULL = off */""",
"""/* the view's capture point (_currentRender): jobs from g_captureJob on run in a second tile phase,
   after the framebuffer inside g_captureRect was copied into g_capture. Tiles that have work on
   both sides keep their depth in g_savedDepth between the phases. */
static int          g_captureJob;               /* -1 = the view has no capture point */
static SwRect       g_captureRect;
static SwImage *    g_capture;
static float **     g_savedDepth;               /* per tile, allocated on first use */
static int *        g_savedDepthStamp;          /* == g_viewStamp: valid for this view */
static int          g_phaseLo, g_phaseHi;       /* the job range the running tile phase replays */
static int          g_phaseKeepsDepth;          /* phase A of a captured view */

static float *      g_dbgDepth;                 /* sw_debug_capture targets, NULL = off */"""),
])

patch(SW + 'rast/tile.c', [
("""BIN_FN(bin_flat_al,      SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_FLAT)
#undef BIN_FN
""", """BIN_FN(bin_flat_al,      SW_OP_COLOR, SW_DEPTH_ALWAYS, SW_KERN_FLAT)
#undef BIN_FN
/* the rare kernels: one copy each, the depth test stays a run-time value */
#define BIN_FN_RT(name, kn)                                                                             \\
    static BIN_LINKAGE void name(const int *items, int n, const SwDraw *d, TileCtx *c, int fl)  \\
    { const int dt = d->depthTest; for (int i = 0; i < n; i++) raster_tri(&g_tris[items[i]], items[i], d, c, SW_OP_COLOR, dt, kn, fl); }
BIN_FN_RT(bin_dual,   SW_KERN_DUAL)
BIN_FN_RT(bin_env,    SW_KERN_ENV)
BIN_FN_RT(bin_screen, SW_KERN_SCREEN)
#undef BIN_FN_RT
"""),
("""        } else if (d->kernel == SW_KERN_STAGE) {
            if (dt == SW_DEPTH_EQUAL) bin_stage_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_stage_le(items, n, d, c, 0); else bin_stage_al(items, n, d, c, 0);
        } else {""", """        } else if (d->kernel == SW_KERN_STAGE) {
            if (dt == SW_DEPTH_EQUAL) bin_stage_eq(items, n, d, c, 0); else if (dt == SW_DEPTH_LEQUAL) bin_stage_le(items, n, d, c, 0); else bin_stage_al(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_DUAL) {
            bin_dual(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_ENV) {
            bin_env(items, n, d, c, 0);
        } else if (d->kernel == SW_KERN_SCREEN) {
            bin_screen(items, n, d, c, 0);
        } else {"""),
("""    if (d->op == SW_OP_COLOR) w = d->kernel == SW_KERN_INTERACTION ? 8 : (d->kernel == SW_KERN_STAGE ? 3 : 1);""",
 """    if (d->op == SW_OP_COLOR) w = d->kernel == SW_KERN_INTERACTION ? 8 : (d->kernel == SW_KERN_FLAT ? 1 : (d->kernel == SW_KERN_STAGE ? 3 : 5));"""),
# job_tile: a job range, depth kept across the capture point
("""    const int tile = g_tileOrder[order];
    const volatile LONG64 *bits = &g_tileJobBits[(size_t)tile * g_jobWords];
    int any = 0;
    for (int w = 0; w < g_jobWords; w++) if (bits[w]) { any = 1; break; }
    if (!any) return;                                       /* nothing binned here: the framebuffer stands */
""", """    const int tile = g_tileOrder[order];
    const volatile LONG64 *bits = &g_tileJobBits[(size_t)tile * g_jobWords];
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
"""),
("""    tile_load_color(tcol, c.tx0, c.ty0);
    {
        const VF far1 = _mm512_set1_ps(1.0f);
        for (int i = 0; i < TILE_PIXELS; i += 16) _mm512_store_ps(tz + i, far1);
    }
    memset(ts, 128, TILE_PIXELS);                           /* RB_BeginDrawingView clears stencil to 128 */

    int curSeg = -1, lastClear = -1;
    for (int w = 0; w < g_jobWords; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        while (word) {""", """    c.capture = g_capture; c.captureRect = g_captureRect;

    tile_load_color(tcol, c.tx0, c.ty0);
    if (lo > 0 && g_savedDepth && g_savedDepth[tile] && g_savedDepthStamp[tile] == g_viewStamp) {
        memcpy(tz, g_savedDepth[tile], sizeof(tz));         /* the phase before the capture left its depth here */
    } else {
        const VF far1 = _mm512_set1_ps(1.0f);
        for (int i = 0; i < TILE_PIXELS; i += 16) _mm512_store_ps(tz + i, far1);
    }
    memset(ts, 128, TILE_PIXELS);                           /* RB_BeginDrawingView clears stencil to 128 */

    int curSeg = -1, lastClear = -1;
    for (int w = wLo; w < wHi; w++) {
        unsigned long long word = (unsigned long long)bits[w];
        if (w == wLo) word &= maskLo;
        if (w == wHi - 1) word &= maskHi;
        while (word) {"""),
("""    tile_store_color(tcol, c.tx0, c.ty0);
    if (g_dbgDepth || g_dbgStencil) tile_debug_capture(tz, ts, c.tx0, c.ty0);
}""", """    tile_store_color(tcol, c.tx0, c.ty0);
    if (later) {
        if (!g_savedDepth[tile]) g_savedDepth[tile] = (float *)_aligned_malloc(sizeof(tz), 64);
        memcpy(g_savedDepth[tile], tz, sizeof(tz));
        g_savedDepthStamp[tile] = g_viewStamp;              /* one writer per tile per phase */
    }
    if (g_dbgDepth || g_dbgStencil) tile_debug_capture(tz, ts, c.tx0, c.ty0);
}

/* the capture copy: 64 rows of g_captureRect per job */
static void job_capture(int chunk, int tid)
{
    (void)tid;
    const int w = g_captureRect.x1 - g_captureRect.x0 + 1, h = g_captureRect.y1 - g_captureRect.y0 + 1;
    const int r0 = chunk * 64, r1 = r0 + 64 < h ? r0 + 64 : h;
    for (int r = r0; r < r1; r++)
        memcpy(g_capture->texels + (size_t)r * w, g_fb + (size_t)(g_captureRect.y0 + r) * g_fbPitch + g_captureRect.x0, (size_t)w * 4);
}"""),
])

patch(SW + 'sw_frame.c', [
("""    _aligned_free((void *)g_tileJobBits); g_tileJobBits = NULL; g_capJobWords = 0;
    free(g_tileOrder); free(g_tileSortKeys); g_tileOrder = NULL; g_tileSortKeys = NULL;
}""", """    _aligned_free((void *)g_tileJobBits); g_tileJobBits = NULL; g_capJobWords = 0;
    free(g_tileOrder); free(g_tileSortKeys); g_tileOrder = NULL; g_tileSortKeys = NULL;
    if (g_savedDepth) for (int i = 0; i < g_ntiles; i++) _aligned_free(g_savedDepth[i]);
    free(g_savedDepth); free(g_savedDepthStamp); g_savedDepth = NULL; g_savedDepthStamp = NULL;
    sw_image_destroy(g_capture); g_capture = NULL;
}"""),
("""    g_tileOrder = (int *)malloc((size_t)g_ntiles * sizeof(int));
    g_tileSortKeys = (uint64_t *)malloc((size_t)g_ntiles * sizeof(uint64_t));
}""", """    g_tileOrder = (int *)malloc((size_t)g_ntiles * sizeof(int));
    g_tileSortKeys = (uint64_t *)malloc((size_t)g_ntiles * sizeof(uint64_t));
    g_savedDepth = (float **)calloc((size_t)g_ntiles, sizeof(float *));
    g_savedDepthStamp = (int *)calloc((size_t)g_ntiles, sizeof(int));
}"""),
("""    g_arenaUsed = 0; g_numXforms = 0;
    g_inView = 1;""", """    g_arenaUsed = 0; g_numXforms = 0;
    g_captureJob = -1;
    g_inView = 1;"""),
("""static double now_ms(void)""", """void sw_capture_point(const SwRect *rect)
{
    if (!g_inView || g_captureJob >= 0) return;             /* one per view */
    g_captureJob = g_numJobs;
    g_captureRect = *rect;
    if (g_captureRect.x0 < 0) g_captureRect.x0 = 0;
    if (g_captureRect.y0 < 0) g_captureRect.y0 = 0;
    if (g_captureRect.x1 > g_width - 1) g_captureRect.x1 = g_width - 1;
    if (g_captureRect.y1 > g_height - 1) g_captureRect.y1 = g_height - 1;
}

void sw_read_pixels(const SwRect *rect, uint32_t *rgba, int pitch)
{
    for (int y = rect->y0; y <= rect->y1; y++)
        for (int x = rect->x0; x <= rect->x1; x++)
            rgba[(size_t)(y - rect->y0) * pitch + (x - rect->x0)] =
                (x >= 0 && y >= 0 && x < g_width && y < g_height) ? g_fb[(size_t)y * g_fbPitch + x] : 0xFF000000u;
}

static double now_ms(void)"""),
("""    dispatch(job_tile, g_numTileJobs);
    const double t2 = now_ms();""", """    const int cw = g_captureRect.x1 - g_captureRect.x0 + 1, ch = g_captureRect.y1 - g_captureRect.y0 + 1;
    if (g_captureJob < 0 || g_captureJob >= g_numJobs || cw <= 0 || ch <= 0) {
        g_phaseLo = 0; g_phaseHi = g_numJobs; g_phaseKeepsDepth = 0;
        dispatch(job_tile, g_numTileJobs);
    } else {
        /* (I5) a whole-frame read is a sync point: everything before it, the copy, everything after */
        g_phaseLo = 0; g_phaseHi = g_captureJob; g_phaseKeepsDepth = 1;
        if (g_captureJob > 0) dispatch(job_tile, g_numTileJobs);
        if (!g_capture || g_capture->w0 != cw || g_capture->h0 != ch) {
            sw_image_destroy(g_capture);
            uint8_t *blank = (uint8_t *)calloc((size_t)cw * ch, 4);
            const uint8_t *level = blank;
            g_capture = sw_image_create(cw, ch, 1, &level, SW_WRAP_CLAMP);
            free(blank);
        }
        dispatch(job_capture, (ch + 63) / 64);
        g_phaseLo = g_captureJob; g_phaseHi = g_numJobs; g_phaseKeepsDepth = 0;
        dispatch(job_tile, g_numTileJobs);
    }
    const double t2 = now_ms();"""),
])
print('done')
