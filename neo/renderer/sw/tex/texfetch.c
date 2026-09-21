/* tex/texfetch.c -- SwImage and the bilinear fetch in 16-bit fields.
   From CRenderer's frame/texfetch.c: per-lane mip tables permuted out of one vector each, the
   horizontal texel pair of a bilinear row fetched as ONE 64-bit gather element, the integer
   bilinear blend in two dwords of two 16-bit fields. Changed for DOOM 3: RGBA byte order,
   a clamp addressing mode beside repeat, any coordinate set given as numerator / denominator
   planes (so projective lookups share the path).

   A 64-byte pad trails the chain: the +1 texel of a pair gather at the very last texel reads
   into it. It is load-bearing. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

#define SW_MAX_MIPS 16
#if SW_ORACLES
#define SW_ORACLE_PAD 256       /* cut 6 reads 32 texels from any texel of the chain */
#else
#define SW_ORACLE_PAD 0
#endif

struct SwImage {
    uint32_t *  texels;                 /* all levels, contiguous, 64-byte aligned */
    int         w0, h0, nmips, wrap;
    int         cube, faceTexels;       /* a cube map: six chains of faceTexels texels each, face f at f * faceTexels */
    int         zeroBorder;             /* every border texel of every level is 0: outside [0, 1] a clamped lookup is exactly black */
    /* per-level tables padded to 16 so a zmm permute is a per-lane lookup */
    __declspec(align(64)) int   moff[SW_MAX_MIPS];
    __declspec(align(64)) int   mw[SW_MAX_MIPS];
    __declspec(align(64)) int   mwm1[SW_MAX_MIPS];
    __declspec(align(64)) int   mhm1[SW_MAX_MIPS];
    __declspec(align(64)) float fmw[SW_MAX_MIPS];
    __declspec(align(64)) float fmh[SW_MAX_MIPS];
};

SwImage *sw_image_create(int width, int height, int numLevels, const uint8_t *const *levels, int wrap)
{
    if (width < 1 || height < 1 || numLevels < 1) return NULL;
    if (numLevels > SW_MAX_MIPS) numLevels = SW_MAX_MIPS;
    SwImage *im = (SwImage *)_aligned_malloc(sizeof(SwImage), 64);
    memset(im, 0, sizeof(*im));
    im->w0 = width; im->h0 = height; im->wrap = wrap;
    size_t total = 0;
    int w = width, h = height, n = 0;
    for (; n < numLevels; n++) {
        im->moff[n] = (int)total; im->mw[n] = w; im->mwm1[n] = w - 1; im->mhm1[n] = h - 1;
        im->fmw[n] = (float)w; im->fmh[n] = (float)h;
        total += (size_t)w * h;
        if (w == 1 && h == 1) { n++; break; }
        w = w > 1 ? w >> 1 : 1; h = h > 1 ? h >> 1 : 1;
    }
    im->nmips = n;
    for (int i = n; i < SW_MAX_MIPS; i++) {                 /* a level past the chain reads the last one */
        im->moff[i] = im->moff[n - 1]; im->mw[i] = im->mw[n - 1]; im->mwm1[i] = im->mwm1[n - 1];
        im->mhm1[i] = im->mhm1[n - 1]; im->fmw[i] = im->fmw[n - 1]; im->fmh[i] = im->fmh[n - 1];
    }
    im->texels = (uint32_t *)_aligned_malloc(total * 4 + 64 + SW_ORACLE_PAD, 64);
    for (int i = 0; i < n; i++)
        memcpy(im->texels + im->moff[i], levels[i], (size_t)im->mw[i] * (im->mhm1[i] + 1) * 4);
    memset(im->texels + total, 0, 64);
    im->zeroBorder = wrap == SW_WRAP_CLAMP;
    for (int i = 0; i < n && im->zeroBorder; i++) {
        const uint32_t *t = im->texels + im->moff[i];
        const int lw = im->mw[i], lh = im->mhm1[i] + 1;
        for (int x = 0; x < lw && im->zeroBorder; x++) if (t[x] | t[(size_t)(lh - 1) * lw + x]) im->zeroBorder = 0;
        for (int y = 0; y < lh && im->zeroBorder; y++) if (t[(size_t)y * lw] | t[(size_t)y * lw + lw - 1]) im->zeroBorder = 0;
    }
    return im;
}

SwImage *sw_image_create_cube(int size, int numLevels, const uint8_t *const *const faces[6])
{
    SwImage *im = sw_image_create(size, size, numLevels, faces[0], SW_WRAP_CLAMP);
    if (!im) return NULL;
    size_t total = 0;
    for (int i = 0; i < im->nmips; i++) total += (size_t)im->mw[i] * im->mw[i];
    uint32_t *all = (uint32_t *)_aligned_malloc(total * 6 * 4 + 64 + SW_ORACLE_PAD, 64);
    for (int f = 0; f < 6; f++)
        for (int i = 0; i < im->nmips; i++)
            memcpy(all + total * f + im->moff[i], faces[f][i], (size_t)im->mw[i] * im->mw[i] * 4);
    memset(all + total * 6, 0, 64);
    _aligned_free(im->texels);
    im->texels = all; im->cube = 1; im->faceTexels = (int)total; im->zeroBorder = 0;
    return im;
}

/* the core's own images (the capture): freed from INSIDE the tile pass, which must not wait for itself */
static void image_free(SwImage *image)
{
    if (!image) return;
    _aligned_free(image->texels);
    _aligned_free(image);
}

void sw_image_destroy(SwImage *image)
{
    frame_wait();                                           /* a running tile pass may sample it */
    image_free(image);
}

typedef struct TexLevel { VI mip, iw, iwm1, ihm1, off; VF fw, fh; const SwImage *tx; int level0; } TexLevel;
/* win: the block's texels lie in a WINDOW of 16 columns by 5 rows of one level (tex_window): five row
   loads and permutes fetch them instead of four gathers. rowBase = the texel index of each window
   row's first column; sel0 / sel1 = (row << 4 | column) of a lane's upper and lower left texel. */
