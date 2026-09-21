/* sw_api.h -- the C interface between the engine glue (C++, sees DOOM 3) and the software
   rasterizer core (C, sees only this file). Plain structs, no engine types. The core never
   calls the engine (invariant I7): everything it needs arrives through these calls, and the
   pointers a draw carries stay valid until sw_end_view returns.

   Conventions at this interface (CLAUDE.md has the table):
   - clip space is x, y, z', w with z' = (z_gl + w) / 2, so 0 <= z' <= w and the near test is z' >= 0
   - rectangles are inclusive pixel rectangles, y DOWN, in framebuffer pixels
   - colours are RGBA bytes, r in the low byte of the uint32
   - cull types are DOOM's: a triangle is "front" when it winds CLOCKWISE in OpenGL's y-up window
     space, which is what GL_Cull(CT_FRONT_SIDED) leaves visible */
#ifndef SW_API_H
#define SW_API_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SwRect { int x0, y0, x1, y1; } SwRect;

/* segment kinds: the replay order inside a tile (plan 6.4). A segment is an ordered list of draws. */
enum {
    SW_SEG_DEPTH,       /* depth fill; when it ends the tile's depth is final and its z ranges are built */
    SW_SEG_SHADOW,      /* stencil shadow volumes of one light chain */
    SW_SEG_LIGHT,       /* interactions of one light chain */
    SW_SEG_COLOR        /* ambient stages, 2D, fog, post: anything else that writes colour */
};

/* what a draw does at a covered pixel */
enum {
    SW_OP_DEPTH_FILL,       /* LEQUAL test, writes depth and the draw's colour */
    SW_OP_STENCIL_ZPASS,    /* external shadow volume: depth pass changes the count, front +1, back -1 */
    SW_OP_STENCIL_ZFAIL,    /* view inside the volume: depth fail changes the count, front -1, back +1 */
    SW_OP_COLOR             /* depth test / stencil test / blend as the draw says */
};

enum { SW_DEPTH_ALWAYS, SW_DEPTH_LEQUAL, SW_DEPTH_EQUAL };
enum { SW_CULL_FRONT_SIDED, SW_CULL_BACK_SIDED, SW_CULL_TWO_SIDED };

/* GL blend factors (GL_State, Appendix B of the plan). ONE, ZERO replaces; ZERO, ONE draws nothing. */
enum {
    SW_BF_ZERO, SW_BF_ONE,
    SW_BF_SRC_COLOR, SW_BF_ONE_MINUS_SRC_COLOR,
    SW_BF_DST_COLOR, SW_BF_ONE_MINUS_DST_COLOR,
    SW_BF_SRC_ALPHA, SW_BF_ONE_MINUS_SRC_ALPHA,
    SW_BF_DST_ALPHA, SW_BF_ONE_MINUS_DST_ALPHA,
    SW_BF_SRC_ALPHA_SATURATE
};
/* colour write mask bits: set = the channel IS written (the opposite of GLS_*MASK) */
enum { SW_WRITE_R = 1, SW_WRITE_G = 2, SW_WRITE_B = 4, SW_WRITE_A = 8, SW_WRITE_RGB = 7, SW_WRITE_RGBA = 15,
       SW_WRITE_NONE = 16 };    /* no channel at all (a depth-only stage); 0 in SwDraw.writeMask means RGBA */

/* alpha tests: NONE, the three GLS_ATEST bits, and the depth fill's GL_GREATER against a register */
enum { SW_ATEST_NONE, SW_ATEST_EQ_255, SW_ATEST_LT_128, SW_ATEST_GE_128, SW_ATEST_GT_REF };

/* ---- images ----------------------------------------------------------------
   An SwImage is a mip chain of RGBA8 texels, top row first, as idImage::GenerateImage builds it
   on the CPU before it uploads. Border colours of the zero-clamp modes are baked into the texels
   by the engine, so the core knows two addressing modes only. */
typedef struct SwImage SwImage;
enum { SW_WRAP_REPEAT, SW_WRAP_CLAMP };
/* levels[0] is width x height, each further level halves (never below 1). The texels are copied. */
SwImage *       sw_image_create( int width, int height, int numLevels, const uint8_t *const *levels, int wrap );
/* a cube map: six square chains in GL's face order +X -X +Y -Y +Z -Z (idImage::GenerateCubeImage);
   faces[f][level]. Lookups clamp to the face's edge, as GL without seamless filtering does. */
SwImage *       sw_image_create_cube( int size, int numLevels, const uint8_t *const *const faces[6] );
void            sw_image_destroy( SwImage *image );

