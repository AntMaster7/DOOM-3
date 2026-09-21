/* core/config.h -- tile and span shape, fixed-point edge bounds, tunables.
   Lifted from CRenderer's core/config.h and frame/frame.h; the feature switches stayed behind. */
#pragma once
#include "base.h"

#define TILE_SHIFT      6                       /* 64x64 pixel tiles */
#define TILE_SIZE       (1 << TILE_SHIFT)

/* 16x16-pixel cells: the granularity of the hierarchical coverage classification and of the
   tile's z ranges. 4x4 cells per tile, so a cell mask is 16 bits and a cell table one vector. */
#define CELL_SHIFT      4
#define CELL_DIM        (TILE_SIZE >> CELL_SHIFT)
#define CELL_N          (CELL_DIM * CELL_DIM)

/* A vector is a 4x4 block of pixels (CRenderer measured 37-46% fewer kernel calls than 16x1).
   Tile-local depth, stencil and colour are stored SWIZZLED: a block's 16 pixels are contiguous,
   so a block is one aligned load and one store. SPAN_OFF counts ELEMENTS: floats for depth,
   bytes for stencil, uint32 for colour. */
#define SPAN_W          4
#define SPAN_H          4
#define SPAN_BX         (TILE_SIZE / SPAN_W)
#define SPAN_BY         (TILE_SIZE / SPAN_H)
#define SPAN_OFF(bx, by) ((((size_t)(by) * SPAN_BX) + (size_t)(bx)) * 16)
#define TILE_PIXELS     (TILE_SIZE * TILE_SIZE)

#define MAX_THREADS     64

/* ----------------------------------------------------------------------------
   Fixed-point edge functions (CRenderer, 2026-09-06).

   Screen positions snap to a 1/SUBPIX_ONE px grid (cvtss2si, nearest-even; every triangle
   sharing a vertex snaps it to the same integer). Edge gradients are integer differences of
   snapped coordinates, so the two triangles on either side of an edge evaluate EXACT negations
   and the top-left rule hands every pixel centre to exactly one of them (invariant I8). C[k] is
   edge k's value at the centre of the origin pixel, in int64 from the 16.16 product, floored
   back to "A per whole pixel" units with the fill-rule bias inside the shift. The rasterizer
   forms a block corner value in scalar int64, saturates it to +-EDGE_SAT, broadcasts, and adds a
   per-triangle 32-bit lane delta: the sum cannot overflow and its sign is exact while
   |A|+|B| <= EDGE_MAG_MAX. Here the guard-band clip keeps EVERY triangle's screen extent under
   EDGE_EXT_MAX, so CRenderer's out-of-line "wide" edge setup does not exist.
---------------------------------------------------------------------------- */
#define SUBPIX_SHIFT    8
#define SUBPIX_ONE      (1 << SUBPIX_SHIFT)
#define EDGE_SAT        (1 << 30)
#define EDGE_LANE_SPAN  15
#define EDGE_MAG_MAX    ((EDGE_SAT - 1) / EDGE_LANE_SPAN)
#define EDGE_EXT_MAX    ((EDGE_MAG_MAX - 2) / SUBPIX_ONE)  /* about 279k px: bbox extent the fast setup is proven for */

/* A bbox-x-tile walk of at least this many span blocks classifies whole cells first. CRenderer
   A/B'd 24 against 48: a wash. Bit-exact either way. */
#define HIER_MIN_BLOCKS 24

/* one geometry job = at most this many triangles of one draw; bins are keyed by job (I1).
   256, measured on real frames at 4K (2026-09-21, three builds interleaved): a job of 1024
   triangles is about 1 ms of setup that no other thread can help with, and the geometry phase
   cost 1.52 ms mean / 4.2 ms p95; with 256 it is 1.09 / 2.4 (49.1 against 48.3 fps). 64 is better
   still for setup (1.01 / 2.0) but the tile phase then walks more bin rows and loses more. */
#ifndef JOB_TRIS
#define JOB_TRIS        256
#endif

/* the clip keeps w at or above this, so 1/w is finite for directions (w = 0 shadow vertices) */
#define CLIP_W_MIN      1e-6f
