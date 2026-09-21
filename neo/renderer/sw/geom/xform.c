/* geom/xform.c -- object space to clip space, in the pool, and the view arena the results live in.

   The glue asks for a transform while it walks the view (sw_transform) and gets the address of
   the result at once; the numbers are written when sw_end_view starts, before the geometry
   phase reads them. One transform per (vertex array, space, projection variant): the glue caches
   the returned pointer, so the depth fill, every light pass and the ambient stages of a surface
   read the SAME clip positions (invariant I2). Should the cache ever miss, the recomputation is
   still bit-identical: one code path, explicit fmadds, the tail goes through the same vector
   code under a mask.

   SW_XF_SHADOW is shadow.vp: a w = 0 vertex is the w = 1 vertex pushed to infinity away from the
   light. The program computes (pos - L) + w * L, which is not exactly pos for w = 1; here it is
   pos - (1 - w) * L, exact on both ends. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

#define ARENA_BYTES     ((size_t)256 << 20)     /* demand-zero: only what a view touches is ever real */
#define XFORM_BATCH     2048                    /* vertices per pool job: a job must be worth its atomic */

typedef struct SwXformRec { SwXform x; float *out; } SwXformRec;
typedef struct SwXformBatch { int first, count; } SwXformBatch;

static char *       g_arena;
static size_t       g_arenaUsed;
static SwXformRec * g_xforms;       static int g_numXforms, g_capXforms;
static SwXformBatch *g_xformBatches; static int g_numXformBatches, g_capXformBatches;

