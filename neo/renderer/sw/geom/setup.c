/* geom/setup.c -- the geometry phase: clip, project, snap, edge and depth-plane setup, binning.
   From CRenderer's frame/setup.c, with the changes DOOM 3 forces:

   - near, w and GUARD-BAND clipping in homogeneous clip space. Shadow volumes carry vertices at
     infinity (w = 0): nothing may divide before the clip, and a forward-facing point at infinity
     has an unbounded screen position, so polygons are cut against |x| <= G*w, |y| <= G*w with G
     chosen so every screen extent stays under EDGE_EXT_MAX. Every triangle then takes the proven
     fast edge setup.
   - clip intersections are computed from the INSIDE vertex towards the outside one, whatever
     direction the polygon walks the edge in. Two triangles sharing an edge walk it in opposite
     directions, and P + (Q-P)t is not bit-identical to Q + (P-Q)(1-t): a canonical direction is
     what keeps the shared vertex shared after clipping (invariant I8, stencil counts).
   - facing comes from the SNAPPED integer area, so it always agrees with the coverage.
   - planes are anchored at the bbox corner clamped to the viewport, never to a scissor (I3).
   - one writer per bin row: rows are geometry jobs (I1).

   Compiled precise: a triangle must set up bit-identically in every pass it appears in (I2), and
   /fp:fast contracts and redistributes mul/add trees differently per inlined copy. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/types.h"

#pragma float_control(precise, on, push)
#pragma fp_contract(off)

typedef struct ClipVert { float x, y, z, w; float var[SW_VAR_PAD]; } ClipVert;
typedef struct ScrVert { float sx, sy, z, invW; const float *var; } ScrVert;

enum {
    OC_NEAR = 1, OC_WMIN = 2,
    OC_LEFT = 4, OC_RIGHT = 8, OC_BOTTOM = 16, OC_TOP = 32,          /* the frustum sides: reject only */
    OC_GLEFT = 64, OC_GRIGHT = 128, OC_GBOTTOM = 256, OC_GTOP = 512, /* the guard band: clip */
    OC_REJECT = OC_NEAR | OC_WMIN | OC_LEFT | OC_RIGHT | OC_BOTTOM | OC_TOP,
    OC_CLIP = OC_NEAR | OC_WMIN | OC_GLEFT | OC_GRIGHT | OC_GBOTTOM | OC_GTOP
};

static int alloc_tri_slot(int tid)
{
    SwSlotCache *s = &g_slot[tid];
    if (s->next == s->end) {
        s->next = InterlockedExchangeAdd(&g_triCount, 256);
        s->end = s->next + 256;
    }
    if (s->next >= g_triCap) return -1;          /* array full: drop the triangle */
    return s->next++;
}

static void bin_push(SwBin *b, int v)
{
    if (b->stamp != g_viewStamp) { b->stamp = g_viewStamp; b->count = 0; }
    if (b->count == b->cap) {
        b->cap = b->cap ? b->cap * 2 : 64;
        b->items = (int *)realloc(b->items, (size_t)b->cap * sizeof(int));   /* the CRT heap, never the engine's (I7) */
    }
    b->items[b->count++] = v;
}

static __forceinline int32_t snap_subpix(float v)
{
    return _mm_cvtss_si32(_mm_set_ss(v * (float)SUBPIX_ONE));   /* nearest-even under the default MXCSR */
}

#define SAMPLE_OX ((int64_t)(SUBPIX_ONE / 2))       /* the sample sits at the pixel centre */
#define SAMPLE_OY ((int64_t)(SUBPIX_ONE / 2))

/* One integer edge (CRenderer's SETUP_EDGE): gradient (A, B) points into the interior of a
   NEGATIVE-area triangle in y-down screen space. Fill rule: a centre exactly on an edge belongs
   to the triangle only if the edge is a left edge (A > 0) or a BOTTOM edge on screen (A == 0,
   B < 0); folded into C as -1 BEFORE the floor shift, so e >= 0 is the whole test.
   CRenderer's rule was top-left. OpenGL applies top-left in ITS window space, where y points up,
   which is bottom-left on screen: measured against the reference GPU on the console's bottom
   bar, whose upper edge sits exactly on a row of pixel centres at 1080p (478 * 2.25 = 1075.5).
   GUIs are full of such edges (640x480 virtual pixels scaled by 2.25, 3 or 4.5). */
