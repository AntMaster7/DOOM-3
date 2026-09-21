/* sw_frame.c -- the implementation of sw_api.h: init, framebuffer, the view's segment and draw
   lists, and sw_end_view, which runs the two pool phases (geometry, tiles).
   Everything in here runs on the calling thread except the jobs dispatch() hands out. */
#include "core/base.h"
#include "core/config.h"
#include "core/types.h"

static int cpu_has_avx512(void)
{
    int r[4];
    __cpuid(r, 0);
    if (r[0] < 7) return 0;
    __cpuid(r, 1);
    if (!(r[2] & (1 << 27))) return 0;                      /* OSXSAVE */
    if ((_xgetbv(0) & 0xE6) != 0xE6) return 0;              /* the OS saves xmm, ymm, opmask and zmm state */
    __cpuidex(r, 7, 0);
    const unsigned need = (1u << 16) | (1u << 17) | (1u << 30) | (1u << 31);   /* F, DQ, BW, VL */
    return ((unsigned)r[1] & need) == need;
}

int sw_init(int threads)
{
    if (!cpu_has_avx512()) return 0;
    if (g_numThreads) return 1;
    {   /* TSC ticks per millisecond, for SW_OPT_PROFILE */
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f); QueryPerformanceCounter(&a);
        const uint64_t t0 = __rdtsc();
        do QueryPerformanceCounter(&b); while ((b.QuadPart - a.QuadPart) * 1000 < f.QuadPart * 20);
        g_tscPerMs = (double)(__rdtsc() - t0) / ((double)(b.QuadPart - a.QuadPart) * 1000.0 / (double)f.QuadPart);
    }
    pool_init(threads);
    g_triCap = 4 << 20;                                     /* demand-zero pages: only what a view touches is ever real */
    g_tris = (SwTri *)VirtualAlloc(NULL, (size_t)g_triCap * sizeof(SwTri), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_triAttrs = (SwTriAttr *)VirtualAlloc(NULL, (size_t)g_triCap * sizeof(SwTriAttr), MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_tris != NULL && g_triAttrs != NULL && arena_init();
}

static void free_bins(void)
{
    if (g_bins) {
        for (size_t i = 0; i < g_binRows * (size_t)g_ntiles; i++) free(g_bins[i].items);
        free(g_bins);
    }
    g_bins = NULL; g_binRows = 0;
    _aligned_free((void *)g_tileJobBits); g_tileJobBits = NULL; g_capJobWords = 0;
    free(g_tileOrder); free(g_tileSortKeys); g_tileOrder = NULL; g_tileSortKeys = NULL;
    if (g_savedDepth) for (int i = 0; i < g_ntiles; i++) _aligned_free(g_savedDepth[i]);
    free(g_savedDepth); free(g_savedDepthStamp); g_savedDepth = NULL; g_savedDepthStamp = NULL;
    sw_image_destroy(g_capture); g_capture = NULL;
}

static void free_framebuffers(void)
{
    for (int i = 0; i < SW_MAX_FRAMEBUFFERS; i++) {
        if (g_fbs[i]) VirtualFree(g_fbs[i], 0, MEM_RELEASE);
        g_fbs[i] = NULL;
    }
    g_fb = NULL;
}

void sw_shutdown(void)
{
    async_shutdown();
    pool_shutdown();
    free_bins();
    free_framebuffers();
    if (g_tris) VirtualFree(g_tris, 0, MEM_RELEASE);
    if (g_triAttrs) VirtualFree(g_triAttrs, 0, MEM_RELEASE);
    g_triAttrs = NULL;
    arena_shutdown();
    free(g_draws); free(g_segs); free(g_jobs);
    g_fb = NULL; g_tris = NULL; g_draws = NULL; g_segs = NULL; g_jobs = NULL;
    g_capDraws = g_capSegs = g_capJobs = 0;
    g_width = g_height = 0;
}

int sw_num_threads(void) { return g_numThreads; }

void sw_set_option(int option, int value)
{
    frame_wait();
    if (option >= 0 && option < SW_OPT_COUNT) g_opt[option] = value;
}

/* (I5) A read of the framebuffer, or a write from outside, while a frame is being recorded: what was
   recorded is rendered first, and the frame goes on after it. */
static void frame_sync(void)
{
    frame_wait();
    if (g_frameOpen && !g_inView && g_numJobs) { sw_end_frame(); sw_begin_frame(); }
}

void sw_resize(int width, int height)
{
    frame_wait();
    if (width == g_width && height == g_height) return;
    sw_end_frame();
    free_bins();
    free_framebuffers();
    g_width = width; g_height = height;
    g_tilesX = (width + TILE_SIZE - 1) >> TILE_SHIFT;
    g_tilesY = (height + TILE_SIZE - 1) >> TILE_SHIFT;
    g_ntiles = g_tilesX * g_tilesY;
    g_fbPitch = g_tilesX * TILE_SIZE;                       /* whole tiles: loads and streamed stores need no edge cases */
    g_fbBytes = ((size_t)g_fbPitch * g_tilesY * TILE_SIZE * 4 + 65535) & ~(size_t)65535;
    g_fb = g_fbs[0] = (uint32_t *)VirtualAlloc(NULL, g_fbBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    g_tileOrder = (int *)malloc((size_t)g_ntiles * sizeof(int));
    g_tileSortKeys = (uint64_t *)malloc((size_t)g_ntiles * sizeof(uint64_t));
    g_savedDepth = (float **)calloc((size_t)g_ntiles, sizeof(float *));
    g_savedDepthStamp = (int *)calloc((size_t)g_ntiles, sizeof(int));
}

void sw_clear_framebuffer(uint32_t rgba)
{
    frame_sync();
    const size_t n = (size_t)g_fbPitch * g_tilesY * TILE_SIZE;
    for (size_t i = 0; i < n; i++) g_fb[i] = rgba;
}

size_t sw_framebuffer_bytes(void) { return g_fb ? g_fbBytes : 0; }

/* A presenter that lets the GPU read a frame in place renders the next one somewhere else. The
   selected framebuffer is what views draw into and what every read sees; a tile still loads its
   colour first (I6), so pixels nobody draws show that buffer's older frame. */
const uint32_t *sw_framebuffer_select(int index)
{
    if (index < 0 || index >= SW_MAX_FRAMEBUFFERS || g_inView || !g_fbs[0]) return g_fb;
    frame_sync();
    if (!g_fbs[index]) {
        g_fbs[index] = (uint32_t *)VirtualAlloc(NULL, g_fbBytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!g_fbs[index]) return g_fb;
        memcpy(g_fbs[index], g_fb, g_fbBytes);              /* its first "older frame" is the current one */
    }
    g_fb = g_fbs[index];
    return g_fb;
}

const uint32_t *sw_framebuffer(int *pitchPixels)
{
    frame_sync();
    if (pitchPixels) *pitchPixels = g_fbPitch;
    return g_fb;
}

/* A frame: the views between sw_begin_frame and sw_end_frame share one job list and ONE tile pass
   (every tile is loaded and stored once, whatever the number of views). Without a frame, a view
   is a frame of its own, as the harness uses it. */
void sw_begin_frame(void)
{
    frame_wait();                                           /* the last frame's tile pass reads what is reset here */
    if (g_frameOpen) sw_end_frame();
    g_frameOpen = 1; g_frameImplicit = 0;
    g_viewIndex = 0;
    g_numDraws = g_numSegs = g_numJobs = 0;
    g_arenaUsed = 0;
    g_numCaptures = 0;
    g_jobWords = 0;
    g_viewStamp++;                                          /* bins are stamped per FRAME: a view's bins wait for the tile pass */
    g_triCount = 0;
    for (int i = 0; i < MAX_THREADS; i++) g_slot[i].next = g_slot[i].end = 0;
    memset(g_stats, 0, sizeof(g_stats));
    g_frameXformMs = g_frameSetupMs = 0.0;
}

void sw_begin_view(const SwRect *viewport)
{
    if (!g_frameOpen) { sw_begin_frame(); g_frameImplicit = 1; }
    g_view = *viewport;
    if (g_view.x0 < 0) g_view.x0 = 0;
    if (g_view.y0 < 0) g_view.y0 = 0;
    if (g_view.x1 > g_width - 1) g_view.x1 = g_width - 1;
    if (g_view.y1 > g_height - 1) g_view.y1 = g_height - 1;
    /* the viewport transform uses the UNclamped rectangle, as glViewport does */
    g_viewHw = 0.5f * (float)(viewport->x1 - viewport->x0 + 1);
    g_viewHh = 0.5f * (float)(viewport->y1 - viewport->y0 + 1);
    g_viewCx = (float)viewport->x0 + g_viewHw;
    g_viewCy = (float)viewport->y0 + g_viewHh;
    /* guard band: clipped screen extents (2G * hw) + (2G * hh) must stay under EDGE_EXT_MAX */
    float guard = floorf(0.9f * (float)EDGE_EXT_MAX / (2.0f * (g_viewHw + g_viewHh)));
    g_guard = guard < 2.0f ? 2.0f : (guard > 256.0f ? 256.0f : guard);
    g_viewFirstJob = g_numJobs; g_viewFirstSeg = g_numSegs;
    g_numXforms = 0;                                        /* transforms are per view: they run in its geometry phase */
    g_viewCaptured = 0;
    g_inView = 1;
}

void sw_begin_segment(int kind, const SwRect *stencilClear)
{
    if (g_numSegs == g_capSegs) {
        g_capSegs = g_capSegs ? g_capSegs * 2 : 256;
        g_segs = (SwSegment *)realloc(g_segs, (size_t)g_capSegs * sizeof(SwSegment));
    }
    SwSegment *s = &g_segs[g_numSegs];
    s->kind = kind;
    s->view = g_viewIndex;
    s->firstJob = g_numJobs; s->numJobs = 0;
    s->hasClear = stencilClear != NULL;
    if (stencilClear) s->clear = *stencilClear;
    /* a clear belongs to its view: the stencil starts at 128 again when the next view does */
    s->clearSeg = s->hasClear ? g_numSegs : (g_numSegs > g_viewFirstSeg ? g_segs[g_numSegs - 1].clearSeg : -1);
    s->testsStencil = 0; s->numConsumers = 0;
    g_numSegs++;
}

void sw_draw(const SwDraw *draw)
{
    if (!g_inView || g_numSegs <= g_viewFirstSeg || draw->numIndexes < 3) return;
    if (g_numDraws == g_capDraws) {
        g_capDraws = g_capDraws ? g_capDraws * 2 : 1024;
        g_draws = (SwDraw *)realloc(g_draws, (size_t)g_capDraws * sizeof(SwDraw));
    }
    g_draws[g_numDraws] = *draw;
    if (draw->op == SW_OP_COLOR && draw->stencilTest) g_segs[g_numSegs - 1].testsStencil = 1;
    const int tris = draw->numIndexes / 3;
    for (int first = 0; first < tris; first += JOB_TRIS) {
        if (g_numJobs == g_capJobs) {
            g_capJobs = g_capJobs ? g_capJobs * 2 : 1024;
            g_jobs = (SwJob *)realloc(g_jobs, (size_t)g_capJobs * sizeof(SwJob));
        }
        SwJob *j = &g_jobs[g_numJobs++];
        j->seg = g_numSegs - 1; j->draw = g_numDraws;
        j->firstTri = first;
        j->numTris = tris - first < JOB_TRIS ? tris - first : JOB_TRIS;
        g_segs[g_numSegs - 1].numJobs++;
    }
    g_numDraws++;
}

void sw_capture_point(const SwRect *rect)
{
    if (!g_inView || g_viewCaptured || g_numCaptures == SW_MAX_CAPTURES) return;    /* one per view */
    SwRect r = *rect;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > g_width - 1) r.x1 = g_width - 1;
    if (r.y1 > g_height - 1) r.y1 = g_height - 1;
    if (r.x1 < r.x0 || r.y1 < r.y0) return;
    g_viewCaptured = 1;
    g_captureJobs[g_numCaptures] = g_numJobs; g_captureViews[g_numCaptures] = g_viewIndex;
    g_captureRects[g_numCaptures] = r;
    g_numCaptures++;
}

void sw_read_pixels(const SwRect *rect, uint32_t *rgba, int pitch)
{
    frame_sync();
    for (int y = rect->y0; y <= rect->y1; y++)
        for (int x = rect->x0; x <= rect->x1; x++)
            rgba[(size_t)(y - rect->y0) * pitch + (x - rect->x0)] =
                (x >= 0 && y >= 0 && x < g_width && y < g_height) ? g_fb[(size_t)y * g_fbPitch + x] : 0xFF000000u;
}

/* The finished frame into memory the caller owns (a presenter's upload buffer), in the pool:
   64 rows per job. Such memory is usually write-combined: written once, front to back, never read. */
static uint8_t *g_copyDst;
static int g_copyPitch;

static void job_copy_frame(int chunk, int tid)
{
    (void)tid;
    const int y1 = (chunk + 1) * 64 < g_height ? (chunk + 1) * 64 : g_height;
    for (int y = chunk * 64; y < y1; y++)
        memcpy(g_copyDst + (size_t)y * g_copyPitch * 4, g_fb + (size_t)y * g_fbPitch, (size_t)g_width * 4);
}

void sw_copy_frame(void *dst, int pitchPixels)
{
    if (!g_fb || !dst || g_inView) return;
    frame_sync();
    g_copyDst = (uint8_t *)dst; g_copyPitch = pitchPixels;
    dispatch(job_copy_frame, (g_height + 63) / 64);
}

static double now_ms(void)
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static void job_setup_view(int j, int tid)
{
    const uint64_t t0 = g_opt[SW_OPT_PROFILE] ? __rdtsc() : 0;
    job_setup(j + g_setupBase, tid);
    if (t0) g_stats[tid].tsc[SW_PROF_SETUP] += __rdtsc() - t0;
}

/* room in the per-tile job bits for every job so far. The stride (words per tile) is chosen when the
   frame's first view ends, with headroom for the small views that follow (a HUD), and the whole
   array is cleared once; a later view that does not fit re-lays the rows out. */
static void job_bits_reserve(void)
{
    const int words = (g_numJobs + 63) >> 6;
    if (g_jobWords == 0) {                                  /* first view of the frame */
        g_jobStride = words + 8;
        if (g_jobStride > g_capJobWords) {
            _aligned_free((void *)g_tileJobBits);
            g_capJobWords = g_jobStride * 2;
            g_tileJobBits = (volatile LONG64 *)_aligned_malloc((size_t)g_ntiles * g_capJobWords * sizeof(LONG64), 64);
        }
        memset((void *)g_tileJobBits, 0, (size_t)g_ntiles * g_jobStride * sizeof(LONG64));
    } else if (words > g_jobStride) {
        const int stride = words + 8;
        LONG64 *bits = (LONG64 *)_aligned_malloc((size_t)g_ntiles * stride * 2 * sizeof(LONG64), 64);
        memset(bits, 0, (size_t)g_ntiles * stride * sizeof(LONG64));
        for (int t = 0; t < g_ntiles; t++)
            memcpy(bits + (size_t)t * stride, (const void *)(g_tileJobBits + (size_t)t * g_jobStride), (size_t)g_jobStride * sizeof(LONG64));
        _aligned_free((void *)g_tileJobBits);
        g_tileJobBits = (volatile LONG64 *)bits; g_capJobWords = stride * 2; g_jobStride = stride;
    }
    g_jobWords = words;
}

/* The view's geometry phase: now, while its viewport transform stands and the caller's vertex
   data is alive. Its tiles wait for the end of the frame. */
void sw_end_view(void)
{
    if (!g_inView) return;
    g_inView = 0;
    if (g_numJobs > g_viewFirstJob && g_ntiles) {
        /* bins: one row per geometry job; rows only ever grow, new ones start stamped empty */
        if ((size_t)g_numJobs > g_binRows) {
            size_t rows = g_binRows ? g_binRows : 256;
            while (rows < (size_t)g_numJobs) rows *= 2;
            g_bins = (SwBin *)realloc(g_bins, rows * (size_t)g_ntiles * sizeof(SwBin));
            memset(g_bins + g_binRows * (size_t)g_ntiles, 0, (rows - g_binRows) * (size_t)g_ntiles * sizeof(SwBin));
            g_binRows = rows;
        }
        job_bits_reserve();

        const double tx = now_ms();
        xform_make_batches();
        if (g_numXformBatches == 1) job_xform(0, 0);     /* a 2D view: not worth waking 31 threads */
        else dispatch(job_xform, g_numXformBatches);
        const double t0 = now_ms();
        g_setupBase = g_viewFirstJob;
        dispatch(job_setup_view, g_numJobs - g_viewFirstJob);
        g_frameXformMs += t0 - tx; g_frameSetupMs += now_ms() - t0;
    }
    g_viewIndex++;
    if (g_frameImplicit) sw_end_frame();
}

/* Closes the recording. Returns 0 when there is nothing to render. */
static int frame_close(void)
{
    if (g_inView) sw_end_view();
    if (!g_frameOpen) return 0;
    g_frameOpen = 0; g_frameImplicit = 0;
    if (!g_numJobs || !g_ntiles) { memset(&g_lastStats, 0, sizeof(g_lastStats)); return 0; }
    return 1;
}

/* The frame's ONE tile pass over the jobs of all its views. Runs on the caller's thread or on the
   coordinator's: it touches nothing but the core's own memory. */
static void frame_tiles(void)
{

    if (g_dbgDepth) for (size_t i = 0; i < (size_t)g_width * g_height; i++) g_dbgDepth[i] = 1.0f;
    if (g_dbgStencil) memset(g_dbgStencil, 128, (size_t)g_width * g_height);

    /* who reads each shadow segment's counts: the stencil-testing segments of ITS VIEW up to the next clear */
    for (int si = 0; si < g_numSegs; si++) {
        SwSegment *s = &g_segs[si];
        if (s->kind != SW_SEG_SHADOW) continue;
        s->numConsumers = 0;
        for (int k = si + 1; k < g_numSegs && !g_segs[k].hasClear && g_segs[k].view == s->view; k++) {
            const SwSegment *c = &g_segs[k];
            if (!c->testsStencil || !c->numJobs) continue;
            if (s->numConsumers == 4) { s->numConsumers = -1; break; }      /* too many: always needed */
            s->consumerLo[s->numConsumers] = c->firstJob; s->consumerHi[s->numConsumers] = c->firstJob + c->numJobs;
            s->numConsumers++;
        }
    }

    g_texWindow = g_opt[SW_OPT_TEX_WINDOW];
    g_texLevel0 = g_opt[SW_OPT_TEX_LEVEL0];
    g_envHelpers = g_opt[SW_OPT_ENV_HELPERS];
    g_texTrilinear = g_opt[SW_OPT_TRILINEAR];
    const double t1 = now_ms();
    tiles_sort_by_cost();                                   /* serial: keep it cheap */
    const double t1b = now_ms();
    /* (I5) a capture point is a sync point: the jobs before it, the copy, the jobs after it */
    int lo = 0;
    uint64_t passLate = 0, passEarly = 0;
    double captureMs = 0.0, captureMpx = 0.0;
    g_phaseView = 0;
    for (int ci = 0; ci <= g_numCaptures; ci++) {
        const int hi = ci < g_numCaptures ? g_captureJobs[ci] : g_numJobs;
        g_phaseLo = lo; g_phaseHi = hi; g_phaseKeepsDepth = ci < g_numCaptures;
        if (hi > lo) {
            for (int i = 0; i < MAX_THREADS; i++) g_stats[i].tscFirst = g_stats[i].tscLast = 0;
            g_passStartTsc = __rdtsc();
            dispatch(job_tile, g_numTileJobs);
            const uint64_t passEnd = __rdtsc();
            for (int i = 0; i < g_numThreads; i++) {
                /* a thread that got no tile at all was late for the whole pass */
                const uint64_t first = g_stats[i].tscFirst ? g_stats[i].tscFirst : passEnd, last = g_stats[i].tscLast ? g_stats[i].tscLast : passEnd;
                passLate += first - g_passStartTsc; passEarly += passEnd - last;
            }
        }
        if (ci < g_numCaptures) {
            const double tc0 = now_ms();
            g_captureRect = g_captureRects[ci];
            const int cw = g_captureRect.x1 - g_captureRect.x0 + 1, ch = g_captureRect.y1 - g_captureRect.y0 + 1;
            if (!g_capture || g_capture->w0 != cw || g_capture->h0 != ch) {
                image_free(g_capture);                      /* NOT sw_image_destroy: that one waits for this very pass */
                uint8_t *blank = (uint8_t *)calloc((size_t)cw * ch, 4);
                const uint8_t *level = blank;
                g_capture = sw_image_create(cw, ch, 1, &level, SW_WRAP_CLAMP);
                free(blank);
            }
            if (capture_region(ci, &g_captureCopy)) {
                dispatch(job_capture, (g_captureCopy.y1 - g_captureCopy.y0 + 1 + 63) / 64);
                captureMpx += 1e-6 * (double)(g_captureCopy.x1 - g_captureCopy.x0 + 1) * (double)(g_captureCopy.y1 - g_captureCopy.y0 + 1);
            }
            captureMs += now_ms() - tc0;
            g_phaseView = g_captureViews[ci];
        }
        lo = hi;
    }
    const double t2 = now_ms();

    SwStats *o = &g_lastStats;
    memset(o, 0, sizeof(*o));
    o->xformMs = g_frameXformMs; o->setupMs = g_frameSetupMs; o->sortMs = t1b - t1; o->tilesMs = t2 - t1b;
    o->draws = g_numDraws; o->jobs = g_numJobs;
    for (int i = 0; i < MAX_THREADS; i++) {
        const SwThreadStats *s = &g_stats[i];
        o->trisIn += s->trisIn; o->trisSetup += s->trisSetup; o->trisClipped += s->trisClipped;
        o->trisDropped += s->trisDropped; o->binPushes += s->binPushes; o->binSkips += s->binSkips;
        o->blocksVisited += s->blocksVisited; o->blocksCovered += s->blocksCovered;
        o->pxDepth += s->pxDepth; o->pxStencil += s->pxStencil; o->pxColor += s->pxColor;
        o->cellsRejected += s->cellsRejected; o->cellsFast += s->cellsFast;
        o->triTileRejected += s->triTileRejected; o->equalFailures += s->equalFailures; o->equalInFront += s->equalInFront;
        for (int k = 0; k < SW_PROF_COUNT; k++) o->tileMs[k] += (double)s->tsc[k] / (g_tscPerMs * (double)g_numThreads);
        o->kernelCalls += s->kernelCalls; o->kernelLanes += s->kernelLanes;
        o->lightLanes += s->lightLanes; o->lightCalls += s->lightCalls;
        o->shadowBinsCulled += s->shadowBinsCulled; o->shadowBinsDrawn += s->shadowBinsDrawn;
        for (int k = 0; k < 16; k++) o->ltCallsByFlags[k] += s->ltCallsByFlags[k];
        o->ltLanesLit += s->ltLanesLit; o->ltLanesFacing += s->ltLanesFacing; o->ltLanesSpec += s->ltLanesSpec;
        for (int k = 0; k < 6; k++) { o->stLanes[k] += s->stLanes[k]; o->stCalls[k] += s->stCalls[k]; o->stWinHits[k] += s->stWinHits[k]; }
        o->stVertexColorCalls += s->stVertexColorCalls;
        o->shTris += s->shTris; o->shTrisWalked += s->shTrisWalked; o->shBlocksVisited += s->shBlocksVisited;
        o->shBlocksCovered += s->shBlocksCovered; o->shBlocksWritten += s->shBlocksWritten; o->shCellsFast += s->shCellsFast;
        o->lightBlocksDark += s->lightBlocksDark; o->lightLanesDark += s->lightLanesDark; o->lightCellsDark += s->lightCellsDark;
    }
    o->captureMs = captureMs; o->captureMpx = captureMpx;

    if (g_opt[SW_OPT_PROFILE]) {
        o->lateMs = (double)passLate / (g_tscPerMs * (double)g_numThreads);
        o->earlyMs = (double)passEarly / (g_tscPerMs * (double)g_numThreads);
    }
}

void sw_end_frame(void)
{
    frame_wait();
    if (frame_close()) frame_tiles();
}

/* ---- the coordinator: a frame's tile pass next to the caller's next frame ---- */

static DWORD WINAPI async_proc(LPVOID param)
{
    (void)param;
    for (;;) {
        WaitForSingleObject(g_asyncGo, INFINITE);
        if (g_asyncQuit) return 0;
        frame_tiles();
        if (g_asyncDoneFn) g_asyncDoneFn(g_asyncDoneCtx);   /* the presenter: it sees a finished frame */
        InterlockedExchange(&g_asyncBusy, 0);
        SetEvent(g_asyncDone);
    }
}

static void frame_wait(void)
{
    /* never from the pass itself: that froze the first frame with a capture point (the pass re-created
       its capture image through sw_image_destroy, which waits here) */
    if (g_asyncBusy && GetCurrentThreadId() != g_asyncThreadId) WaitForSingleObject(g_asyncDone, INFINITE);
}

void sw_wait(void) { frame_wait(); }

/* As sw_end_frame, but the tile pass runs on the coordinator thread and `done( ctx )` is called
   there when the frame is finished; the call returns as soon as the pass is handed over. */
void sw_end_frame_async(void (*done)(void *ctx), void *ctx)
{
    frame_wait();
    if (!frame_close()) {
        if (done) done(ctx);
        return;
    }
    if (!g_asyncThread) {
        g_asyncGo = CreateEventA(NULL, FALSE, FALSE, NULL);
        g_asyncDone = CreateEventA(NULL, TRUE, TRUE, NULL);     /* manual reset: any number of waiters, any number of times */
        g_asyncQuit = 0;
        g_asyncThread = CreateThread(NULL, 0, async_proc, NULL, 0, &g_asyncThreadId);
        if (!g_asyncThread) {
            frame_tiles();
            if (done) done(ctx);
            return;
        }
    }
    g_asyncDoneFn = done; g_asyncDoneCtx = ctx;
    ResetEvent(g_asyncDone);
    InterlockedExchange(&g_asyncBusy, 1);
    SetEvent(g_asyncGo);
}

static void async_shutdown(void)
{
    frame_wait();
    if (!g_asyncThread) return;
    g_asyncQuit = 1;
    SetEvent(g_asyncGo);
    WaitForSingleObject(g_asyncThread, INFINITE);
    CloseHandle(g_asyncThread); CloseHandle(g_asyncGo); CloseHandle(g_asyncDone);
    g_asyncThread = NULL; g_asyncGo = g_asyncDone = NULL;
}

void sw_get_stats(SwStats *out) { frame_wait(); *out = g_lastStats; }

void sw_debug_capture(float *depth, uint8_t *stencil) { frame_wait(); g_dbgDepth = depth; g_dbgStencil = stencil; }
void sw_debug_count_equal_failures(int enable) { frame_wait(); g_dbgEqual = enable; }
