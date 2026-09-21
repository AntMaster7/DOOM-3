/* core/types.h -- the core's types and its frame state. One translation unit, so the state is
   a set of statics, as in CRenderer. */
#pragma once
#include "base.h"
#include "config.h"
#include "../sw_api.h"

/* A set-up triangle: the hot header every raster op reads. Attribute planes for the shading
   kernels will live in a separate block, so depth fill and stencil never pull them in.

   All planes are anchored at (ox, oy), the corner of the triangle's bbox clamped to the VIEWPORT
   (invariant I3): scissor rectangles differ between passes, the origin must not. min/max is the
   rectangle that is walked: bbox, viewport and the draw's scissor intersected.
   Edges: e_k(px, py) = A[k] * (px - ox) + B[k] * (py - oy) + C[k] at whole pixels, inside is
   e >= 0 (the fill-rule bias is inside C). */
typedef struct SwTri {
    int32_t     A[3], B[3];
    int64_t     C[3];
    float       zc, zgx, zgy;       /* depth plane at the centre of pixel (ox, oy) + gradients */
    float       minz, maxz;         /* vertex depth range, offset included: every sample clamps to it */
    int16_t     ox, oy;
    int16_t     minx, miny, maxx, maxy;
    int32_t     draw;               /* index into the view's draws */
    int32_t     back;               /* 1: DOOM's back side faces the viewer (mirror already applied) */
} SwTri;

/* The attribute planes of a triangle whose kernel has varyings, in a block of its own so depth
   fill and stencil never pull it into cache. Plane k is value(X, Y) = p[k][0] + p[k][1] * X +
   p[k][2] * Y at the local pixel centres the depth plane uses. w is 1/w (screen-linear); every
   varying is premultiplied by 1/w, which is what makes a linear plane perspective-correct.
   Up to SW_MAX_VARYINGS planes: a full interaction needs 20. 256 bytes. */
#define SW_MAX_VARYINGS 20
#define SW_VAR_PAD      24              /* varying arrays are padded so 8-wide loops need no tail */
typedef struct SwTriAttr {
    float       w[3];
    float       p[SW_MAX_VARYINGS][3];
    float       pad;
} SwTriAttr;

/* One tile's share of one geometry job. `stamp` makes a bin empty: it belongs to the current
   view only when stamp == g_viewStamp (CRenderer: clearing every bin up front was 0.33 ms of
   serial time; lazily, the few percent in use clear themselves in bin_push). */
typedef struct SwBin { int *items; int count, cap; int stamp; } SwBin;

typedef struct SwSegment {
    int         kind;
    int         view;               /* index of its view within the frame */
    int         firstJob, numJobs;
    int         clearSeg;           /* latest segment at or before this one that clears stencil, -1 = none */
    int         testsStencil;       /* a draw of this segment reads the stencil count */
    /* SHADOW segments: the job ranges [lo, hi) of the segments that read what this one counts (those
       after it, up to the next stencil clear, that test the stencil). DOOM 3's pattern needs two
       (local interactions, global interactions); more fall back to "always needed". */
    int         numConsumers;
    int         consumerLo[4], consumerHi[4];
    int         hasClear;
    SwRect      clear;
} SwSegment;

/* a run of at most JOB_TRIS triangles of one draw; the unit of geometry work and the bin row */
typedef struct SwJob { int seg, draw, firstTri, numTris; } SwJob;

typedef struct SwThreadStats {
    uint64_t    blocksVisited, blocksCovered;
    uint64_t    pxDepth, pxStencil, pxColor;
    uint64_t    cellsRejected, cellsFast, triTileRejected, equalFailures, equalInFront;
    uint64_t    kernelCalls, kernelLanes, lightBlocksDark, lightLanesDark, lightCellsDark;
    int         trisIn, trisSetup, trisClipped, trisDropped, binPushes, binSkips;
    uint64_t    tsc[9];             /* SW_PROF_*: cycles by category */
    uint64_t    lightLanes;         /* lanes k_interaction was asked for */
    uint64_t    lightCalls;         /* ... in this many calls */
    uint64_t    shadowBinsCulled, shadowBinsDrawn;
    uint64_t    tscFirst, tscLast;  /* of the running tile pass: entry into this thread's first tile, exit from its last */
    uint64_t    shTris, shTrisWalked, shBlocksVisited, shBlocksCovered, shBlocksWritten, shCellsFast;   /* SW_ORACLES builds */
    uint64_t    ltCallsByFlags[16], ltLanesLit, ltLanesFacing, ltLanesSpec;     /* SW_ORACLES builds */
    uint64_t    stLanes[6], stCalls[6], stWinHits[6], stVertexColorCalls;      /* SW_ORACLES builds */
    char        pad[24];            /* to 640 bytes = ten whole cache lines; the array is 64-byte aligned */
} SwThreadStats;
_Static_assert(sizeof(SwThreadStats) % 64 == 0, "SwThreadStats must be whole cache lines");

/* per-thread block of triangle slots: threads claim 256 at a time, one atomic per 256 triangles */
typedef struct SwSlotCache { int next, end; char pad[56]; } SwSlotCache;

