/* rast/raster.c -- raster_tri, the block rasterizer, in DOOM 3's raster ops.
   From CRenderer's frame/raster.c: the 4x4 block walk, the exact integer edge test, the
   hierarchical classification of 16x16 cells. New here: the ops (depth fill, the two stencil
   counts, colour with depth/stencil test), the triangle's depth clamped on BOTH sides, z-range
   rejects against per-cell zmin AND zmax, and whole-cell stencil updates. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

typedef struct TileCtx {
    int             tx0, ty0;           /* the tile's top-left pixel */
    float *         tz;                 /* swizzled tile-local depth, stencil, colour */
    uint8_t *       ts;
    uint32_t *      tc;
    int             zrange;             /* cellZmin/cellZmax are valid: depth is final (I4) */
    int             optHier, optZrange, optCellFast, optLightCells, kernelCut, dbgEqual;  /* per-tile copies of the switches */
    int             optProfile, optDepthBounds;
    /* the depth bounds of the stencil draw being replayed (job_tile sets them per bin) */
    int             hasBounds;
    float           boundsMin, boundsMax;
    unsigned        boundsCells;        /* cells whose stored z range reaches into the bounds */
    unsigned        boundsFull;         /* cells whose stored z range lies inside them: no per-pixel test needed for the fast path */
    const SwImage * capture;            /* the view's capture, for SW_KERN_SCREEN */
    SwRect          captureRect;
    __declspec(align(64)) float cellZmin[CELL_N];
    __declspec(align(64)) float cellZmax[CELL_N];
    SwThreadStats * st;
} TileCtx;

/* THE depth evaluation (invariant I2): every op gets a block's depth from here, and from nowhere
   else. Two explicit fused multiply-adds leave the compiler nothing to contract or regroup, and
   the pragmas pin that down: /fp:fast otherwise rewrites mul/add trees per inlined copy, and the
   depth-EQUAL passes need the bits the depth fill stored. Clamped to the triangle's own vertex
   range on both sides: snapped edges admit centres up to 1/512 px outside the float triangle,
   where the plane extrapolates. */
#pragma float_control(precise, on, push)
#pragma fp_contract(off)
static __forceinline VF tri_depth(VF zgx, VF zgy, VF zc, VF X, VF Y, VF zmin, VF zmax)
{
    const VF z = _mm512_fmadd_ps(zgy, Y, _mm512_fmadd_ps(zgx, X, zc));
    return _mm512_min_ps(_mm512_max_ps(z, zmin), zmax);
}
#pragma float_control(pop)

/* bitmask (bit = cy * 4 + cx) of the cells a pixel rectangle touches in this tile */
static __forceinline unsigned rect_cells(int minx, int miny, int maxx, int maxy, int tx0, int ty0)
{
    const int cx0 = (minx - tx0) >> CELL_SHIFT, cy0 = (miny - ty0) >> CELL_SHIFT;
    const int cx1 = (maxx - tx0) >> CELL_SHIFT, cy1 = (maxy - ty0) >> CELL_SHIFT;
    const unsigned rowm = (2u << cx1) - (1u << cx0);
    const unsigned rows = (0x1111u << (cy0 * CELL_DIM)) & ((0x10u << (cy1 * CELL_DIM)) - 1u);
    return rowm * rows;
}

/* add a signed delta to the stencil count of one whole cell: 4 block rows of 64 bytes */
static __forceinline void stencil_add_cell(uint8_t *ts, int cx, int cy, int delta)
{
    const VI dv = _mm512_set1_epi8((char)delta);
    for (int r = 0; r < 4; r++) {
        uint8_t *p = ts + SPAN_OFF(cx * 4, cy * 4 + r);
        _mm512_store_si512((void *)p, _mm512_add_epi8(_mm512_load_si512((const void *)p), dv));
    }
}

/* Rasterize one triangle into one tile. op, dtest and kern are compile-time constants at every
   call site, so each combination is its own straight-line loop. slot indexes g_triAttrs. */