typedef struct TexCoords1 { VI idx0, idx1, idx0w, idx1w, fx, fy; __mmask16 xw;
                           int win; int rowBase[5]; VI sel0, sel1; } TexCoords1;

/* SW_OPT_TEX_WINDOW as the running tile pass sees it. A line of its own: it is read per lookup by
   every thread, and nothing written while the pool runs may share a line with that. */
static __declspec(align(64)) int g_texWindow = 1;
static __declspec(align(64)) int g_envHelpers = 1;          /* SW_OPT_ENV_HELPERS: k_env, see there */
static __declspec(align(64)) int g_texTrilinear = 0;        /* SW_OPT_TRILINEAR: GL_LINEAR_MIPMAP_LINEAR instead of .._NEAREST */
static __declspec(align(64)) int g_texLevel0 = 1;           /* SW_OPT_TEX_LEVEL0, the same way */
static __declspec(align(64)) int g_texWindowPadEnd;
#if SW_ORACLES
static __declspec(thread) int t_lastWin;    /* the last lookup's window tier on THIS thread (thread-local: no shared line) */
#endif
#define TEXWIN_STAT(i) ((void)0)   /* (a racy global counter here put 32 writers on one cache line: 68 -> 15 fps) */

/* The level GL_LINEAR_MIPMAP_NEAREST reads: round(log2(rho)), rho = the longer footprint axis in
   texels. rho2 is rho squared: floor(0.5 * log2(2 * rho2)) is the scaled exponent halved,
   whatever the mantissa (CRenderer, 2026-09-16). */
static __forceinline VI tex_level_from_rho2(VF rho2)
{
    return _mm512_srai_epi32(_mm512_cvttps_epi32(_mm512_getexp_ps(
        _mm512_mul_ps(_mm512_max_ps(rho2, _mm512_set1_ps(1e-12f)), _mm512_set1_ps(2.0f)))), 1);
}

/* level0: the caller KNOWS every lane reads level 0 (tex_set_rule proved it, or the lookup says so): the tables
   are constants then, and the window test needs no look at the levels. */
static __forceinline void tex_level(const SwImage *tx, VI lvl, const int level0, TexLevel *L)
{
    L->tx = tx; L->level0 = level0;
    if (level0) {
        L->mip = _mm512_setzero_si512();
        L->fw = _mm512_set1_ps(tx->fmw[0]); L->fh = _mm512_set1_ps(tx->fmh[0]);
        L->iw = _mm512_set1_epi32(tx->mw[0]); L->iwm1 = _mm512_set1_epi32(tx->mwm1[0]);
        L->ihm1 = _mm512_set1_epi32(tx->mhm1[0]); L->off = _mm512_set1_epi32(tx->moff[0]);
        return;
    }
    VI mip = _mm512_max_epi32(lvl, _mm512_setzero_si512());
    mip = _mm512_min_epi32(mip, _mm512_set1_epi32(tx->nmips - 1));
    L->mip = mip;
    L->fw = _mm512_permutexvar_ps(mip, _mm512_load_ps(tx->fmw));
    L->fh = _mm512_permutexvar_ps(mip, _mm512_load_ps(tx->fmh));
    L->iw = _mm512_permutexvar_epi32(mip, _mm512_load_si512((const void *)tx->mw));
    L->iwm1 = _mm512_permutexvar_epi32(mip, _mm512_load_si512((const void *)tx->mwm1));
    L->ihm1 = _mm512_permutexvar_epi32(mip, _mm512_load_si512((const void *)tx->mhm1));
    L->off = _mm512_permutexvar_epi32(mip, _mm512_load_si512((const void *)tx->moff));
}

/* Can the fetch read this block through a window? Measured (4K, oracle): the four 8-element gathers
   are a third of every bilinear lookup, 2.3 ms of a 6.5 ms light pass. At the level the rule picks,
   a 4x4 block's footprint is a handful of texels across, so nearly always YES.
   The window's corner is the smallest column and row of the MASKED lanes (the others may hold
   anything: a lane outside its triangle, a pole of the perspective division), and EVERY masked
   lane is then checked against it, so no choice of corner can make the fetch read a wrong texel:
   a lane out of the window means gathers for the whole block, as before. Conditions: one level
   for all masked lanes; columns within 15 of the corner (the right neighbour is column + 1, the
   pair gather's own rule: what lies behind a row's last texel has weight 0); rows within 4 for
   the upper texel and 5 for the lower one, which makes a wrapped row (repeat) fail unless it
   really is in the window; no lane whose right texel wraps to column 0 (xw). */