/* ---- kernels ----------------------------------------------------------------
   A kernel is a vertex half, run per triangle corner in the geometry phase, and a fragment half,
   run per 4x4 block. The vertex half reads idDrawVert-shaped vertices through SwVertexSource. */
enum {
    SW_KERN_FLAT,           /* the draw's constant colour; no vertex source needed */
    SW_KERN_STAGE,          /* one texture x colour: old-style stages, 2D, the perforated depth fill */
    SW_KERN_INTERACTION,    /* interaction.vfp (plan, Appendix A.1) */
    SW_KERN_DUAL,           /* colour x projective texture x second texture, object-linear texgens: fog, blend lights, TG_SCREEN */
    SW_KERN_ENV,            /* environment.vfp / bumpyEnvironment.vfp */
    SW_KERN_SCREEN          /* heatHaze*.vfp, colorProcess.vfp: reads the view's capture (sw_capture_point) */
};

typedef struct SwVertexSource {
    const void *    base;
    int             stride;
    int             ofsXyz, ofsSt, ofsNormal, ofsTangent0, ofsTangent1, ofsColor;  /* bytes; colour is 4 bytes RGBA */
    const float *   texCoords3;         /* drawSurf->dynamicTexCoords: 3 floats per vertex (skybox, wobblesky), or NULL */
    int             texCoords3Stride;   /* bytes */
} SwVertexSource;

/* where a stage's texture coordinates come from (RB_PrepareStageTexturing) */
enum {
    SW_TG_EXPLICIT,         /* st through the texture matrix */
    SW_TG_DIFFUSE_CUBE,     /* the vertex normal, into a cube map */
    SW_TG_DYNAMIC3          /* SwVertexSource.texCoords3, into a cube map: TG_SKYBOX_CUBE, TG_WOBBLESKY_CUBE */
};

typedef struct SwStageParms {
    const SwImage * image;
    int             texgen;             /* SW_TG_* */
    float           texMatrix[2][4];    /* s = dot((s, t, 0, 1), row 0), t = row 1 */
    float           color[4];           /* the stage's colour registers */
    float           vertexColorModulate, vertexColorAdd;   /* 0/1 ignore, 1/0 modulate, -1/1 inverse */
    int             alphaTest;          /* SW_ATEST_* */
    float           alphaRef;           /* SW_ATEST_GT_REF */
    int             screenAligned;      /* a 2D view: the footprint has no diagonal part, the cheap level rule is exact */
    int             hasClip;            /* a mirror view's clip plane: lanes with dot(clipPlane, (x, y, z, 1)) <= 0 are dropped */
    float           clipPlane[4];       /* in the vertices' space */
} SwStageParms;

/* every coordinate is dot(plane, (x, y, z, 1)) of the object-space vertex */
typedef struct SwDualParms {
    const SwImage * image0;             /* looked up at (s0 / q0, t0 / q0); NULL = white */
    const SwImage * image1;             /* looked up at (s1, t1); NULL = white */
    float           s0[4], t0[4], q0[4], s1[4], t1[4];
    float           color[4];
} SwDualParms;

typedef struct SwEnvParms {
    const SwImage * cube;
    const SwImage * bump;               /* NULL: environment.vfp; else bumpyEnvironment.vfp (st is its coordinate) */
    float           localViewOrigin[3];
    float           modelRows[3][3];    /* bumpy: program.env[6..8].xyz, local to global */
    float           color[4];           /* environment.vfp multiplies by vertex.color: the stage colour ... */
    float           vertexColorModulate;/* ... or, when not 0, the vertex colours as they are */
} SwEnvParms;

enum { SW_SCREEN_HEATHAZE, SW_SCREEN_HEATHAZE_MASK, SW_SCREEN_HEATHAZE_MASK_VERTEX, SW_SCREEN_COLORPROCESS };
typedef struct SwScreenParms {
    int             program;            /* SW_SCREEN_* */
    const SwImage * bump, *mask;
    float           parm0[4], parm1[4]; /* program.local[0] and [1]: scroll and deform magnitude; colorProcess: fraction and target */
    float           mvRow2[4];          /* state.matrix.modelview.row[2] */
    float           projRow0[4], projRow3[4];
} SwScreenParms;