/* ---- frame state ---------------------------------------------------------- */
static int          g_width, g_height;          /* framebuffer size in pixels */
static int          g_tilesX, g_tilesY, g_ntiles;
static uint32_t *   g_fb;                       /* RGBA8, row 0 on top, padded to whole tiles: g_fbs[the selected one] */
static uint32_t *   g_fbs[SW_MAX_FRAMEBUFFERS]; /* all but [0] exist only once selected (sw_framebuffer_select) */
static int          g_fbPitch;                  /* pixels per row: g_tilesX * TILE_SIZE */
static size_t       g_fbBytes;                  /* the region's size: whole tiles, rounded up to 64 KB */

static SwRect       g_view;                     /* the view's viewport */
static float        g_viewCx, g_viewCy, g_viewHw, g_viewHh;
static float        g_guard;                    /* guard band, in units of w */
static int          g_inView;

static SwDraw *     g_draws;    static int g_numDraws, g_capDraws;
static SwSegment *  g_segs;     static int g_numSegs, g_capSegs;
static SwJob *      g_jobs;     static int g_numJobs, g_capJobs;

static SwTri *      g_tris;     static int g_triCap;
static SwTriAttr *  g_triAttrs;                 /* parallel to g_tris; touched only by draws with varyings */
static volatile LONG g_triCount;
static SwSlotCache  g_slot[MAX_THREADS];

static SwBin *      g_bins;     static size_t g_binRows;        /* g_binRows * g_ntiles bins */
static int          g_viewStamp;
static volatile LONG64 *g_tileJobBits;          /* per tile: which jobs have a non-empty bin */
static int          g_jobWords, g_capJobWords;
static int          g_jobStride;                /* words per tile in g_tileJobBits: fixed while a frame's views add jobs */

/* A FRAME is one job list: every view of it appends draws, segments and jobs, runs its own
   geometry phase when it ends (its viewport transform is only valid then, and the engine's vertex
   data only until the caller returns), and ONE tile pass replays all of it when the frame ends.
   A view boundary inside a tile's replay resets depth, stencil and z ranges. */
static int          g_frameOpen, g_frameImplicit;   /* implicit: opened by sw_begin_view, ended by sw_end_view */
static int          g_viewIndex;                    /* views of this frame so far */
static int          g_viewFirstJob, g_viewFirstSeg, g_viewCaptured;
static int          g_setupBase;                    /* job_setup_view: the job index of dispatch job 0 */
static double       g_frameXformMs, g_frameSetupMs;
static uint64_t     g_passStartTsc;                 /* written before a tile pass is dispatched, read once per thread */

/* The tile pass of a frame may run on a coordinator thread (sw_end_frame_async) while the caller goes
   on: from the end of the geometry phases it reads only the core's own memory (triangles, bins,
   draws, the arena, images, the framebuffer). Until it is done nobody else may touch those: every
   entry point that does calls frame_wait() first (I5 again: a read of the frame is a sync point). */
static HANDLE       g_asyncThread, g_asyncGo, g_asyncDone;
static volatile LONG g_asyncBusy, g_asyncQuit;
static DWORD        g_asyncThreadId;
static void       (*g_asyncDoneFn)(void *);
static void *       g_asyncDoneCtx;
static void         frame_wait(void);
static void         async_shutdown(void);

static int *        g_tileOrder;                /* PH_TILES job index -> tile, costliest first */
static int          g_numTileJobs;              /* tiles with anything binned */
static uint64_t *   g_tileSortKeys;

/* FALSE SHARING, measured 2026-09-21: growing g_opt by one int moved g_dbgEqual, which the
   depth-EQUAL path reads per BLOCK, onto a cache line that a thread's counters are written to per
   block, and every light pass cost 2x (1080p, one light: 0.46 -> 0.87 ms) while nothing else
   moved. With static globals the linker decides who shares a line. So:
   - whatever is WRITTEN while the pool runs sits on cache lines of its own (this, the pool's
     counters in core/pool.c);
   - whatever the raster loops READ per triangle or per block is copied into TileCtx, on the
     worker's stack, when a tile starts. g_opt and g_dbgEqual are never read below job_tile. */
static __declspec(align(64)) SwThreadStats g_stats[MAX_THREADS];
static __declspec(align(64)) SwStats g_lastStats;
static int          g_opt[SW_OPT_COUNT] = { 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1, 1 };
static double       g_tscPerMs;                 /* measured in sw_init */

/* the view's capture point (_currentRender): jobs from g_captureJob on run in a second tile phase,
   after the framebuffer inside g_captureRect was copied into g_capture. Tiles that have work on
   both sides keep their depth in g_savedDepth between the phases. */
#define SW_MAX_CAPTURES 4                       /* per frame; one per view */
static int          g_captureJobs[SW_MAX_CAPTURES], g_captureViews[SW_MAX_CAPTURES], g_numCaptures;
static SwRect       g_captureRects[SW_MAX_CAPTURES];
static SwRect       g_captureRect;              /* of the capture the running phase samples (it outlives the frame: a 2D view's
                                                   post-process reads the last 3D view's capture) */
static int          g_phaseView;                /* the view the running phase starts in */
static SwRect       g_captureCopy;              /* the part of g_captureRect job_capture copies */
static SwImage *    g_capture;
static float **     g_savedDepth;               /* per tile, allocated on first use */
static int *        g_savedDepthStamp;          /* == g_viewStamp: valid for this view */
static int          g_phaseLo, g_phaseHi;       /* the job range the running tile phase replays */
static int          g_phaseKeepsDepth;          /* phase A of a captured view */

static float *      g_dbgDepth;                 /* sw_debug_capture targets, NULL = off */
static uint8_t *    g_dbgStencil;
static int          g_dbgEqual;