static __forceinline void tex_window(const TexLevel *L, VI x0, VI y0, VI y1, __mmask16 m, TexCoords1 *tc)
{
    tc->win = 0;
#if SW_ORACLES
    t_lastWin = 0;
#endif
    TEXWIN_STAT(0);
    if (tc->xw) TEXWIN_STAT(2);
    if (!g_texWindow || tc->xw || !m) return;
    const int big = 0x7FFFFFFF;
    const int xmin = _mm512_mask_reduce_min_epi32(m, x0), ymin = _mm512_mask_reduce_min_epi32(m, y0);
    const int mip = L->level0 ? 0 : _mm512_mask_reduce_min_epi32(m, L->mip);
    (void)big;
    const VI dx = _mm512_sub_epi32(x0, _mm512_set1_epi32(xmin));
    const VI dy0 = _mm512_sub_epi32(y0, _mm512_set1_epi32(ymin)), dy1 = _mm512_sub_epi32(y1, _mm512_set1_epi32(ymin));
    __mmask16 ok = _mm512_mask_cmple_epu32_mask(m, dx, _mm512_set1_epi32(14));
    ok = _mm512_mask_cmple_epu32_mask(ok, dy0, _mm512_set1_epi32(3));
    ok = _mm512_mask_cmple_epu32_mask(ok, dy1, _mm512_set1_epi32(4));
    if (!L->level0) ok = _mm512_mask_cmpeq_epi32_mask(ok, L->mip, _mm512_set1_epi32(mip));
    if (ok != m) return;
    TEXWIN_STAT(1);
    const SwImage *tx = L->tx;
    const int iw = tx->mw[mip], hm1 = tx->mhm1[mip], base = tx->moff[mip] + xmin;
    for (int k = 0; k < 5; k++) {
        const int y = ymin + k < hm1 ? ymin + k : hm1;      /* a row below the level is never selected: keep its load inside the chain */
        tc->rowBase[k] = base + y * iw;
    }
    tc->sel0 = _mm512_or_si512(_mm512_slli_epi32(dy0, 4), dx);
    tc->sel1 = _mm512_or_si512(_mm512_slli_epi32(dy1, 4), dx);
    /* how many rows the fetch has to load: 2 (a magnified map, the light's own textures), 3, or all 5 */
    /* (the larger of the two: a wrapped lower row of a map 4 rows high can sit ABOVE its upper one) */
    const VI dyMax = _mm512_max_epu32(dy0, dy1);
    tc->win = _mm512_mask_cmpgt_epu32_mask(m, dyMax, _mm512_set1_epi32(1)) == 0 ? 2
            : (_mm512_mask_cmpgt_epu32_mask(m, dyMax, _mm512_set1_epi32(2)) == 0 ? 3 : 5);
    /* bisecting: 1 = all tiers, 2 = the five-row path only, 3 = two rows or five, 4 = three rows or five */
    if (g_texWindow == 2 || (g_texWindow == 3 && tc->win == 3) || (g_texWindow == 4 && tc->win == 2)) tc->win = 5;
#if SW_ORACLES
    t_lastWin = tc->win;
#endif
}

/* (u, v) on a looked-up level. `wrap` is a constant at the call site. */
static __forceinline void tex_coords_at(const TexLevel *L, VF u, VF v, __mmask16 m, const int wrap, TexCoords1 *tc)
{
    const VI zero = _mm512_setzero_si512(), onei = _mm512_set1_epi32(1);
    const VF half = _mm512_set1_ps(0.5f), fzero = _mm512_setzero_ps();
    const VI iwm1 = L->iwm1, ihm1 = L->ihm1;
    VF xf, yf;
    if (wrap == SW_WRAP_REPEAT) {
        u = _mm512_sub_ps(u, _mm512_floor_ps(u));
        v = _mm512_sub_ps(v, _mm512_floor_ps(v));
        xf = _mm512_fmsub_ps(u, L->fw, half);
        yf = _mm512_fmsub_ps(v, L->fh, half);
    } else {
        /* clamp to edge: the sample point stays between the first and the last texel centre. At
           the last centre the fraction is 0, so the pair gather's right texel (whatever lies
           behind the row) has weight 0: no fix-up. max() returns its SECOND operand for a NaN. */
        xf = _mm512_min_ps(_mm512_max_ps(_mm512_fmsub_ps(u, L->fw, half), fzero), _mm512_sub_ps(L->fw, _mm512_set1_ps(1.0f)));
        yf = _mm512_min_ps(_mm512_max_ps(_mm512_fmsub_ps(v, L->fh, half), fzero), _mm512_sub_ps(L->fh, _mm512_set1_ps(1.0f)));
    }
    const VF xfl = _mm512_floor_ps(xf), yfl = _mm512_floor_ps(yf);
    VI fx = _mm512_cvtps_epi32(_mm512_mul_ps(_mm512_sub_ps(xf, xfl), _mm512_set1_ps(256.0f)));
    VI fy = _mm512_cvtps_epi32(_mm512_mul_ps(_mm512_sub_ps(yf, yfl), _mm512_set1_ps(256.0f)));
    fx = _mm512_min_epi32(_mm512_max_epi32(fx, zero), _mm512_set1_epi32(256));
    fy = _mm512_min_epi32(_mm512_max_epi32(fy, zero), _mm512_set1_epi32(256));
    VI x0 = _mm512_cvtps_epi32(xfl), y0 = _mm512_cvtps_epi32(yfl);
    VI x1, y1;
    __mmask16 xw = 0;
    if (wrap == SW_WRAP_REPEAT) {
        const __mmask16 xn = _mm512_cmplt_epi32_mask(x0, zero), yn = _mm512_cmplt_epi32_mask(y0, zero);
        x0 = _mm512_mask_mov_epi32(_mm512_min_epi32(x0, iwm1), xn, iwm1);      /* -1 wraps to w-1 */
        y0 = _mm512_mask_mov_epi32(_mm512_min_epi32(y0, ihm1), yn, ihm1);
        x0 = _mm512_max_epi32(x0, zero); y0 = _mm512_max_epi32(y0, zero);
        x1 = _mm512_add_epi32(x0, onei); y1 = _mm512_add_epi32(y0, onei);
        xw = _mm512_mask_cmpgt_epi32_mask(m, x1, iwm1);                         /* the right texel is column 0 */
        x1 = _mm512_mask_mov_epi32(x1, xw, zero);
        y1 = _mm512_mask_mov_epi32(y1, _mm512_cmpgt_epi32_mask(y1, ihm1), zero);
    } else {
        x0 = _mm512_min_epi32(_mm512_max_epi32(x0, zero), iwm1);
        y0 = _mm512_min_epi32(_mm512_max_epi32(y0, zero), ihm1);
        x1 = _mm512_min_epi32(_mm512_add_epi32(x0, onei), iwm1);
        y1 = _mm512_min_epi32(_mm512_add_epi32(y0, onei), ihm1);
    }
    const VI row0 = _mm512_add_epi32(L->off, _mm512_mullo_epi32(y0, L->iw));
    const VI row1 = _mm512_add_epi32(L->off, _mm512_mullo_epi32(y1, L->iw));
    tc->idx0 = _mm512_add_epi32(row0, x0); tc->idx1 = _mm512_add_epi32(row1, x0);
    tc->idx0w = _mm512_add_epi32(row0, x1); tc->idx1w = _mm512_add_epi32(row1, x1);
    tc->fx = fx; tc->fy = fy; tc->xw = xw;
    tex_window(L, x0, y0, y1, m, tc);
}