typedef struct SwInteractionParms {
    float           localLightOrigin[3], localViewOrigin[3];
    float           lightProjectS[4], lightProjectT[4], lightProjectQ[4], lightFalloffS[4];
    float           bumpMatrix[2][4], diffuseMatrix[2][4], specularMatrix[2][4];
    float           vertexColorModulate, vertexColorAdd;
    float           diffuseColor[4], specularColor[4];      /* fragment env 0 and 1 */
    const SwImage * bump, *diffuse, *specular, *falloff, *projection;
    /* The specular term is max(4 N.H - 3, 0)^2, clamped to this. interaction.vfp reads it from a
       256-texel table, which ends at 1. DOOM 3's normal maps are not renormalized, so N.H exceeds
       1 on strong highlights, and there the two differ by up to 2x: see CLAUDE.md, "the specular
       table". 0 means 1. */
    float           specularMax;
    int             ambientLight;       /* the light vector is a constant instead of normalize(tc0) */
    float           ambientVector[3];
} SwInteractionParms;

typedef struct SwDraw {
    const float *   clip;           /* x, y, z', w per vertex */
    int             clipStride;     /* bytes between vertices */
    const int *     indexes;        /* triangle list */
    int             numIndexes;

    int             op;             /* SW_OP_* */
    int             cull;           /* SW_CULL_*; ignored by the stencil ops, which need both sides */
    int             mirror;         /* the view is a mirror: front and back swap */
    SwRect          scissor;        /* limits the walked rectangle only, never the plane origin (I3) */

    float           offsetFactor;   /* polygon offset: z += factor * max|dz| + units * 2^-24 */
    float           offsetUnits;
    float           depthRangeMax;  /* glDepthRange(0, this); 0 means 1 */
    /* the stencil ops: GL_EXT_depth_bounds_test. A pixel whose STORED depth lies outside
       [min, max] is left alone. max <= min means off. */
    float           depthBoundsMin, depthBoundsMax;

    /* SW_OP_COLOR and SW_OP_DEPTH_FILL */
    uint32_t        color;          /* SW_KERN_FLAT's colour; what the depth fill writes (black) */
    int             kernel;         /* SW_KERN_*. The depth fill takes SW_KERN_STAGE for its alpha test only. */
    const void *    kernelParms;    /* SwStageParms / SwInteractionParms; valid until sw_end_view */
    SwVertexSource  verts;          /* same vertex order as `clip` */
    /* SW_OP_COLOR */
    int             depthTest;      /* SW_DEPTH_* */
    int             depthWrite;
    int             stencilTest;    /* pass where the stored count is at most 128 (GL_GEQUAL, 128) */
    int             srcBlend, dstBlend;     /* SW_BF_* */
    int             writeMask;      /* SW_WRITE_*; 0 means RGBA */
} SwDraw;

/* ---- transforms ---------------------------------------------------------------
   Object space to the clip space of this interface, run in the pool when sw_end_view starts.
   The returned block (x, y, z', w per vertex, 16 bytes apart) is what SwDraw.clip points at; it is
   valid, like everything from sw_view_alloc, until the next sw_begin_view. Ask ONCE per (vertex
   array, space, projection variant) and hand the same pointer to every pass (invariant I2). */
enum {
    SW_XF_POINTS,           /* 3 floats per vertex, w = 1 */
    SW_XF_SHADOW            /* 4 floats per vertex (shadowCache_t); shadow.vp: w = 0 vertices leave for infinity, away from the light */
};
typedef struct SwXform {
    const void *    positions;
    int             stride;         /* bytes */
    int             numVerts;
    int             kind;           /* SW_XF_* */
    float           mvp[16];        /* OpenGL's: column-major, clip z in -w..w; the core applies z' = (z + w) / 2 */
    float           lightOrigin[3]; /* SW_XF_SHADOW: the light in the vertices' space */
} SwXform;
const float *   sw_transform( const SwXform *xform );   /* NULL when the view arena is full */
void *          sw_view_alloc( size_t bytes );          /* 64-byte aligned scratch for draw parameters; NULL when full */

enum { SW_PROF_LOAD, SW_PROF_DEPTH, SW_PROF_ZRANGE, SW_PROF_SHADOW, SW_PROF_LIGHT, SW_PROF_COLOR, SW_PROF_STORE,
       SW_PROF_SETUP,   /* not of the tile pass: the geometry jobs' thread time, to set against the phase's wall time */
       SW_PROF_BUSY,    /* a thread inside job_tile, empty tiles included: against the pass's wall time it gives the idle share */
       SW_PROF_COUNT };