#define SETUP_EDGE(t, k, Ae, Be, Xi, Yi, RX, RY) do {                                           \
        const int64_t bias_ = ((Ae) < 0) | (((Ae) == 0) & ((Be) >= 0));                        \
        (t)->C[k] = ((Ae) * ((RX) + SAMPLE_OX - (Xi)) + (Be) * ((RY) + SAMPLE_OY - (Yi)) - bias_) >> SUBPIX_SHIFT; \
        (t)->A[k] = (int32_t)(Ae); (t)->B[k] = (int32_t)(Be);                                   \
    } while (0)

static void setup_and_bin(const ScrVert *s0, const ScrVert *s1, const ScrVert *s2, int job, int nvar, int tid)
{
    SwThreadStats *st = &g_stats[tid];
    const int drawIdx = g_jobs[job].draw;
    const SwDraw *d = &g_draws[drawIdx];

    int32_t X0 = snap_subpix(s0->sx), Y0 = snap_subpix(s0->sy);
    int32_t X1 = snap_subpix(s1->sx), Y1 = snap_subpix(s1->sy);
    int32_t X2 = snap_subpix(s2->sx), Y2 = snap_subpix(s2->sy);

    /* facing from the snapped integers: exact, and the same integers decide coverage */
    const int64_t area = ((int64_t)X1 - X0) * ((int64_t)Y2 - Y0) - ((int64_t)Y1 - Y0) * ((int64_t)X2 - X0);
    if (area == 0) return;                                  /* covers no sample in any pass */
    /* DOOM's front side winds clockwise in GL's y-up window space = positive area in y-down
       (GL_Cull: CT_FRONT_SIDED culls GL_FRONT); a mirror view swaps the sides */
    const int back = (area > 0) == (d->mirror != 0);
    if (d->op == SW_OP_COLOR || d->op == SW_OP_DEPTH_FILL) {
        if (d->cull == SW_CULL_FRONT_SIDED && back) return;
        if (d->cull == SW_CULL_BACK_SIDED && !back) return;
    }
    if (area > 0) {                                         /* the edge setup wants negative area */
        const ScrVert *ts = s1; s1 = s2; s2 = ts;
        int32_t ti = X1; X1 = X2; X2 = ti;
        ti = Y1; Y1 = Y2; Y2 = ti;
    }

    /* the pixels whose CENTRE lies inside the snapped bbox: exact, so a triangle that falls
       between centres on either axis drops out here */
    const int32_t Xmin = X0 < X1 ? (X0 < X2 ? X0 : X2) : (X1 < X2 ? X1 : X2);
    const int32_t Xmax = X0 > X1 ? (X0 > X2 ? X0 : X2) : (X1 > X2 ? X1 : X2);
    const int32_t Ymin = Y0 < Y1 ? (Y0 < Y2 ? Y0 : Y2) : (Y1 < Y2 ? Y1 : Y2);
    const int32_t Ymax = Y0 > Y1 ? (Y0 > Y2 ? Y0 : Y2) : (Y1 > Y2 ? Y1 : Y2);
    if (((int64_t)Xmax - Xmin) + ((int64_t)Ymax - Ymin) > (int64_t)EDGE_MAG_MAX - 2) {
        st->trisDropped++;                                  /* the guard band makes this unreachable */
        return;
    }
    int bx0 = (Xmin + (SUBPIX_ONE / 2 - 1)) >> SUBPIX_SHIFT, bx1 = (Xmax - SUBPIX_ONE / 2) >> SUBPIX_SHIFT;
    int by0 = (Ymin + (SUBPIX_ONE / 2 - 1)) >> SUBPIX_SHIFT, by1 = (Ymax - SUBPIX_ONE / 2) >> SUBPIX_SHIFT;
    if (bx0 < g_view.x0) bx0 = g_view.x0;
    if (by0 < g_view.y0) by0 = g_view.y0;
    if (bx1 > g_view.x1) bx1 = g_view.x1;
    if (by1 > g_view.y1) by1 = g_view.y1;
    if (bx0 > bx1 || by0 > by1) return;
    const int ox = bx0, oy = by0;                           /* the plane origin: viewport-clamped, scissor-free (I3) */
    if (bx0 < d->scissor.x0) bx0 = d->scissor.x0;
    if (by0 < d->scissor.y0) by0 = d->scissor.y0;
    if (bx1 > d->scissor.x1) bx1 = d->scissor.x1;
    if (by1 > d->scissor.y1) by1 = d->scissor.y1;
    if (bx0 > bx1 || by0 > by1) return;

    const int slot = alloc_tri_slot(tid);
    if (slot < 0) { st->trisDropped++; return; }
    SwTri *t = &g_tris[slot];

    const int64_t RX = (int64_t)ox << SUBPIX_SHIFT, RY = (int64_t)oy << SUBPIX_SHIFT;
    const int64_t A0 = (int64_t)Y2 - Y1, B0 = (int64_t)X1 - X2;   /* edge v1->v2, opposite vertex 0 */
    const int64_t A1 = (int64_t)Y0 - Y2, B1 = (int64_t)X2 - X0;
    const int64_t A2 = (int64_t)Y1 - Y0, B2 = (int64_t)X0 - X1;
    SETUP_EDGE(t, 0, A0, B0, X1, Y1, RX, RY);
    SETUP_EDGE(t, 1, A1, B1, X2, Y2, RX, RY);
    SETUP_EDGE(t, 2, A2, B2, X0, Y0, RX, RY);

    /* the depth plane in double at the local origin (float at a global origin shredded long thin
       triangles in CRenderer). A degenerate float area next to a non-zero snapped one gives inf
       or NaN gradients; the raster's clamp to [minz, maxz] absorbs both. */
    const double x0d = s0->sx, y0d = s0->sy;
    const double dx10 = (double)s1->sx - x0d, dy10 = (double)s1->sy - y0d;
    const double dx20 = (double)s2->sx - x0d, dy20 = (double)s2->sy - y0d;
    const double den = dx10 * dy20 - dy10 * dx20;
    const double invA = fabs(den) > 1e-30 ? 1.0 / den : 0.0;    /* a float-degenerate sliver gets flat planes */
    const double z0 = s0->z, z1 = s1->z, z2 = s2->z;
    const double gx = ((z1 - z0) * dy20 - (z2 - z0) * dy10) * invA;
    const double gy = ((z2 - z0) * dx10 - (z1 - z0) * dx20) * invA;
    double zc = z0 + gx * ((double)ox - x0d) + gy * ((double)oy - y0d);

    float mn = s0->z < s1->z ? (s0->z < s2->z ? s0->z : s2->z) : (s1->z < s2->z ? s1->z : s2->z);
    float mx = s0->z > s1->z ? (s0->z > s2->z ? s0->z : s2->z) : (s1->z > s2->z ? s1->z : s2->z);
    if (d->offsetFactor != 0.0f || d->offsetUnits != 0.0f) {
        const double m = fabs(gx) > fabs(gy) ? fabs(gx) : fabs(gy);
        const double off = (double)d->offsetFactor * m + (double)d->offsetUnits * (1.0 / 16777216.0);
        if (off == off && fabs(off) < 4.0) {                /* a NaN/inf slope offsets nothing */
            zc += off; mn += (float)off; mx += (float)off;
        }
    }
    t->zc = (float)zc; t->zgx = (float)gx; t->zgy = (float)gy;
    t->minz = mn < 0.0f ? 0.0f : mn;                        /* GL clamps window depth to the depth range */
    t->maxz = mx > 1.0f ? 1.0f : mx;
    if (t->maxz < t->minz) t->maxz = t->minz;

    t->ox = (int16_t)ox; t->oy = (int16_t)oy;
    t->minx = (int16_t)bx0; t->miny = (int16_t)by0; t->maxx = (int16_t)bx1; t->maxy = (int16_t)by1;
    t->draw = drawIdx;
    t->back = back;
    st->trisSetup++;

    if (nvar) {
        /* The attribute planes, eight at a time in double (CRenderer's PLANE_VEC): 1/w is
           screen-linear, every varying is premultiplied by it. Float planes at a global origin
           shredded long thin triangles; double at the local origin does not. */
        SwTriAttr *a = &g_triAttrs[slot];
        const double iw0 = s0->invW, iw1 = s1->invW, iw2 = s2->invW;
        const double rx = (double)ox - x0d, ry = (double)oy - y0d;
        {
            const double wgx = ((iw1 - iw0) * dy20 - (iw2 - iw0) * dy10) * invA;
            const double wgy = ((iw2 - iw0) * dx10 - (iw1 - iw0) * dx20) * invA;
            a->w[0] = (float)(iw0 + wgx * rx + wgy * ry); a->w[1] = (float)wgx; a->w[2] = (float)wgy;
        }
        const __m512d vdy20 = _mm512_set1_pd(dy20), vdy10 = _mm512_set1_pd(dy10);
        const __m512d vdx10 = _mm512_set1_pd(dx10), vdx20 = _mm512_set1_pd(dx20);
        const __m512d vinvA = _mm512_set1_pd(invA), vrx = _mm512_set1_pd(rx), vry = _mm512_set1_pd(ry);
        for (int k = 0; k < nvar; k += 8) {
            const __m512d p0 = _mm512_cvtps_pd(_mm256_mul_ps(_mm256_loadu_ps(s0->var + k), _mm256_set1_ps(s0->invW)));
            const __m512d p1 = _mm512_cvtps_pd(_mm256_mul_ps(_mm256_loadu_ps(s1->var + k), _mm256_set1_ps(s1->invW)));
            const __m512d p2 = _mm512_cvtps_pd(_mm256_mul_ps(_mm256_loadu_ps(s2->var + k), _mm256_set1_ps(s2->invW)));
            const __m512d d1 = _mm512_sub_pd(p1, p0), d2 = _mm512_sub_pd(p2, p0);
            const __m512d gxv = _mm512_mul_pd(_mm512_sub_pd(_mm512_mul_pd(d1, vdy20), _mm512_mul_pd(d2, vdy10)), vinvA);
            const __m512d gyv = _mm512_mul_pd(_mm512_sub_pd(_mm512_mul_pd(d2, vdx10), _mm512_mul_pd(d1, vdx20)), vinvA);
            const __m512d cv = _mm512_add_pd(_mm512_add_pd(p0, _mm512_mul_pd(gxv, vrx)), _mm512_mul_pd(gyv, vry));
            __declspec(align(32)) float fc[8], fgx[8], fgy[8];
            _mm256_store_ps(fc, _mm512_cvtpd_ps(cv));
            _mm256_store_ps(fgx, _mm512_cvtpd_ps(gxv));
            _mm256_store_ps(fgy, _mm512_cvtpd_ps(gyv));
            for (int i = 0; i < 8 && k + i < nvar; i++) { a->p[k + i][0] = fc[i]; a->p[k + i][1] = fgx[i]; a->p[k + i][2] = fgy[i]; }
        }
    }

    const int tx0 = bx0 >> TILE_SHIFT, tx1 = bx1 >> TILE_SHIFT;
    const int ty0 = by0 >> TILE_SHIFT, ty1 = by1 >> TILE_SHIFT;
    /* Which tiles of the bounding box can the triangle cover at all? A long diagonal sliver (every
       silhouette quad of a shadow volume is one) touches a band of its box, and each tile it was
       binned into for nothing costs a bin entry now and a whole raster_tri set-up later (measured at
       4K: 82% of the shadow time is walks, 6.3 blocks each). The test is the raster's own, one
       level up from its 16x16 cells: an edge function is linear, so its largest value over the
       tile's part of the walked rectangle sits at one corner; negative there for ANY edge means
       no pixel of the tile passes. Same integers, same fill-rule bias inside C: exact.
       A box one tile wide or high needs no test: a triangle's extent along the other axis is an
       interval, so it reaches into every tile of the strip. */
    const int tileTest = g_opt[SW_OPT_BIN_EXACT] && tx1 > tx0 && ty1 > ty0;
    for (int ty = ty0; ty <= ty1; ty++) {
        int64_t rowMax[3] = { 0, 0, 0 };
        if (tileTest) {
            const int ry0 = (ty << TILE_SHIFT) > by0 ? (ty << TILE_SHIFT) : by0;
            const int ry1 = ((ty << TILE_SHIFT) + TILE_SIZE - 1) < by1 ? ((ty << TILE_SHIFT) + TILE_SIZE - 1) : by1;
            for (int k = 0; k < 3; k++)
                rowMax[k] = (int64_t)t->B[k] * (int64_t)((t->B[k] > 0 ? ry1 : ry0) - oy) + t->C[k];
        }
        for (int tx = tx0; tx <= tx1; tx++) {
            if (tileTest) {
                const int rx0 = (tx << TILE_SHIFT) > bx0 ? (tx << TILE_SHIFT) : bx0;
                const int rx1 = ((tx << TILE_SHIFT) + TILE_SIZE - 1) < bx1 ? ((tx << TILE_SHIFT) + TILE_SIZE - 1) : bx1;
                int outside = 0;
                for (int k = 0; k < 3; k++)
                    outside |= (rowMax[k] + (int64_t)t->A[k] * (int64_t)((t->A[k] > 0 ? rx1 : rx0) - ox)) < 0;
                if (outside) { st->binSkips++; continue; }
            }
            const int tile = ty * g_tilesX + tx;
            SwBin *b = &g_bins[(size_t)job * g_ntiles + tile];
            if (b->stamp != g_viewStamp)                    /* first push into this (job, tile): the tile's presence bit */
                InterlockedOr64(&g_tileJobBits[(size_t)tile * g_jobStride + (job >> 6)], (LONG64)1 << (job & 63));
            bin_push(b, slot);
            st->binPushes++;
        }
    }
}