/* one texel per lane out of the window's rows: sel = row << 4 | column, rows 0..3 (r4 == NULL) or 0..4 */
static __forceinline VI tex_window_pick(VI sel, VI r0, VI r1, VI r2, VI r3, const VI *r4)
{
    const VI a = _mm512_permutex2var_epi32(r0, sel, r1), b = _mm512_permutex2var_epi32(r2, sel, r3);   /* the index's low five bits */
    VI t = _mm512_mask_mov_epi32(a, _mm512_test_epi32_mask(sel, _mm512_set1_epi32(32)), b);
    if (r4) t = _mm512_mask_permutexvar_epi32(t, _mm512_test_epi32_mask(sel, _mm512_set1_epi32(64)), sel, *r4);
    return t;
}

/* The pair gathers and the integer bilinear blend. Returns two dwords of two 16-bit fields:
   lo = R | B << 16, hi = G | A << 16, every field in 8.8 FIXED POINT (0..65280 = 255 * 256).

   CRenderer shifted each lerp stage back to 8 bits, truncating twice: about -1 code of bias on
   every texel. Invisible on albedo; here it moved specular highlights by 20+ codes, because
   DOOM 3 does not renormalize its normal maps and the specular curve has a slope of up to 8
   (harness, 2026-09-21). A GPU keeps the filtered value at full precision, so this does too: the
   horizontal lerp is exact in 16 bits, the vertical one is two mulhi (error under 2/256 of a
   code), and the consumer rounds once. Same number of operations. */
static __forceinline VI tex_weight16(VI w)      /* a 0..256 weight as a mulhi multiplier, in both fields of a dword */
{
    const VI v = _mm512_min_epi32(_mm512_slli_epi32(w, 8), _mm512_set1_epi32(65535));
    return _mm512_or_si512(v, _mm512_slli_epi32(v, 16));
}
/* an 8.8 field rounded to its 8-bit code, both fields of a dword at once */
static __forceinline VI tex_round8(VI x) { return _mm512_srli_epi16(_mm512_add_epi16(x, _mm512_set1_epi16(128)), 8); }
static __forceinline void tex_fetch16_1(const uint32_t *texels, const TexCoords1 *tc, __mmask16 m, VI *lfin, VI *hfin)
{
    const VI zero = _mm512_setzero_si512();
    const VI evens = _mm512_setr_epi32(0,2,4,6,8,10,12,14,16,18,20,22,24,26,28,30);
    const VI odds  = _mm512_setr_epi32(1,3,5,7,9,11,13,15,17,19,21,23,25,27,29,31);
    const __mmask8 mlo = (__mmask8)m, mhi = (__mmask8)(m >> 8);
    VI t00, t01, t10, t11;
    if (tc->win == 2) {
        /* two rows hold every texel: one two-source permute per tap, the zero for lanes outside the mask included */
        const VI r0 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[0])), r1 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[1]));
        const VI one = _mm512_set1_epi32(1);
        t00 = _mm512_maskz_permutex2var_epi32(m, r0, tc->sel0, r1);
        t01 = _mm512_maskz_permutex2var_epi32(m, r0, _mm512_add_epi32(tc->sel0, one), r1);
        t10 = _mm512_maskz_permutex2var_epi32(m, r0, tc->sel1, r1);
        t11 = _mm512_maskz_permutex2var_epi32(m, r0, _mm512_add_epi32(tc->sel1, one), r1);
        goto blend;
    }
    if (tc->win == 3) {
        const VI r0 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[0])), r1 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[1]));
        const VI r2 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[2]));
        const VI one = _mm512_set1_epi32(1), s01 = _mm512_add_epi32(tc->sel0, one), s11 = _mm512_add_epi32(tc->sel1, one);
        /* row 2 = bit 5 of the selector; its lanes are taken from r2 by column (the low four bits) */
        const __mmask16 k0 = _mm512_mask_test_epi32_mask(m, tc->sel0, _mm512_set1_epi32(32)), k1 = _mm512_mask_test_epi32_mask(m, tc->sel1, _mm512_set1_epi32(32));
        t00 = _mm512_mask_permutexvar_epi32(_mm512_maskz_permutex2var_epi32(m, r0, tc->sel0, r1), k0, tc->sel0, r2);
        t01 = _mm512_mask_permutexvar_epi32(_mm512_maskz_permutex2var_epi32(m, r0, s01, r1), k0, s01, r2);
        t10 = _mm512_mask_permutexvar_epi32(_mm512_maskz_permutex2var_epi32(m, r0, tc->sel1, r1), k1, tc->sel1, r2);
        t11 = _mm512_mask_permutexvar_epi32(_mm512_maskz_permutex2var_epi32(m, r0, s11, r1), k1, s11, r2);
        goto blend;
    }
    if (tc->win) {
        /* the same four texels per lane as below, by address: texels[idx0], [idx0 + 1], [idx1], [idx1 + 1] */
        const VI r0 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[0])), r1 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[1]));
        const VI r2 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[2])), r3 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[3]));
        const VI r4 = _mm512_loadu_si512((const void *)(texels + tc->rowBase[4]));
        const VI one = _mm512_set1_epi32(1);
        t00 = tex_window_pick(tc->sel0, r0, r1, r2, r3, NULL);
        t01 = tex_window_pick(_mm512_add_epi32(tc->sel0, one), r0, r1, r2, r3, NULL);
        t10 = tex_window_pick(tc->sel1, r0, r1, r2, r3, &r4);
        t11 = tex_window_pick(_mm512_add_epi32(tc->sel1, one), r0, r1, r2, r3, &r4);
        /* A lane outside the mask reads ZERO from the gathers below, and one consumer can tell: bumpyEnvironment takes
           its cube level from differences between neighbouring lanes, masked or not (found by the byte comparison of
           demo frames: 4 of 23 differed). Same here, or the switch is not exact. */
        t00 = _mm512_maskz_mov_epi32(m, t00); t01 = _mm512_maskz_mov_epi32(m, t01);
        t10 = _mm512_maskz_mov_epi32(m, t10); t11 = _mm512_maskz_mov_epi32(m, t11);
        goto blend;
    }