typedef struct SwStats {
    double      xformMs;                    /* the vertex transforms, first pool phase of sw_end_view */
    double      setupMs, tilesMs;           /* wall time of the two pool phases of the last view */
    double      sortMs;                     /* the serial tile ordering between them */
    int         draws, jobs, trisIn, trisSetup, trisClipped, trisDropped;
    int         binPushes;
    /* summed over threads for the last view */
    uint64_t    blocksVisited, blocksCovered;
    uint64_t    pxDepth, pxStencil, pxColor;    /* lanes written, per op class */
    uint64_t    kernelCalls, kernelLanes;       /* fragment kernel invocations and the lanes they were asked for */
    uint64_t    lightCellsDark;                 /* 16x16 cells skipped as outside the light volume */
    uint64_t    lightBlocksDark, lightLanesDark; /* k_interaction: blocks / lanes dropped by its exact early-outs */
    uint64_t    lightLanes;                     /* lanes k_interaction was asked for (after depth and stencil tests) */
    uint64_t    lightCalls;                     /* ... in this many calls: lanes / calls of 16 is how full a block is */
    uint64_t    cellsRejected;                  /* 16x16 cells skipped on their z range */
    uint64_t    cellsFast;                      /* 16x16 cells settled without a per-block walk */
    /* shadow volumes only, counted in SW_ORACLES builds: triangles handed to raster_tri per tile, those the
       (triangle, tile) z test let through, 4x4 blocks the walk visited / that had coverage / that changed a count */
    uint64_t    shTris, shTrisWalked, shBlocksVisited, shBlocksCovered, shBlocksWritten, shCellsFast;
    /* SW_ORACLES builds: light kernel calls by variant (bit 0 specular, 1 vertex colour, 2 diffuse shares the bump
       map's texel coordinates, 3 specular does), and how far the lanes got: past the light's two textures, past N.L,
       lanes that fetched the specular map */
    uint64_t    ltCallsByFlags[16], ltLanesLit, ltLanesFacing, ltLanesSpec;
    uint64_t    texWin[8];                      /* SW_ORACLES builds: window fetch tries, hits, misses by reason (wrap, columns, upper rows, lower rows, levels, upper rows that 8 rows would hold) */
    /* SW_ORACLES builds: the stage kernel's colour work by class = (3D view ? 3 : 0) + (0 replace, 1 add, 2 any other
       blend): lanes asked, calls, calls whose lookup went through the texel window; and calls with a vertex colour */
    uint64_t    stLanes[6], stCalls[6], stWinHits[6], stVertexColorCalls;
    uint64_t    binSkips;                       /* (triangle, tile) pairs SW_OPT_BIN_EXACT left out; binPushes = those binned */
    uint64_t    shadowBinsCulled, shadowBinsDrawn; /* SW_OPT_SHADOW_CULL: (shadow job, tile) bins skipped / replayed */
    uint64_t    triTileRejected;                /* (triangle, tile) pairs skipped on the tile's z range */
    /* SW_OPT_PROFILE: thread-summed time inside the tile phase, by category, divided by the thread
       count: what each category would cost in wall time if the threads were evenly loaded */
    double      tileMs[SW_PROF_COUNT];
    double      captureMs;                  /* the capture points' framebuffer copies, inside tilesMs */
    double      captureMpx;                 /* ... and how much they copied */
    double      lateMs, earlyMs;            /* SW_OPT_PROFILE: per thread on average, from the tile pass's start to its first tile, and from its last tile to the pass's end */
    uint64_t    equalFailures;                  /* I2 counter: see sw_debug_count_equal_failures */
    uint64_t    equalInFront;                   /* ... of those, the ones CLOSER than the stored depth: in a real frame hidden light
                                                   fragments fail EQUAL legitimately, a fragment in front of the depth fill never does */
} SwStats;

/* options for same-binary A/Bs; all default to 1 */
enum {
    SW_OPT_HIER,            /* hierarchical cell classification of big walks */
    SW_OPT_ZRANGE,          /* z-range rejects against the tile's cell zmin/zmax */
    SW_OPT_CELL_FAST,       /* whole-cell stencil updates without a block walk */
    SW_OPT_LIGHT_CELLS,     /* k_interaction: 16x16 cells proven outside the light volume skip their blocks */
    SW_OPT_KERNEL_CUT,      /* ORACLE (builds with /DSW_ORACLES=1 only), wrong image, right cost: k_interaction
                               returns dark after stage N (1 = at entry, 2 = after falloff, 3 = after
                               projection, 4 = after bump and N.L); 0 = off */
    SW_OPT_PROFILE,         /* time the tile phase by category (two rdtsc per bin); default 0 */
    SW_OPT_DEPTH_BOUNDS,    /* honour SwDraw.depthBoundsMin/Max; default 1 */
    SW_OPT_SHADOW_CULL,     /* a tile skips the shadow volumes whose counts nothing in it reads; default 1 */
    SW_OPT_TEX_LEVEL0,      /* a block proven to read level 0 everywhere skips the level rule and the per-lane level tables (exact); default 1 */
    SW_OPT_TEX_WINDOW,      /* a block whose texels lie in a 16 x 5 window of one level fetches them with row loads, not gathers (exact); default 1 */
    SW_OPT_BIN_EXACT,       /* a triangle goes only into the tiles it can cover, not into all of its bounding box (exact); default 1 */
    SW_OPT_CAPTURE_REGION,  /* a capture point copies only what its readers can reach (exact; 0 = the whole rectangle); default 1 */
    SW_OPT_COUNT
};