static __forceinline int outcode(const ClipVert *v, float guard)
{
    const float gw = guard * v->w;
    int o = 0;
    if (v->z < 0.0f) o |= OC_NEAR;
    if (v->w < CLIP_W_MIN) o |= OC_WMIN;
    if (v->x < -v->w) o |= OC_LEFT;
    if (v->x > v->w) o |= OC_RIGHT;
    if (v->y < -v->w) o |= OC_BOTTOM;
    if (v->y > v->w) o |= OC_TOP;
    if (v->x < -gw) o |= OC_GLEFT;
    if (v->x > gw) o |= OC_GRIGHT;
    if (v->y < -gw) o |= OC_GBOTTOM;
    if (v->y > gw) o |= OC_GTOP;
    return o;
}

static __forceinline float clip_dist(const ClipVert *v, int plane, float guard)
{
    switch (plane) {
    case OC_NEAR:    return v->z;
    case OC_WMIN:    return v->w - CLIP_W_MIN;
    case OC_GLEFT:   return guard * v->w + v->x;
    case OC_GRIGHT:  return guard * v->w - v->x;
    case OC_GBOTTOM: return guard * v->w + v->y;
    default:         return guard * v->w - v->y;
    }
}

/* Sutherland-Hodgman against one plane; the intersection always runs inside -> outside */
static int clip_polygon(const ClipVert *in, int n, ClipVert *out, int plane, float guard, int nvar)
{
    int m = 0;
    for (int i = 0; i < n; i++) {
        const ClipVert *pc = &in[i], *pn = &in[i + 1 == n ? 0 : i + 1];
        const float dc = clip_dist(pc, plane, guard), dn = clip_dist(pn, plane, guard);
        const int cIn = dc >= 0.0f, nIn = dn >= 0.0f;
        if (cIn) out[m++] = *pc;
        if (cIn != nIn) {
            const ClipVert *a = cIn ? pc : pn, *b = cIn ? pn : pc;      /* a inside, b outside */
            const float da = cIn ? dc : dn, db = cIn ? dn : dc;
            const float tt = da / (da - db);
            ClipVert *o = &out[m++];
            o->x = a->x + (b->x - a->x) * tt;
            o->y = a->y + (b->y - a->y) * tt;
            o->z = a->z + (b->z - a->z) * tt;
            o->w = a->w + (b->w - a->w) * tt;
            if (plane == OC_NEAR) o->z = 0.0f;              /* exactly on the plane */
            if (plane == OC_WMIN) o->w = CLIP_W_MIN;
            for (int k = 0; k < nvar; k += 8) {             /* clip space is linear in t, varyings included */
                const __m256 va = _mm256_loadu_ps(a->var + k), vb = _mm256_loadu_ps(b->var + k);
                _mm256_storeu_ps(o->var + k, _mm256_add_ps(va, _mm256_mul_ps(_mm256_sub_ps(vb, va), _mm256_set1_ps(tt))));
            }
        }
    }
    return m;
}