#if SW_ORACLES          /* cut 6: what a lookup would cost if its texels came from two contiguous row loads and four
                           permutes instead of four gathers (WRONG image: every lane reads lane 0's neighbourhood) */
    if (g_opt[SW_OPT_KERNEL_CUT] == 6) {
        int b0 = _mm_cvtsi128_si32(_mm512_castsi512_si128(tc->idx0)), b1 = _mm_cvtsi128_si32(_mm512_castsi512_si128(tc->idx1));
        /* lane 0's real neighbourhood: a lane in the same row and within 16 texels of it reads its TRUE texel, so the
           early-outs fire as they should and the work downstream is the real work (oracle builds pad the chain for it) */
        const VI row0 = _mm512_loadu_si512((const void *)(texels + b0)), row0b = _mm512_loadu_si512((const void *)(texels + b0 + 16));
        const VI row1 = _mm512_loadu_si512((const void *)(texels + b1)), row1b = _mm512_loadu_si512((const void *)(texels + b1 + 16));
        const VI k = _mm512_and_si512(_mm512_sub_epi32(tc->idx0, _mm512_set1_epi32(b0)), _mm512_set1_epi32(15));
        const VI k1 = _mm512_add_epi32(k, _mm512_set1_epi32(1));
        VI o00 = _mm512_permutex2var_epi32(row0, k, row0b), o01 = _mm512_permutex2var_epi32(row0, k1, row0b);
        VI o10 = _mm512_permutex2var_epi32(row1, k, row1b), o11 = _mm512_permutex2var_epi32(row1, k1, row1b);
        const VI M = _mm512_set1_epi32(0x00FF00FF);
        const VI a1x = tc->fx, a0x = _mm512_sub_epi32(_mm512_set1_epi32(256), tc->fx);
        const VI a1y = tc->fy, a0y = _mm512_sub_epi32(_mm512_set1_epi32(256), tc->fy);
#define OLERP(s0, s1) _mm512_add_epi32(_mm512_mullo_epi32(s0, a0x), _mm512_mullo_epi32(s1, a1x))
        const VI lt = OLERP(_mm512_and_si512(o00, M), _mm512_and_si512(o01, M)), ht = OLERP(_mm512_srli_epi16(o00, 8), _mm512_srli_epi16(o01, 8));
        const VI lb = OLERP(_mm512_and_si512(o10, M), _mm512_and_si512(o11, M)), hb = OLERP(_mm512_srli_epi16(o10, 8), _mm512_srli_epi16(o11, 8));
#undef OLERP
        const VI u0 = tex_weight16(a0y), u1 = tex_weight16(a1y);
        *lfin = _mm512_add_epi16(_mm512_mulhi_epu16(lt, u0), _mm512_mulhi_epu16(lb, u1));
        *hfin = _mm512_add_epi16(_mm512_mulhi_epu16(ht, u0), _mm512_mulhi_epu16(hb, u1));
        return;
    }
#endif
    const VI p0l = _mm512_mask_i32gather_epi64(zero, mlo, _mm512_castsi512_si256(tc->idx0), texels, 4);
    const VI p0h = _mm512_mask_i32gather_epi64(zero, mhi, _mm512_extracti32x8_epi32(tc->idx0, 1), texels, 4);
    const VI p1l = _mm512_mask_i32gather_epi64(zero, mlo, _mm512_castsi512_si256(tc->idx1), texels, 4);
    const VI p1h = _mm512_mask_i32gather_epi64(zero, mhi, _mm512_extracti32x8_epi32(tc->idx1, 1), texels, 4);
    t00 = _mm512_permutex2var_epi32(p0l, evens, p0h); t01 = _mm512_permutex2var_epi32(p0l, odds, p0h);
    t10 = _mm512_permutex2var_epi32(p1l, evens, p1h); t11 = _mm512_permutex2var_epi32(p1l, odds, p1h);
    const __mmask16 fixw = tc->xw;
    if (fixw) {                                             /* u-wrap lanes read a stale right texel: re-gather (rare) */
        t01 = _mm512_mask_i32gather_epi32(t01, fixw, tc->idx0w, texels, 4);
        t11 = _mm512_mask_i32gather_epi32(t11, fixw, tc->idx1w, texels, 4);
    }
blend:;
    const VI M16 = _mm512_set1_epi32(0x00FF00FF);
    const VI w1x = tc->fx, w0x = _mm512_sub_epi32(_mm512_set1_epi32(256), tc->fx);
    const VI w1y = tc->fy, w0y = _mm512_sub_epi32(_mm512_set1_epi32(256), tc->fy);
    const VI l00 = _mm512_and_si512(t00, M16), h00 = _mm512_srli_epi16(t00, 8);
    const VI l01 = _mm512_and_si512(t01, M16), h01 = _mm512_srli_epi16(t01, 8);
    const VI l10 = _mm512_and_si512(t10, M16), h10 = _mm512_srli_epi16(t10, 8);
    const VI l11 = _mm512_and_si512(t11, M16), h11 = _mm512_srli_epi16(t11, 8);
    /* along x: s0 * w0 + s1 * w1 with w0 + w1 = 256 is at most 65280, exact in the 16-bit field */
#define LERPX(s0, s1) _mm512_add_epi32(_mm512_mullo_epi32(s0, w0x), _mm512_mullo_epi32(s1, w1x))
    const VI ltop = LERPX(l00, l01), htop = LERPX(h00, h01);
    const VI lbot = LERPX(l10, l11), hbot = LERPX(h10, h11);
#undef LERPX
    const VI wv0 = tex_weight16(w0y), wv1 = tex_weight16(w1y);
    *lfin = _mm512_add_epi16(_mm512_mulhi_epu16(ltop, wv0), _mm512_mulhi_epu16(lbot, wv1));
    *hfin = _mm512_add_epi16(_mm512_mulhi_epu16(htop, wv0), _mm512_mulhi_epu16(hbot, wv1));
}