static int arena_init(void)
{
    if (!g_arena) g_arena = (char *)VirtualAlloc(NULL, ARENA_BYTES, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    return g_arena != NULL;
}

static void arena_shutdown(void)
{
    if (g_arena) VirtualFree(g_arena, 0, MEM_RELEASE);
    g_arena = NULL; g_arenaUsed = 0;
    free(g_xforms); free(g_xformBatches);
    g_xforms = NULL; g_xformBatches = NULL;
    g_numXforms = g_capXforms = g_numXformBatches = g_capXformBatches = 0;
}

void *sw_view_alloc(size_t bytes)
{
    bytes = (bytes + 63) & ~(size_t)63;
    if (!g_arena || g_arenaUsed + bytes > ARENA_BYTES) return NULL;
    void *p = g_arena + g_arenaUsed;
    g_arenaUsed += bytes;
    return p;
}

const float *sw_transform(const SwXform *x)
{
    if (x->numVerts <= 0) return NULL;
    /* whole vectors of 16 vertices are stored, so the block is padded to them */
    float *out = (float *)sw_view_alloc(((size_t)x->numVerts + 15) / 16 * 16 * 4 * sizeof(float));
    if (!out) return NULL;
    if (g_numXforms == g_capXforms) {
        g_capXforms = g_capXforms ? g_capXforms * 2 : 1024;
        g_xforms = (SwXformRec *)realloc(g_xforms, (size_t)g_capXforms * sizeof(SwXformRec));
    }
    g_xforms[g_numXforms].x = *x;
    g_xforms[g_numXforms].out = out;
    g_numXforms++;
    return out;
}

static void xform_run(const SwXformRec *r)
{
    const SwXform *x = &r->x;
    const float *m = x->mvp;
    const VF m0 = _mm512_set1_ps(m[0]), m4 = _mm512_set1_ps(m[4]), m8 = _mm512_set1_ps(m[8]), m12 = _mm512_set1_ps(m[12]);
    const VF m1 = _mm512_set1_ps(m[1]), m5 = _mm512_set1_ps(m[5]), m9 = _mm512_set1_ps(m[9]), m13 = _mm512_set1_ps(m[13]);
    const VF m2 = _mm512_set1_ps(m[2]), m6 = _mm512_set1_ps(m[6]), m10 = _mm512_set1_ps(m[10]), m14 = _mm512_set1_ps(m[14]);
    const VF m3 = _mm512_set1_ps(m[3]), m7 = _mm512_set1_ps(m[7]), m11 = _mm512_set1_ps(m[11]), m15 = _mm512_set1_ps(m[15]);
    const VF lx = _mm512_set1_ps(x->lightOrigin[0]), ly = _mm512_set1_ps(x->lightOrigin[1]), lz = _mm512_set1_ps(x->lightOrigin[2]);
    const VF one = _mm512_set1_ps(1.0f), half = _mm512_set1_ps(0.5f);
    const VI lane = _mm512_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    const VI stride = _mm512_set1_epi32(x->stride);
    const float *base = (const float *)x->positions;
    float *out = r->out;

    for (int i = 0; i < x->numVerts; i += 16, out += 64) {
        const int left = x->numVerts - i;
        const __mmask16 k = left >= 16 ? (__mmask16)0xFFFF : (__mmask16)((1u << left) - 1);
        const VI ofs = _mm512_mullo_epi32(_mm512_add_epi32(_mm512_set1_epi32(i), lane), stride);
        VF px = _mm512_mask_i32gather_ps(_mm512_setzero_ps(), k, ofs, base, 1);
        VF py = _mm512_mask_i32gather_ps(_mm512_setzero_ps(), k, ofs, base + 1, 1);
        VF pz = _mm512_mask_i32gather_ps(_mm512_setzero_ps(), k, ofs, base + 2, 1);
        VF pw = one;
        if (x->kind == SW_XF_SHADOW) {
            pw = _mm512_mask_i32gather_ps(one, k, ofs, base + 3, 1);
            const VF away = _mm512_sub_ps(one, pw);
            px = _mm512_fnmadd_ps(lx, away, px);
            py = _mm512_fnmadd_ps(ly, away, py);
            pz = _mm512_fnmadd_ps(lz, away, pz);
        }
        /* column-major M * (px, py, pz, pw), one fixed association */
        const VF cx = _mm512_fmadd_ps(m12, pw, _mm512_fmadd_ps(m8,  pz, _mm512_fmadd_ps(m4, py, _mm512_mul_ps(m0, px))));
        const VF cy = _mm512_fmadd_ps(m13, pw, _mm512_fmadd_ps(m9,  pz, _mm512_fmadd_ps(m5, py, _mm512_mul_ps(m1, px))));
        const VF cz = _mm512_fmadd_ps(m14, pw, _mm512_fmadd_ps(m10, pz, _mm512_fmadd_ps(m6, py, _mm512_mul_ps(m2, px))));
        const VF cw = _mm512_fmadd_ps(m15, pw, _mm512_fmadd_ps(m11, pz, _mm512_fmadd_ps(m7, py, _mm512_mul_ps(m3, px))));
        const VF cz01 = _mm512_mul_ps(_mm512_add_ps(cz, cw), half);     /* -w..w becomes 0..w */

        /* SoA to AoS: 4x4 transposes inside the 128-bit lanes, then a 4x4 transpose of the lanes */
        const VF t0 = _mm512_unpacklo_ps(cx, cy), t1 = _mm512_unpackhi_ps(cx, cy);
        const VF t2 = _mm512_unpacklo_ps(cz01, cw), t3 = _mm512_unpackhi_ps(cz01, cw);
        const VF r0 = _mm512_shuffle_ps(t0, t2, 0x44), r1 = _mm512_shuffle_ps(t0, t2, 0xEE);
        const VF r2 = _mm512_shuffle_ps(t1, t3, 0x44), r3 = _mm512_shuffle_ps(t1, t3, 0xEE);
        const VF a = _mm512_shuffle_f32x4(r0, r1, 0x44), b = _mm512_shuffle_f32x4(r2, r3, 0x44);
        const VF c = _mm512_shuffle_f32x4(r0, r1, 0xEE), d = _mm512_shuffle_f32x4(r2, r3, 0xEE);
        _mm512_store_ps(out,      _mm512_shuffle_f32x4(a, b, 0x88));
        _mm512_store_ps(out + 16, _mm512_shuffle_f32x4(a, b, 0xDD));
        _mm512_store_ps(out + 32, _mm512_shuffle_f32x4(c, d, 0x88));
        _mm512_store_ps(out + 48, _mm512_shuffle_f32x4(c, d, 0xDD));
    }
}

static void job_xform(int j, int tid)
{
    (void)tid;
    const SwXformBatch *b = &g_xformBatches[j];
    for (int i = 0; i < b->count; i++) xform_run(&g_xforms[b->first + i]);
}

/* serial: group the transforms into batches of about XFORM_BATCH vertices */
static void xform_make_batches(void)
{
    g_numXformBatches = 0;
    int first = 0, verts = 0;
    for (int i = 0; i < g_numXforms; i++) {
        verts += g_xforms[i].x.numVerts;
        if (verts >= XFORM_BATCH || i == g_numXforms - 1) {
            if (g_numXformBatches == g_capXformBatches) {
                g_capXformBatches = g_capXformBatches ? g_capXformBatches * 2 : 256;
                g_xformBatches = (SwXformBatch *)realloc(g_xformBatches, (size_t)g_capXformBatches * sizeof(SwXformBatch));
            }
            g_xformBatches[g_numXformBatches].first = first;
            g_xformBatches[g_numXformBatches].count = i + 1 - first;
            g_numXformBatches++;
            first = i + 1; verts = 0;
        }
    }
}