static __forceinline void project(const ClipVert *c, ScrVert *s, float depthMax)
{
    s->var = c->var;
    const float invW = 1.0f / c->w;
    s->sx = g_viewCx + c->x * invW * g_viewHw;
    s->sy = g_viewCy - c->y * invW * g_viewHh;              /* y flip: the screen is y-down */
    s->z = c->z * invW * depthMax;
    s->invW = invW;
}

static void clip_and_setup(const SwDraw *d, const int *idx, int job, float depthMax, int nvar, int tid)
{
    SwThreadStats *st = &g_stats[tid];
    ClipVert a[12], b[12];
    const char *base = (const char *)d->clip;
    const size_t stride = (size_t)d->clipStride;
    for (int i = 0; i < 3; i++) {
        const float *c = (const float *)(base + idx[i] * stride);
        a[i].x = c[0]; a[i].y = c[1]; a[i].z = c[2]; a[i].w = c[3];
    }
    st->trisIn++;

    const float guard = g_guard;
    const int o0 = outcode(&a[0], guard), o1 = outcode(&a[1], guard), o2 = outcode(&a[2], guard);
    if (o0 & o1 & o2 & OC_REJECT) return;                   /* all three outside one frustum plane */

    if (nvar) {                                             /* the vertex program, per corner, after the cheap reject */
        memset(a[0].var, 0, sizeof(a[0].var)); memset(a[1].var, 0, sizeof(a[1].var)); memset(a[2].var, 0, sizeof(a[2].var));
        vertex_half(d, idx[0], a[0].var); vertex_half(d, idx[1], a[1].var); vertex_half(d, idx[2], a[2].var);
    }

    ScrVert sv[12];
    const int need = (o0 | o1 | o2) & OC_CLIP;
    if (!need) {
        project(&a[0], &sv[0], depthMax); project(&a[1], &sv[1], depthMax); project(&a[2], &sv[2], depthMax);
        setup_and_bin(&sv[0], &sv[1], &sv[2], job, nvar, tid);
        return;
    }

    st->trisClipped++;
    ClipVert *in = a, *out = b;
    int n = 3;
    static const int planes[6] = { OC_NEAR, OC_WMIN, OC_GLEFT, OC_GRIGHT, OC_GBOTTOM, OC_GTOP };
    for (int p = 0; p < 6 && n >= 3; p++) {
        if (!(need & planes[p])) continue;
        n = clip_polygon(in, n, out, planes[p], guard, nvar);
        ClipVert *tmp = in; in = out; out = tmp;
    }
    if (n < 3) return;
    for (int i = 0; i < n; i++) project(&in[i], &sv[i], depthMax);
    for (int k = 2; k < n; k++)                             /* fan from vertex 0 */
        setup_and_bin(&sv[0], &sv[k - 1], &sv[k], job, nvar, tid);
}

/* PH_SETUP: one geometry job */
static void job_setup(int j, int tid)
{
    const SwJob *job = &g_jobs[j];
    const SwDraw *d = &g_draws[job->draw];
    const int *idx = d->indexes + (size_t)job->firstTri * 3;
    const float depthMax = d->depthRangeMax > 0.0f ? d->depthRangeMax : 1.0f;
    const int nvar = (d->op == SW_OP_COLOR || d->op == SW_OP_DEPTH_FILL) ? kernel_num_varyings(d->kernel) : 0;
    for (int i = 0; i < job->numTris; i++, idx += 3)
        clip_and_setup(d, idx, j, depthMax, nvar, tid);
}

#pragma float_control(pop)