/* A coordinate set is numerator planes over a denominator plane: (u/w, v/w) over 1/w for a
   texture coordinate, (s/w, t/w) over q/w for a projective one: the w cancels. This evaluates
   the quotient and, by the quotient rule, the level: d(u)/dx = (ugx - u * dgx) / den. */
/* level0: every lane reads level 0, proven. two: trilinear, and some lane sits between `level` and the next one
   by frac (0..256); then the lookup reads both levels. */
typedef struct TexSet { VF u, v; VI level; int level0; int two; VI frac; } TexSet;
static __forceinline void tex_set_rule(const SwImage *tx, VF un, VF vn, VF rden,
                                       VF ugx, VF ugy, VF vgx, VF vgy, VF dgx, VF dgy, TexSet *o, const int exactLevel)
{
    const VF u = _mm512_mul_ps(un, rden), v = _mm512_mul_ps(vn, rden);
    const VF tw = _mm512_mul_ps(rden, _mm512_set1_ps((float)tx->w0)), th = _mm512_mul_ps(rden, _mm512_set1_ps((float)tx->h0));
    const VF dudx = _mm512_mul_ps(tw, _mm512_fnmadd_ps(u, dgx, ugx)), dvdx = _mm512_mul_ps(th, _mm512_fnmadd_ps(v, dgx, vgx));
    const VF dudy = _mm512_mul_ps(tw, _mm512_fnmadd_ps(u, dgy, ugy)), dvdy = _mm512_mul_ps(th, _mm512_fnmadd_ps(v, dgy, vgy));
    const VF px2 = _mm512_fmadd_ps(dudx, dudx, _mm512_mul_ps(dvdx, dvdx));
    const VF py2 = _mm512_fmadd_ps(dudy, dudy, _mm512_mul_ps(dvdy, dvdy));
    o->u = u; o->v = v;
    /* rho^2, the squared footprint length that picks the level. SW_LOD_RULE:
         0  the specification's: the longer of the two screen-axis derivative vectors
         1  the major axis of the footprint ellipse (what rule 0 underestimates by up to sqrt 2 when
            the footprint is stretched diagonally on screen)
         2  the sum of the two lengths
       Measured against the reference GPU with image_colorMipLevels (CLAUDE.md, Stage 8): it follows
       rule 1. Against GL, demo1 frame 100 reads 44.2 dB with rule 0, 52.6 with rule 1, 37.3 with
       rule 2. DOOM 3 does not renormalize its normal maps, so a level too fine or too coarse
       changes the LENGTH of N and with it every highlight: the level choice is not cosmetic here. */
#ifndef SW_LOD_RULE
#define SW_LOD_RULE 1
#endif
    /* Level 0 without the rule: the level below is 0 exactly when rho^2 < 2 (its exponent arithmetic; a negative
       level clamps to 0). For the cheap rule rho^2 IS max(A, C). For the ellipse, major^2 <= A + C (the two axes'
       squares add up to it), and what is computed exceeds the true value by at most rsqrt14's 2^-14 on half of
       the sum: below 1.99 the computed value stays below 2. (Rule 0 is below A + C as well; rule 2 is at most twice
       it.) One lane at or over the bound, or a NaN (the ordered compare refuses it), sends the whole block through
       the rule as before. Exact; and it is the common case at 3840x2160: every lookup up to 1.4 texels per
       pixel, all smoke, the light's own textures. */
    o->level0 = 0;
    if (g_texLevel0) {
        /* trilinear reads level 0 ALONE only while the footprint is at most one texel: rho^2 <= 1 */
#if SW_LOD_RULE == 2
        const float bound = g_texTrilinear ? (exactLevel ? 0.49f : 1.0f) : (exactLevel ? 0.99f : 2.0f);
#else
        const float bound = g_texTrilinear ? (exactLevel ? 0.99f : 1.0f) : (exactLevel ? 1.99f : 2.0f);
#endif
        o->two = 0;
        if (tx->nmips <= 1 || _mm512_cmp_ps_mask(exactLevel ? _mm512_add_ps(px2, py2) : _mm512_max_ps(px2, py2),
                                                 _mm512_set1_ps(bound), _CMP_LT_OQ) == 0xFFFF) {
            o->level = _mm512_setzero_si512(); o->level0 = 1;
            return;
        }
    }
    VF rho2;
    if (!exactLevel) rho2 = _mm512_max_ps(px2, py2);
    else {
#if SW_LOD_RULE == 1
    /* major^2 = ((A + C) + sqrt((A - C)^2 + 4 B^2)) / 2. The root through rsqrt14 (x * rsqrt(x), relative
       error 2^-14): a true sqrt here cost 3% of the 4K frame, and the level only reads the exponent of
       rho^2 and its top mantissa bit. */
    const VF bxy = _mm512_fmadd_ps(dudx, dudy, _mm512_mul_ps(dvdx, dvdy));
    const VF dif = _mm512_sub_ps(px2, py2);
    const VF disc = _mm512_max_ps(_mm512_fmadd_ps(dif, dif, _mm512_mul_ps(_mm512_set1_ps(4.0f), _mm512_mul_ps(bxy, bxy))), _mm512_set1_ps(1e-30f));
    rho2 = _mm512_mul_ps(_mm512_set1_ps(0.5f), _mm512_add_ps(_mm512_add_ps(px2, py2), _mm512_mul_ps(disc, _mm512_rsqrt14_ps(disc))));
#elif SW_LOD_RULE == 2
    rho2 = _mm512_add_ps(_mm512_add_ps(px2, py2), _mm512_mul_ps(_mm512_set1_ps(2.0f), _mm512_sqrt_ps(_mm512_mul_ps(px2, py2))));
#else
    rho2 = _mm512_max_ps(px2, py2);
#endif
    }
    o->two = 0;
    if (g_texTrilinear && tx->nmips > 1) {
        /* GL_LINEAR_MIPMAP_LINEAR: lambda = log2(rho) = log2(rho^2) / 2, levels floor(lambda) and the next one, blended
           by the fraction; at or below 0 it is level 0 alone (the magnification filter), at the last level that
           one alone. log2 = the exponent + a cubic of the mantissa (worst error 0.13 / 256 of a level: a GPU keeps
           the fraction in 8 bits or fewer). */
        const VF r2 = _mm512_max_ps(rho2, _mm512_set1_ps(1e-12f));
        const VF x = _mm512_sub_ps(_mm512_getmant_ps(r2, _MM_MANT_NORM_1_2, _MM_MANT_SIGN_zero), _mm512_set1_ps(1.0f));
        const VF l2m = _mm512_mul_ps(x, _mm512_fmadd_ps(x, _mm512_fmadd_ps(x, _mm512_set1_ps(0.1563861f), _mm512_set1_ps(-0.5772507f)), _mm512_set1_ps(1.4208645f)));
        VF lam = _mm512_mul_ps(_mm512_set1_ps(0.5f), _mm512_add_ps(_mm512_getexp_ps(r2), l2m));
        lam = _mm512_min_ps(_mm512_max_ps(lam, _mm512_setzero_ps()), _mm512_set1_ps((float)(tx->nmips - 1)));
        const VF base = _mm512_floor_ps(lam);
        o->level = _mm512_cvttps_epi32(base);
        o->frac = _mm512_cvtps_epi32(_mm512_mul_ps(_mm512_sub_ps(lam, base), _mm512_set1_ps(256.0f)));
        o->two = _mm512_test_epi32_mask(o->frac, o->frac) != 0;
        return;
    }
    o->level = tx->nmips > 1 ? tex_level_from_rho2(rho2) : _mm512_setzero_si512();
}