/* Returns 0 when the CPU lacks AVX-512 F/BW/DQ/VL; the caller then keeps its other renderer.
   threads = pool size including the calling thread, 0 = logical processors. */
int             sw_init( int threads );
void            sw_shutdown( void );
int             sw_num_threads( void );
void            sw_set_option( int option, int value );

void            sw_resize( int width, int height );
void            sw_clear_framebuffer( uint32_t rgba );
const uint32_t *sw_framebuffer( int *pitchPixels );     /* row 0 is the TOP row */
/* The framebuffer is ONE VirtualAlloc region of whole tiles (pitch a multiple of 64 pixels, size a
   multiple of 64 KB), so a presenter may hand the region itself to the GPU instead of copying it.
   It moves only in sw_resize. */
size_t          sw_framebuffer_bytes( void );
/* Up to SW_MAX_FRAMEBUFFERS regions of that kind; views draw into the selected one and every read
   sees it. For a presenter whose GPU reads frame N in place while frame N + 1 is rendered. Returns
   the selected framebuffer; not inside a view. Regions move only in sw_resize. */
#define SW_MAX_FRAMEBUFFERS 3
const uint32_t *sw_framebuffer_select( int index );
/* the finished frame into memory the caller owns, in the pool; not inside a view */
void            sw_copy_frame( void *dst, int pitchPixels );

/* A FRAME: the views between sw_begin_frame and sw_end_frame share ONE tile pass, run by
   sw_end_frame: every tile is loaded and stored once, and the pass is one piece of work that does
   not read the caller's memory any more. Each view's geometry still runs in its sw_end_view, so
   vertex and index data need to live only that long; kernel parameters, images and everything
   from sw_view_alloc must live until sw_end_frame returns. A view boundary resets depth and
   stencil, as a new GL view does. Anything that READS the frame in between (a copy-render) ends
   the frame first and begins another. Without a frame, a view is a frame of its own. */
void            sw_begin_frame( void );
void            sw_end_frame( void );
/* The same, but the tile pass runs on the core's coordinator thread while the caller goes on with
   ITS next frame; `done( ctx )` is called on that thread when the frame is finished (a presenter:
   it must not call back into the caller's world, I7). Every entry point of this interface that
   reads or changes what the pass uses waits for it first: the framebuffer reads, sw_begin_frame,
   sw_resize, sw_set_option, sw_image_destroy, sw_get_stats, sw_shutdown. sw_wait does only that. */
void            sw_end_frame_async( void (*done)( void *ctx ), void *ctx );
void            sw_wait( void );
void            sw_begin_view( const SwRect *viewport );
/* stencilClear: set the count to 128 inside this rectangle when the segment starts (the first
   segment of a light that casts shadows); NULL for none */
void            sw_begin_segment( int kind, const SwRect *stencilClear );
void            sw_draw( const SwDraw *draw );
/* The view's capture point (_currentRender): everything submitted so far is rendered and the
   framebuffer inside `rect` is copied; SW_KERN_SCREEN draws submitted afterwards sample that
   copy. Depth survives the point. One per view (the GL back end copies once per view too). */
void            sw_capture_point( const SwRect *rect );
void            sw_end_view( void );                    /* runs the view's geometry; outside a frame, the tiles too */
/* copies framebuffer pixels (row 0 on top) into `rgba`, `pitch` pixels per destination row */
void            sw_read_pixels( const SwRect *rect, uint32_t *rgba, int pitch );

void            sw_get_stats( SwStats *out );

/* debug: full-frame depth and stencil of the last view, filled while the tiles store. Either
   pointer may be NULL. The buffers are width * height, row 0 on top. */
void            sw_debug_capture( float *depth, uint8_t *stencil );
/* debug: count, in SW_DEPTH_EQUAL draws, covered pixels whose depth is NOT equal to the stored
   one. With the same geometry drawn in the depth fill it must stay 0 (invariant I2). */
void            sw_debug_count_equal_failures( int enable );

#ifdef __cplusplus
}
#endif

#endif
