"""Stage 8: where the tile phase goes. Per-thread TSC sums by category, behind SW_OPT_PROFILE."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'

patch(SW + 'sw_api.h', [
("""    uint64_t    equalFailures;                  /* I2 counter: see sw_debug_count_equal_failures */""",
"""    /* SW_OPT_PROFILE: thread-summed time inside the tile phase, by category, divided by the thread
       count: what each category would cost in wall time if the threads were evenly loaded */
    double      tileMs[SW_PROF_COUNT];
    uint64_t    equalFailures;                  /* I2 counter: see sw_debug_count_equal_failures */"""),
("""typedef struct SwStats {""", """enum { SW_PROF_LOAD, SW_PROF_DEPTH, SW_PROF_ZRANGE, SW_PROF_SHADOW, SW_PROF_LIGHT, SW_PROF_COLOR, SW_PROF_STORE, SW_PROF_COUNT };

typedef struct SwStats {"""),
("""    SW_OPT_COUNT
};""", """    SW_OPT_PROFILE,         /* time the tile phase by category (two rdtsc per bin); default 0 */
    SW_OPT_COUNT
};"""),
])

patch(SW + 'core/types.h', [
("""    int         trisIn, trisSetup, trisClipped, trisDropped, binPushes;
    char        pad[52];            /* to 192 bytes = three whole cache lines; the array is 64-byte aligned */""",
"""    int         trisIn, trisSetup, trisClipped, trisDropped, binPushes;
    uint64_t    tsc[8];             /* SW_PROF_*: cycles inside the tile phase, by category */
    char        pad[52];            /* to 256 bytes = four whole cache lines; the array is 64-byte aligned */"""),
("""static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0 };""",
 """static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0 };
static double       g_tscPerMs;                 /* measured in sw_init */"""),
])

patch(SW + 'rast/raster.c', [
("""    int             optHier, optZrange, optCellFast, optLightCells, kernelCut, dbgEqual;  /* per-tile copies of the switches */""",
 """    int             optHier, optZrange, optCellFast, optLightCells, kernelCut, dbgEqual;  /* per-tile copies of the switches */
    int             optProfile;"""),
])

patch(SW + 'rast/tile.c', [
("""    c.capture = g_capture; c.captureRect = g_captureRect;

    tile_load_color(tcol, c.tx0, c.ty0);""", """    c.capture = g_capture; c.captureRect = g_captureRect;
    c.optProfile = g_opt[SW_OPT_PROFILE];
    uint64_t tsc0 = c.optProfile ? __rdtsc() : 0;

    tile_load_color(tcol, c.tx0, c.ty0);"""),
("""    memset(ts, 128, TILE_PIXELS);                           /* RB_BeginDrawingView clears stencil to 128 */

    int curSeg = -1, lastClear = -1;""", """    memset(ts, 128, TILE_PIXELS);                           /* RB_BeginDrawingView clears stencil to 128 */
    if (c.optProfile) { const uint64_t t = __rdtsc(); c.st->tsc[SW_PROF_LOAD] += t - tsc0; tsc0 = t; }

    int curSeg = -1, lastClear = -1;"""),
("""                if (seg->kind != SW_SEG_DEPTH && !c.zrange) tile_build_zrange(&c);""",
 """                if (seg->kind != SW_SEG_DEPTH && !c.zrange) {
                    tile_build_zrange(&c);
                    if (c.optProfile) { const uint64_t t = __rdtsc(); c.st->tsc[SW_PROF_ZRANGE] += t - tsc0; tsc0 = t; }
                }"""),
("""            raster_bin(&g_bins[(size_t)j * g_ntiles + tile], d, &c);
            if (d->op == SW_OP_DEPTH_FILL || (d->op == SW_OP_COLOR && d->depthWrite)) c.zrange = 0;""",
 """            raster_bin(&g_bins[(size_t)j * g_ntiles + tile], d, &c);
            if (c.optProfile) {
                const uint64_t t = __rdtsc();
                const int kind = g_segs[job->seg].kind;
                c.st->tsc[kind == SW_SEG_DEPTH ? SW_PROF_DEPTH : (kind == SW_SEG_SHADOW ? SW_PROF_SHADOW : (kind == SW_SEG_LIGHT ? SW_PROF_LIGHT : SW_PROF_COLOR))] += t - tsc0;
                tsc0 = t;
            }
            if (d->op == SW_OP_DEPTH_FILL || (d->op == SW_OP_COLOR && d->depthWrite)) c.zrange = 0;"""),
("""    tile_store_color(tcol, c.tx0, c.ty0);
    if (later) {""", """    tile_store_color(tcol, c.tx0, c.ty0);
    if (c.optProfile) c.st->tsc[SW_PROF_STORE] += __rdtsc() - tsc0;
    if (later) {"""),
])

patch(SW + 'sw_frame.c', [
("""    if (!cpu_has_avx512()) return 0;
    if (g_numThreads) return 1;""", """    if (!cpu_has_avx512()) return 0;
    if (g_numThreads) return 1;
    {   /* TSC ticks per millisecond, for SW_OPT_PROFILE */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        const uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000 < f.QuadPart * 20);
        g_tscPerMs = (double)(__rdtsc() - t0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }"""),
("""        o->kernelCalls += s->kernelCalls; o->kernelLanes += s->kernelLanes;""",
 """        for (int k = 0; k < SW_PROF_COUNT; k++) o->tileMs[k] += (double)s->tsc[k] / (g_tscPerMs * (double)g_numThreads);
        o->kernelCalls += s->kernelCalls; o->kernelLanes += s->kernelLanes;"""),
])
print('done')