/* ---- the two-level front: what the kernels use ---- */
typedef struct TexCoords { TexCoords1 a, b; int two; VI w0, w1; } TexCoords;

static __forceinline void tex_coords(const SwImage *tx, const TexSet *s, __mmask16 m, TexCoords *tc)
{
    TexLevel L;
    tex_level(tx, s->level, s->level0, &L);
    if (tx->wrap == SW_WRAP_REPEAT) tex_coords_at(&L, s->u, s->v, m, SW_WRAP_REPEAT, &tc->a);
    else tex_coords_at(&L, s->u, s->v, m, SW_WRAP_CLAMP, &tc->a);
    tc->two = s->two;
    if (s->two) {
        tex_level(tx, _mm512_add_epi32(s->level, _mm512_set1_epi32(1)), 0, &L);     /* past the chain it clamps to the last level, where frac is 0 */
        if (tx->wrap == SW_WRAP_REPEAT) tex_coords_at(&L, s->u, s->v, m, SW_WRAP_REPEAT, &tc->b);
        else tex_coords_at(&L, s->u, s->v, m, SW_WRAP_CLAMP, &tc->b);
        tc->w0 = tex_weight16(_mm512_sub_epi32(_mm512_set1_epi32(256), s->frac));
        tc->w1 = tex_weight16(s->frac);
    }
}

static __forceinline void tex_fetch16(const uint32_t *texels, const TexCoords *tc, __mmask16 m, VI *lfin, VI *hfin)
{
    tex_fetch16_1(texels, &tc->a, m, lfin, hfin);
    if (tc->two) {
        VI l1, h1;
        tex_fetch16_1(texels, &tc->b, m, &l1, &h1);
        *lfin = _mm512_add_epi16(_mm512_mulhi_epu16(*lfin, tc->w0), _mm512_mulhi_epu16(l1, tc->w1));
        *hfin = _mm512_add_epi16(_mm512_mulhi_epu16(*hfin, tc->w0), _mm512_mulhi_epu16(h1, tc->w1));
    }
}

/* a surface map: the level the GPU would pick, exactly. The rule costs 3.4% of a 4K frame when every
   lookup pays it (same session, two builds interleaved: 48.9 against 47.25 fps) ... */
static __forceinline void tex_set(const SwImage *tx, VF un, VF vn, VF rden,
                                  VF ugx, VF ugy, VF vgx, VF vgy, VF dgx, VF dgy, TexSet *o)
{
    tex_set_rule(tx, un, vn, rden, ugx, ugy, vgx, vgy, dgx, dgy, o, 1);
}

/* ... so the light's own images take the cheap rule: projection, fog and blend-light textures are
   smooth blobs, magnified on screen, where both rules read level 0. */
static __forceinline void tex_set_smooth(const SwImage *tx, VF un, VF vn, VF rden,
                                         VF ugx, VF ugy, VF vgx, VF vgy, VF dgx, VF dgy, TexSet *o)
{
    tex_set_rule(tx, un, vn, rden, ugx, ugy, vgx, vgy, dgx, dgy, o, 0);
}

/* one bilinear sample of an image at a coordinate set; the wrap branch is per call, not per lane */
static __forceinline void tex_sample16(const SwImage *tx, const TexSet *s, __mmask16 m, VI *lo, VI *hi)
{
    TexCoords tc;
    tex_coords(tx, s, m, &tc);
    tex_fetch16(tx->texels, &tc, m, lo, hi);
}