static __forceinline void raster_tri(const SwTri *t, const int slot, const SwDraw *d, TileCtx *c,
                                     const int op, const int dtest, const int kern, const int iflags)
{
    SwThreadStats *st = c->st;
    const int tx0 = c->tx0, ty0 = c->ty0;
    const int minx = t->minx > tx0 ? t->minx : tx0;
    const int maxx = t->maxx < tx0 + TILE_SIZE - 1 ? t->maxx : tx0 + TILE_SIZE - 1;
    const int miny = t->miny > ty0 ? t->miny : ty0;
    const int maxy = t->maxy < ty0 + TILE_SIZE - 1 ? t->maxy : ty0 + TILE_SIZE - 1;
    if (minx > maxx || miny > maxy) return;
#if SW_ORACLES              /* 12 = a colour stage's triangles cost their bin entry and nothing else; 22 = the same for shadow volumes */
    if (op == SW_OP_COLOR && kern == SW_KERN_STAGE && c->kernelCut == 12) return;
    const int isShadow = op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL;
    if (isShadow) { st->shTris++; if (c->kernelCut == 22) return; }
#endif
    const int ox = t->ox, oy = t->oy;

    /* does this op depend on the stored depth at all, and may it use the tile's z ranges? */
    const int zr = c->zrange && c->optZrange && op != SW_OP_DEPTH_FILL && dtest != SW_DEPTH_ALWAYS;
    if (zr) {
        /* (triangle, tile) reject on the cells the walk touches. Exact: written depth stays
           inside [minz, maxz]. ZFAIL changes the count only where the test FAILS, so it is the
           all-pass case that is a no-op there. */
        const __mmask16 cells = (__mmask16)rect_cells(minx, miny, maxx, maxy, tx0, ty0);
        if ((op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL) && c->hasBounds && !(cells & c->boundsCells)) {
            st->triTileRejected++; return;                  /* every touched cell is outside the depth bounds */
        }
        if (op == SW_OP_STENCIL_ZFAIL) {
            const float lo = vmin16(_mm512_mask_mov_ps(_mm512_set1_ps(2.0f), cells, _mm512_load_ps(c->cellZmin)));
            if (t->maxz <= lo) { st->triTileRejected++; return; }
        } else {
            const float hi = vmax16(_mm512_mask_mov_ps(_mm512_set1_ps(-1.0f), cells, _mm512_load_ps(c->cellZmax)));
            if (t->minz > hi) { st->triTileRejected++; return; }
            if (dtest == SW_DEPTH_EQUAL) {
                const float lo = vmin16(_mm512_mask_mov_ps(_mm512_set1_ps(2.0f), cells, _mm512_load_ps(c->cellZmin)));
                if (t->maxz < lo) { st->triTileRejected++; return; }
            }
        }
    }

#if SW_ORACLES
    if (isShadow) st->shTrisWalked++;
#endif
    const int bx0 = (minx - tx0) / SPAN_W, bx1 = (maxx - tx0) / SPAN_W;
    const int by0 = (miny - ty0) / SPAN_H, by1 = (maxy - ty0) / SPAN_H;

    /* stencil: what a lane that changes the count adds. ZPASS: front +1, back -1. ZFAIL: the sum
       of the preload pair and the depth-pass pair of RB_T_Shadow: front -1, back +1. */
    const int sdelta = (op == SW_OP_STENCIL_ZPASS) ? (t->back ? -1 : 1) : (t->back ? 1 : -1);

    /* Hierarchical classification of big walks (CRenderer): a cell whose best corner fails an
       edge is EMPTY, a cell whose worst corner clears every edge is FULL. Exact, because the
       edges are integer and linear. On top of it, per cell: the triangle's depth range over the
       cell against the cell's stored range, which rejects the cell or, for the stencil ops on a
       FULL cell inside the walk, settles all 256 pixels with four byte-vector adds. */
    unsigned cellFull = 0, cellSkip = 0;
    unsigned char rowSkip[4] = { 0, 0, 0, 0 };
    int hier = 0;
    const SwTriAttr *const attrH = &g_triAttrs[slot];
    const int falloffZero = kern == SW_KERN_INTERACTION && ((const SwInteractionParms *)d->kernelParms)->falloff->zeroBorder;
    const int projZero = kern == SW_KERN_INTERACTION && ((const SwInteractionParms *)d->kernelParms)->projection->zeroBorder;
    const int lightCells = c->optLightCells && (falloffZero || projZero);
    if (c->optHier && (bx1 - bx0 + 1) * (by1 - by0 + 1) >= HIER_MIN_BLOCKS) {
        const int cx0 = bx0 >> 2, cx1 = bx1 >> 2, cy0 = by0 >> 2, cy1 = by1 >> 2;
        int64_t aMin[3][4], aMax[3][4], bMin[3][4], bMax[3][4];
        for (int k = 0; k < 3; k++) {
            const int64_t A = t->A[k], B = t->B[k];
            const int64_t a15 = A * 15, a16 = A * 16;
            const int64_t aLo = a15 < 0 ? a15 : 0, aHi = a15 < 0 ? 0 : a15;
            int64_t va = A * (int64_t)(tx0 + cx0 * 16 - ox);
#pragma loop(no_vector)
            for (int cx = cx0; cx <= cx1; cx++, va += a16) { aMin[k][cx] = va + aLo; aMax[k][cx] = va + aHi; }
            const int64_t b15 = B * 15, b16 = B * 16;
            const int64_t bLo = (b15 < 0 ? b15 : 0) + t->C[k], bHi = (b15 < 0 ? 0 : b15) + t->C[k];
            int64_t vb = B * (int64_t)(ty0 + cy0 * 16 - oy);
#pragma loop(no_vector)
            for (int cy = cy0; cy <= cy1; cy++, vb += b16) { bMin[k][cy] = vb + bLo; bMax[k][cy] = vb + bHi; }
        }
        /* slack for the scalar depth bounds below against the vector evaluation's rounding */
        const float zmag = (fabsf(t->zc) + fabsf(t->zgx) * (float)(TILE_SIZE + tx0 - ox) + fabsf(t->zgy) * (float)(TILE_SIZE + ty0 - oy)) * 2e-6f;
        for (int cy = cy0; cy <= cy1; cy++) {
            unsigned rowAll = 1;
            for (int cx = cx0; cx <= cx1; cx++) {
                const unsigned ci = (unsigned)(cy * 4 + cx), bit = 1u << ci;
                int full = 1, empty = 0;
                for (int k = 0; k < 3; k++) {
                    const int64_t lo = aMin[k][cx] + bMin[k][cy], hi = aMax[k][cx] + bMax[k][cy];
                    if (hi < 0) { empty = 1; break; }
                    if (lo < 0) full = 0;
                }
                if (empty) { cellSkip |= bit; continue; }
                if (kern == SW_KERN_INTERACTION && lightCells &&
                    light_cell_dark(attrH, (float)(tx0 + cx * 16 - ox) + 0.5f, (float)(ty0 + cy * 16 - oy) + 0.5f, falloffZero, projZero)) {
                    cellSkip |= bit; st->lightCellsDark++; continue;
                }
                const int stencilOp = op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL;
                if (stencilOp && zr && c->hasBounds && !((c->boundsCells >> ci) & 1)) { cellSkip |= bit; st->cellsRejected++; continue; }
                const int cellInBounds = !stencilOp || !c->hasBounds || ((c->boundsFull >> ci) & 1);
                if (zr) {
                    /* the plane's extremes over the cell's pixel centres sit at its corners */
                    const float xl = (float)(tx0 + cx * 16 - ox) + 0.5f, yl = (float)(ty0 + cy * 16 - oy) + 0.5f;
                    const float gx0 = t->zgx * xl, gx1 = t->zgx * (xl + 15.0f);
                    const float gy0 = t->zgy * yl, gy1 = t->zgy * (yl + 15.0f);
                    float zlo = t->zc + (gx0 < gx1 ? gx0 : gx1) + (gy0 < gy1 ? gy0 : gy1) - zmag;
                    float zhi = t->zc + (gx0 > gx1 ? gx0 : gx1) + (gy0 > gy1 ? gy0 : gy1) + zmag;
                    if (!(zlo > t->minz)) zlo = t->minz;    /* NaN planes fall back to the vertex range */
                    if (!(zhi < t->maxz)) zhi = t->maxz;
                    const float czmin = c->cellZmin[ci], czmax = c->cellZmax[ci];
                    const int inWalk = tx0 + cx * 16 >= minx && tx0 + cx * 16 + 15 <= maxx &&
                                       ty0 + cy * 16 >= miny && ty0 + cy * 16 + 15 <= maxy;
                    if (op == SW_OP_STENCIL_ZPASS) {
                        if (zlo > czmax) { cellSkip |= bit; st->cellsRejected++; continue; }     /* fails everywhere */
                        if (full && inWalk && zhi <= czmin && c->optCellFast && cellInBounds) {  /* passes everywhere */
                            stencil_add_cell(c->ts, cx, cy, sdelta);
                            cellSkip |= bit; st->cellsFast++; st->pxStencil += 256; continue;   /* (shCellsFast = cellsFast: only stencil ops get here) */
                        }
                    } else if (op == SW_OP_STENCIL_ZFAIL) {
                        if (zhi <= czmin) { cellSkip |= bit; st->cellsRejected++; continue; }    /* passes everywhere: no change */
                        if (full && inWalk && zlo > czmax && c->optCellFast && cellInBounds) {   /* fails everywhere */
                            stencil_add_cell(c->ts, cx, cy, sdelta);
                            cellSkip |= bit; st->cellsFast++; st->pxStencil += 256; continue;   /* (shCellsFast = cellsFast: only stencil ops get here) */
                        }
                    } else {
                        if (zlo > czmax || (dtest == SW_DEPTH_EQUAL && zhi < czmin)) { cellSkip |= bit; st->cellsRejected++; continue; }
                    }
                }
                rowAll = 0;
                if (full) cellFull |= bit;
            }
            rowSkip[cy] = (unsigned char)rowAll;
        }
        hier = 1;
    }

    const VF lx = _mm512_load_ps(SPAN_LX), ly = _mm512_load_ps(SPAN_LY);
    const VI lxi = _mm512_load_si512((const void *)SPAN_LXI), lyi = _mm512_load_si512((const void *)SPAN_LYI);
    const VI dl0 = _mm512_add_epi32(_mm512_mullo_epi32(_mm512_set1_epi32(t->A[0]), lxi), _mm512_mullo_epi32(_mm512_set1_epi32(t->B[0]), lyi));
    const VI dl1 = _mm512_add_epi32(_mm512_mullo_epi32(_mm512_set1_epi32(t->A[1]), lxi), _mm512_mullo_epi32(_mm512_set1_epi32(t->B[1]), lyi));
    const VI dl2 = _mm512_add_epi32(_mm512_mullo_epi32(_mm512_set1_epi32(t->A[2]), lxi), _mm512_mullo_epi32(_mm512_set1_epi32(t->B[2]), lyi));
    const int64_t A0 = t->A[0], A1 = t->A[1], A2 = t->A[2];
    const int64_t B0 = t->B[0], B1 = t->B[1], B2 = t->B[2];
    const VI izero = _mm512_setzero_si512();
    const VF vZx = _mm512_set1_ps(t->zgx), vZy = _mm512_set1_ps(t->zgy), vZc = _mm512_set1_ps(t->zc);
    const VF vZmin = _mm512_set1_ps(t->minz), vZmax = _mm512_set1_ps(t->maxz);
    const unsigned xmFirst = SPAN_XREP(SPAN_RUN(minx - (tx0 + bx0 * SPAN_W), SPAN_W - 1));
    const unsigned xmLast  = SPAN_XREP(SPAN_RUN(0, maxx - (tx0 + bx1 * SPAN_W)));

    float *const tz = c->tz;
    uint8_t *const ts = c->ts;
    uint32_t *const tcol = c->tc;
    const VI vColor = _mm512_set1_epi32((int)d->color);
    const __m128i vDelta = _mm_set1_epi8((char)sdelta);
    const int stencilTest = d->stencilTest, depthWrite = d->depthWrite, dbgEqual = c->dbgEqual, kernelCut = c->kernelCut;
    const BlendState bstate = blend_state(d);
    const SwTriAttr *const attr = &g_triAttrs[slot];
    const SwStageParms *const sparms = (const SwStageParms *)d->kernelParms;
    const SwInteractionParms *const iparms = (const SwInteractionParms *)d->kernelParms;
    const int fillColor = d->writeMask != SW_WRITE_NONE;   /* a subview's depth fill leaves the colour alone */
    const VF vBoundsMin = _mm512_set1_ps(c->hasBounds ? c->boundsMin : -3.0e38f), vBoundsMax = _mm512_set1_ps(c->hasBounds ? c->boundsMax : 3.0e38f);

    for (int by = by0; by <= by1; by++) {
        if (hier && rowSkip[by >> 2]) { by |= 3; continue; }
        const int py0 = ty0 + by * SPAN_H;
        const int loy = miny > py0 ? miny - py0 : 0;
        const int hiy = maxy < py0 + SPAN_H - 1 ? maxy - py0 : SPAN_H - 1;
        const unsigned ym = SPAN_YEXP[SPAN_RUN(loy, hiy)];
        const size_t rowOff = SPAN_OFF(0, by);
        const VF Y = _mm512_add_ps(_mm512_set1_ps((float)(py0 - oy) + 0.5f), ly);
        const int64_t yoff = py0 - oy;
        const int64_t rowC0 = t->C[0] + B0 * yoff, rowC1 = t->C[1] + B1 * yoff, rowC2 = t->C[2] + B2 * yoff;
        for (int bx = bx0; bx <= bx1; bx++) {
            int hAcc = 0;
            if (hier) {
                const unsigned ci = (unsigned)(((by >> 2) << 2) | (bx >> 2));
                if ((cellSkip >> ci) & 1) { bx |= 3; continue; }
                hAcc = (int)((cellFull >> ci) & 1);
            }
            const int px0 = tx0 + bx * SPAN_W;
            unsigned xm = ym;
            if (bx == bx0) xm &= xmFirst;
            if (bx == bx1) xm &= xmLast;
            const __mmask16 bbm = (__mmask16)xm;
            const size_t blk = rowOff + (size_t)bx * 16;

            __mmask16 mk;
            if (hAcc) mk = bbm;
            else {
                const int64_t xoff = px0 - ox;
                int64_t v0 = rowC0 + A0 * xoff, v1 = rowC1 + A1 * xoff, v2 = rowC2 + A2 * xoff;
                v0 = v0 > EDGE_SAT ? EDGE_SAT : (v0 < -EDGE_SAT ? -EDGE_SAT : v0);
                v1 = v1 > EDGE_SAT ? EDGE_SAT : (v1 < -EDGE_SAT ? -EDGE_SAT : v1);
                v2 = v2 > EDGE_SAT ? EDGE_SAT : (v2 < -EDGE_SAT ? -EDGE_SAT : v2);
                mk = _mm512_mask_cmpge_epi32_mask(bbm, _mm512_add_epi32(_mm512_set1_epi32((int32_t)v0), dl0), izero);
                mk = _mm512_mask_cmpge_epi32_mask(mk, _mm512_add_epi32(_mm512_set1_epi32((int32_t)v1), dl1), izero);
                mk = _mm512_mask_cmpge_epi32_mask(mk, _mm512_add_epi32(_mm512_set1_epi32((int32_t)v2), dl2), izero);
            }
            st->blocksVisited++;
#if SW_ORACLES
            if (isShadow) st->shBlocksVisited++;
#endif
            if (!mk) continue;
            st->blocksCovered++;
#if SW_ORACLES              /* 21 = a shadow volume's walk and edge tests, but no depth test and no count */
            if (isShadow) { st->shBlocksCovered++; if (kernelCut == 21) continue; }
#endif

            const VF X = _mm512_add_ps(_mm512_set1_ps((float)(px0 - ox) + 0.5f), lx);
            const VF zs = tri_depth(vZx, vZy, vZc, X, Y, vZmin, vZmax);

            if (op == SW_OP_DEPTH_FILL) {
                __mmask16 cov = _mm512_mask_cmp_ps_mask(mk, zs, _mm512_load_ps(tz + blk), _CMP_LE_OQ);
                if (!cov) continue;
                if (kern == SW_KERN_STAGE) {                /* perforated: the stage alpha decides who writes depth */
                    VI unused;
                    st->kernelCalls++; st->kernelLanes += (unsigned)_mm_popcnt_u32(cov);
                    cov = k_stage(attr, sparms, cov, X, Y, &unused);
                    if (!cov) continue;
                }
                _mm512_mask_store_ps(tz + blk, cov, zs);
                if (fillColor) _mm512_mask_store_epi32(tcol + blk, cov, vColor);
                st->pxDepth += (unsigned)_mm_popcnt_u32(cov);
            } else if (op == SW_OP_STENCIL_ZPASS || op == SW_OP_STENCIL_ZFAIL) {
                const VF stored = _mm512_load_ps(tz + blk);
                const unsigned covZ = (unsigned)_mm512_mask_cmp_ps_mask(mk, zs, stored, op == SW_OP_STENCIL_ZPASS ? _CMP_LE_OQ : _CMP_NLE_UQ);
                /* GL_EXT_depth_bounds_test: on the STORED depth, inclusive; the bounds are infinite when the
                   test is off. Written as two UNMASKED compares combined as integers: a chain of masked float
                   compares here (or a mask changed under a run-time `if`) is an internal compiler error in
                   MSVC's optimizer (C1001, toolset 14.51; bisected 2026-09-21). */
                const unsigned inB = (unsigned)_mm512_cmp_ps_mask(stored, vBoundsMin, _CMP_GE_OQ) & (unsigned)_mm512_cmp_ps_mask(stored, vBoundsMax, _CMP_LE_OQ);
                const __mmask16 cov = (__mmask16)(covZ & inB);
                if (!cov) continue;
                const __m128i s = _mm_load_si128((const __m128i *)(ts + blk));
                _mm_store_si128((__m128i *)(ts + blk), _mm_mask_add_epi8(s, cov, s, vDelta));   /* wraps, as GL_INCR_WRAP */
                st->pxStencil += (unsigned)_mm_popcnt_u32(cov);
#if SW_ORACLES
                st->shBlocksWritten++;
#endif
            } else {
                __mmask16 cov = mk;
                if (dtest == SW_DEPTH_LEQUAL) cov = _mm512_mask_cmp_ps_mask(mk, zs, _mm512_load_ps(tz + blk), _CMP_LE_OQ);
                else if (dtest == SW_DEPTH_EQUAL) {
                    cov = _mm512_mask_cmp_ps_mask(mk, zs, _mm512_load_ps(tz + blk), _CMP_EQ_OQ);
                    if (dbgEqual) {
                        st->equalFailures += (unsigned)_mm_popcnt_u32((unsigned)(mk & ~cov));
                        /* IN FRONT of what the depth fill left: no opaque surface can be, so these are I2 violations */
                        st->equalInFront += (unsigned)_mm_popcnt_u32((unsigned)_mm512_mask_cmp_ps_mask((__mmask16)(mk & ~cov), zs, _mm512_load_ps(tz + blk), _CMP_LT_OQ));
                    }
                }
                if (cov && stencilTest)                     /* GL_GEQUAL, 128: pass where the count is at most 128 */
                    cov = _mm_mask_cmple_epu8_mask(cov, _mm_load_si128((const __m128i *)(ts + blk)), _mm_set1_epi8((char)128));
                if (!cov) continue;
#if SW_ORACLES          /* where a colour stage's time goes: 10 = no kernel and no blend, 11 = kernel but no blend */
                if (kern == SW_KERN_STAGE && kernelCut == 10) continue;
#endif
                VI src = vColor;
                if (kern != SW_KERN_FLAT) {
                    st->kernelCalls++; st->kernelLanes += (unsigned)_mm_popcnt_u32(cov);
                    if (kern == SW_KERN_STAGE) {
#if SW_ORACLES
                        const int cls = (sparms->screenAligned ? 0 : 3) + (bstate.fast == 1 ? 0 : (bstate.fast == 2 ? 1 : 2));
                        st->stLanes[cls] += (unsigned)_mm_popcnt_u32(cov); st->stCalls[cls]++;
                        if (sparms->vertexColorModulate != 0.0f) st->stVertexColorCalls++;
                        t_lastWin = 0;
#endif
                        cov = k_stage(attr, sparms, cov, X, Y, &src);
#if SW_ORACLES
                        if (t_lastWin) st->stWinHits[cls]++;
#endif
                    }
                    else if (kern == SW_KERN_INTERACTION) st->lightCalls++, st->lightLanes += (unsigned)_mm_popcnt_u32(cov), cov = k_interaction(attr, iparms, iflags, kernelCut, cov, X, Y, &src, st);
                    else if (kern == SW_KERN_DUAL) cov = k_dual(attr, (const SwDualParms *)d->kernelParms, cov, X, Y, &src);
                    else if (kern == SW_KERN_ENV) cov = k_env(attr, (const SwEnvParms *)d->kernelParms, cov, X, Y, &src);
                    else cov = k_screen(attr, (const SwScreenParms *)d->kernelParms, c->capture, &c->captureRect, cov, X, Y,
                                        _mm512_add_ps(X, _mm512_set1_ps((float)ox)), _mm512_add_ps(Y, _mm512_set1_ps((float)oy)), &src);
                    if (!cov) continue;
                }
#if SW_ORACLES
                if (kern == SW_KERN_STAGE && kernelCut == 11) continue;
#endif
                blend_store(tcol + blk, cov, src, &bstate);
                if (depthWrite) _mm512_mask_store_ps(tz + blk, cov, zs);
                st->pxColor += (unsigned)_mm_popcnt_u32(cov);
            }
        }
    }
}