/* A cube map lookup: GL's major-axis face selection per lane, then the 2D path with the face's
   base offset added to the texel indices (the same trick as the per-lane mip tables).

     face   sc    tc    ma          s = (sc / |ma| + 1) / 2,  t = (tc / |ma| + 1) / 2
     +X    -rz   -ry    rx
     -X    +rz   -ry    rx
     +Y    +rx   +rz    ry
     -Y    +rx   -rz    ry
     +Z    +rx   -ry    rz
     -Z    -rx   -ry    rz

   The level comes from finite differences of the direction inside the block, against the x and
   the y neighbour of each lane's 2x2 quad, which is what a GPU does; GL leaves cube LOD loose. */
static __forceinline void tex_cube_sample16(const SwImage *tx, VF rx, VF ry, VF rz, __mmask16 m, VI *lo, VI *hi)
{
    const VF sign = _mm512_set1_ps(-0.0f), zero = _mm512_setzero_ps(), half = _mm512_set1_ps(0.5f);
    const VF ax = _mm512_andnot_ps(sign, rx), ay = _mm512_andnot_ps(sign, ry), az = _mm512_andnot_ps(sign, rz);
    const __mmask16 isX = _mm512_cmp_ps_mask(ax, ay, _CMP_GE_OQ) & _mm512_cmp_ps_mask(ax, az, _CMP_GE_OQ);
    const __mmask16 isY = (__mmask16)(~isX) & _mm512_cmp_ps_mask(ay, az, _CMP_GE_OQ);
    const __mmask16 isZ = (__mmask16)~(isX | isY);
    const __mmask16 negX = _mm512_cmp_ps_mask(rx, zero, _CMP_LT_OQ), negY = _mm512_cmp_ps_mask(ry, zero, _CMP_LT_OQ), negZ = _mm512_cmp_ps_mask(rz, zero, _CMP_LT_OQ);
    const VF nrx = _mm512_xor_ps(rx, sign), nry = _mm512_xor_ps(ry, sign), nrz = _mm512_xor_ps(rz, sign);

    VF ma = _mm512_mask_blend_ps(isY, _mm512_mask_blend_ps(isX, az, ax), ay);
    /* sc: X faces -rz / +rz, Y faces +rx, Z faces +rx / -rx */
    VF sc = _mm512_mask_blend_ps(negZ, rx, nrx);                            /* Z faces */
    sc = _mm512_mask_blend_ps(isY, sc, rx);
    sc = _mm512_mask_blend_ps(isX, sc, _mm512_mask_blend_ps(negX, nrz, rz));
    /* tc: -ry everywhere except the Y faces, +rz / -rz */
    VF tc = _mm512_mask_blend_ps(isY, nry, _mm512_mask_blend_ps(negY, rz, nrz));
    VI face = _mm512_mask_blend_epi32(negZ, _mm512_set1_epi32(4), _mm512_set1_epi32(5));
    face = _mm512_mask_blend_epi32(isY, face, _mm512_mask_blend_epi32(negY, _mm512_set1_epi32(2), _mm512_set1_epi32(3)));
    face = _mm512_mask_blend_epi32(isX, face, _mm512_mask_blend_epi32(negX, _mm512_set1_epi32(0), _mm512_set1_epi32(1)));
    (void)isZ;

    const VF rma = v_rcp(_mm512_max_ps(ma, _mm512_set1_ps(1e-30f)));
    TexSet ts;
    ts.u = _mm512_fmadd_ps(_mm512_mul_ps(sc, rma), half, half);
    ts.v = _mm512_fmadd_ps(_mm512_mul_ps(tc, rma), half, half);
    if (tx->nmips > 1) {
        const VI px = _mm512_setr_epi32(1,0,3,2, 5,4,7,6, 9,8,11,10, 13,12,15,14);      /* the quad's x neighbour */
        const VI py = _mm512_setr_epi32(4,5,6,7, 0,1,2,3, 12,13,14,15, 8,9,10,11);      /* its y neighbour */
        const VF dxx = _mm512_sub_ps(rx, _mm512_permutexvar_ps(px, rx)), dxy = _mm512_sub_ps(ry, _mm512_permutexvar_ps(px, ry)), dxz = _mm512_sub_ps(rz, _mm512_permutexvar_ps(px, rz));
        const VF dyx = _mm512_sub_ps(rx, _mm512_permutexvar_ps(py, rx)), dyy = _mm512_sub_ps(ry, _mm512_permutexvar_ps(py, ry)), dyz = _mm512_sub_ps(rz, _mm512_permutexvar_ps(py, rz));
        const VF lx2 = _mm512_fmadd_ps(dxx, dxx, _mm512_fmadd_ps(dxy, dxy, _mm512_mul_ps(dxz, dxz)));
        const VF ly2 = _mm512_fmadd_ps(dyx, dyx, _mm512_fmadd_ps(dyy, dyy, _mm512_mul_ps(dyz, dyz)));
        const VF k = _mm512_mul_ps(rma, _mm512_set1_ps(0.5f * (float)tx->w0));          /* direction units -> texels */
        ts.level = tex_level_from_rho2(_mm512_mul_ps(_mm512_max_ps(lx2, ly2), _mm512_mul_ps(k, k)));
    } else
        ts.level = _mm512_setzero_si512();

    TexLevel L; TexCoords1 c;
    tex_level(tx, ts.level, 0, &L);
    tex_coords_at(&L, ts.u, ts.v, m, SW_WRAP_CLAMP, &c);
    const VI fbase = _mm512_mullo_epi32(face, _mm512_set1_epi32(tx->faceTexels));
    c.idx0 = _mm512_add_epi32(c.idx0, fbase); c.idx1 = _mm512_add_epi32(c.idx1, fbase);
    c.win = 0;                                              /* the lanes may sit in different faces: the window knows one chain */
    tex_fetch16_1(tx->texels, &c, m, lo, hi);
}
