/* kern/k_interaction.c -- the fragment half of interaction.vfp (plan, Appendix A.1), one light
   pass over one 4x4 block. As shipped, except where the plan decided otherwise:
   - the normalization cube map and the specular table are replaced by their math (decision 8:
     at most one 8-bit code of deviation);
   - nothing is recomputed per pixel that the vertex program computes per vertex (decision 1).

     Ln    = normalize(tc0)                     (an ambient light: a constant vector)
     N     = bump(tc1), x taken from ALPHA, * 2 - 1, NOT renormalized
     light = (Ln . N) * projection(tc3.xy / tc3.w) * falloff(tc2.x, 0.5)      all four channels
     spec  = saturate((normalize(tc6) . N - 0.75) * 4) ^ 2
     out   = (diffuse(tc4) * env0 + spec * env1 * 2 * specular(tc5)) * light * colour, clamped

   The early-outs are EXACT, because a zero factor zeroes the whole product and a negative one
   clamps to zero: lanes behind the light (q <= 0), lanes where falloff or projection is black
   (most of a light's scissor rectangle lies outside its volume), lanes with Ln . N <= 0. Falloff
   and projection are sampled first, the three surface maps only for the lanes that are left.
   The shade pass costs per CALL, not per lane (CRenderer): what pays is dropping whole blocks. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

/* SW_ORACLES=1 compiles the wrong-image, right-cost oracles in (SW_OPT_KERNEL_CUT). Off by
   default: CRenderer's rule is to #if out what a configuration does not use. */
#ifndef SW_ORACLES
#define SW_ORACLES 0
#endif

enum {
    IF_SPECULAR   = 1,      /* the specular colour is not black and there is a specular map */
    IF_VCOLOR     = 2,      /* the vertex colour is not a constant */
    IF_SHARE_DIFF = 4,      /* diffuse reads at the bump map's texel coordinates (same matrix, same size) */
    IF_SHARE_SPEC = 8
};

static int same_coords(const float m0[2][4], const float m1[2][4], const SwImage *a, const SwImage *b)
{
    return a && b && memcmp(m0, m1, sizeof(float) * 8) == 0 &&
           a->w0 == b->w0 && a->h0 == b->h0 && a->nmips == b->nmips && a->wrap == b->wrap;
}

static int interaction_flags(const SwInteractionParms *p)
{
    int f = 0;
    if (p->specular && (p->specularColor[0] != 0.0f || p->specularColor[1] != 0.0f || p->specularColor[2] != 0.0f || p->specularColor[3] != 0.0f))
        f |= IF_SPECULAR;
    if (p->vertexColorModulate != 0.0f) f |= IF_VCOLOR;
    if (same_coords(p->bumpMatrix, p->diffuseMatrix, p->bump, p->diffuse)) f |= IF_SHARE_DIFF;
    if (same_coords(p->bumpMatrix, p->specularMatrix, p->bump, p->specular)) f |= IF_SHARE_SPEC;
    return f;
}

#define GRAD(a, k, c) _mm512_set1_ps((a)->p[k][c])

/* Is a whole 16x16 cell outside the light volume? Exact, not a heuristic: falloff's coordinate is
   F/W and the projection's are S/Q and T/Q, ratios of planes that are linear on screen, and a
   ratio of two linear functions takes its extremes over a rectangle at the corners as long as the
   denominator keeps its sign. Where a coordinate is at or beyond 0 or 1 on all four corners, every
   clamped bilinear lookup in the cell reads border texels only, and the caller has checked that
   those are black on every level (SwImage.zeroBorder). (xl, yl) = the cell's first pixel centre in
   the triangle's local frame. A dark block costs 80% of a lit one (harness, 2026-09-21): what
   pays is never asking. */
static __forceinline int light_cell_dark(const SwTriAttr *a, float xl, float yl, int falloffZero, int projZero)
{
    float w[4], q[4], f[4], s[4], t[4];
    for (int i = 0; i < 4; i++) {
        const float x = xl + ((i & 1) ? 15.0f : 0.0f), y = yl + ((i & 2) ? 15.0f : 0.0f);
        w[i] = a->w[0] + a->w[1] * x + a->w[2] * y;
        q[i] = a->p[VI_PROJ_Q][0] + a->p[VI_PROJ_Q][1] * x + a->p[VI_PROJ_Q][2] * y;
        f[i] = a->p[VI_FALLOFF][0] + a->p[VI_FALLOFF][1] * x + a->p[VI_FALLOFF][2] * y;
        s[i] = a->p[VI_PROJ_S][0] + a->p[VI_PROJ_S][1] * x + a->p[VI_PROJ_S][2] * y;
        t[i] = a->p[VI_PROJ_T][0] + a->p[VI_PROJ_T][1] * x + a->p[VI_PROJ_T][2] * y;
    }
    /* behind the light everywhere: the kernel drops q <= 0 lanes. The margins below keep float
       rounding of these scalar planes against the kernel's vector evaluation on the safe side. */
    if (q[0] < 0 && q[1] < 0 && q[2] < 0 && q[3] < 0) return 1;
    if (!(w[0] > 0 && w[1] > 0 && w[2] > 0 && w[3] > 0)) return 0;
    const float e = 1e-4f;
    if (falloffZero) {                                      /* u = f / w with w > 0:  u <= 0  <=>  f <= 0,  u >= 1  <=>  f >= w */
        if (f[0] < -e * w[0] && f[1] < -e * w[1] && f[2] < -e * w[2] && f[3] < -e * w[3]) return 1;
        if (f[0] > w[0] * (1 + e) && f[1] > w[1] * (1 + e) && f[2] > w[2] * (1 + e) && f[3] > w[3] * (1 + e)) return 1;
    }
    if (projZero && q[0] > 0 && q[1] > 0 && q[2] > 0 && q[3] > 0) {
        if (s[0] < -e * q[0] && s[1] < -e * q[1] && s[2] < -e * q[2] && s[3] < -e * q[3]) return 1;
        if (s[0] > q[0] * (1 + e) && s[1] > q[1] * (1 + e) && s[2] > q[2] * (1 + e) && s[3] > q[3] * (1 + e)) return 1;
        if (t[0] < -e * q[0] && t[1] < -e * q[1] && t[2] < -e * q[2] && t[3] < -e * q[3]) return 1;
        if (t[0] > q[0] * (1 + e) && t[1] > q[1] * (1 + e) && t[2] > q[2] * (1 + e) && t[3] > q[3] * (1 + e)) return 1;
    }
    return 0;
}

static __forceinline void coords_for(const SwImage *tx, const TexSet *s, __mmask16 m, TexCoords *tc)
{
    tex_coords(tx, s, m, tc);
}

/* a 16-bit field -> float. The fetch's fields are 8.8 (texel * 256); the scale is folded into the
   constants at the use. */
static __forceinline VF field_lo(VI x) { return _mm512_cvtepi32_ps(_mm512_and_si512(x, _mm512_set1_epi32(0xFFFF))); }
static __forceinline VF field_hi(VI x) { return _mm512_cvtepi32_ps(_mm512_srli_epi32(x, 16)); }

/* KERNEL_CALL: see core/simd.h */
static KERNEL_CALL(__mmask16) k_interaction(const SwTriAttr *a, const SwInteractionParms *p, const int flags, const int cutArg,
                                             __mmask16 m, VF X, VF Y, VI *out, SwThreadStats *st)
{
    const __mmask16 mIn = m;
    const VF zero = _mm512_setzero_ps();
#if SW_ORACLES
    const int cut = cutArg;
#else
    const int cut = 0; (void)cutArg;
#endif
    VI lo, hi;
    if (cut == 1) goto dark;

    /* ---- the light's own two textures first ---- */
    const VF Q = PLANE(a, VI_PROJ_Q, X, Y);
    m = _mm512_mask_cmp_ps_mask(m, Q, zero, _CMP_GT_OQ);           /* behind the light: nothing */
    if (!m) goto dark;
    const VF rw = v_rcp(PLANE_W(a, X, Y));
    {
        TexSet fs;                                                  /* falloff at (tc2.x, 0.5), level 0: a smooth ramp */
        fs.u = _mm512_mul_ps(PLANE(a, VI_FALLOFF, X, Y), rw); fs.v = _mm512_set1_ps(0.5f); fs.level = _mm512_setzero_si512(); fs.level0 = g_texLevel0; fs.two = 0;
        tex_sample16(p->falloff, &fs, m, &lo, &hi);
    }
    m = _mm512_mask_test_epi32_mask(m, _mm512_or_si512(lo, hi), _mm512_set1_epi32(-1));
    if (!m || cut == 2) goto dark;
    VI lightLo = lo, lightHi = hi;
    {
        TexSet ps;                                                  /* projective: (s/w, t/w) over q/w, the w cancels */
        tex_set_smooth(p->projection, PLANE(a, VI_PROJ_S, X, Y), PLANE(a, VI_PROJ_T, X, Y), v_rcp(Q),
                GRAD(a, VI_PROJ_S, 1), GRAD(a, VI_PROJ_S, 2), GRAD(a, VI_PROJ_T, 1), GRAD(a, VI_PROJ_T, 2),
                GRAD(a, VI_PROJ_Q, 1), GRAD(a, VI_PROJ_Q, 2), &ps);
        tex_sample16(p->projection, &ps, m, &lo, &hi);
    }
    /* projection * falloff per channel, still in the 16-bit fields: 8.8 times 8.8 >> 16 is the
       product of the two texels in units of 1/255^2, at most 65025 */
    lightLo = _mm512_mulhi_epu16(lightLo, lo); lightHi = _mm512_mulhi_epu16(lightHi, hi);
    m = _mm512_mask_test_epi32_mask(m, _mm512_or_si512(lightLo, lightHi), _mm512_set1_epi32(-1));
    if (!m || cut == 3) goto dark;
#if SW_ORACLES
    st->ltCallsByFlags[flags & 15]++; st->ltLanesLit += (unsigned)_mm_popcnt_u32(m);
#endif

    /* ---- the bump map and N . L ---- */
    TexSet bs; TexCoords bc;
    if (cut == 5) { memset(&bc, 0, sizeof(bc)); lo = _mm512_set1_epi32((int)0x80008000u); hi = lo; goto bumpDone; }
    tex_set(p->bump, PLANE(a, VI_BUMP_U, X, Y), PLANE(a, VI_BUMP_V, X, Y), rw,
            GRAD(a, VI_BUMP_U, 1), GRAD(a, VI_BUMP_U, 2), GRAD(a, VI_BUMP_V, 1), GRAD(a, VI_BUMP_V, 2),
            _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &bs);
    coords_for(p->bump, &bs, m, &bc);
    tex_fetch16(p->bump->texels, &bc, m, &lo, &hi);
bumpDone:;
    const VF two255 = _mm512_set1_ps(2.0f / (255.0f * 256.0f)), one = _mm512_set1_ps(1.0f);
    const VF nx = _mm512_fmsub_ps(field_hi(hi), two255, one);       /* x from ALPHA */
    const VF ny = _mm512_fmsub_ps(field_lo(hi), two255, one);       /* G */
    const VF nz = _mm512_fmsub_ps(field_hi(lo), two255, one);       /* B */
    VF ndl;
    if (p->ambientLight) {
        ndl = _mm512_fmadd_ps(nx, _mm512_set1_ps(p->ambientVector[0]),
              _mm512_fmadd_ps(ny, _mm512_set1_ps(p->ambientVector[1]), _mm512_mul_ps(nz, _mm512_set1_ps(p->ambientVector[2]))));
    } else {
        /* normalize(tc0): tc0 is (plane) * w and the normalization cancels the w */
        const VF lx = PLANE(a, VI_L, X, Y), ly = PLANE(a, VI_L + 1, X, Y), lz = PLANE(a, VI_L + 2, X, Y);
        const VF rl = v_rsqrt(_mm512_max_ps(_mm512_fmadd_ps(lx, lx, _mm512_fmadd_ps(ly, ly, _mm512_mul_ps(lz, lz))), _mm512_set1_ps(1e-30f)));
        ndl = _mm512_mul_ps(rl, _mm512_fmadd_ps(nx, lx, _mm512_fmadd_ps(ny, ly, _mm512_mul_ps(nz, lz))));
    }
    m = _mm512_mask_cmp_ps_mask(m, ndl, zero, _CMP_GT_OQ);         /* a negative product clamps to 0 */
    if (!m || cut == 4) goto dark;
#if SW_ORACLES
    st->ltLanesFacing += (unsigned)_mm_popcnt_u32(m);
#endif

    /* ---- the surface colour ---- */
    if (cut == 5) { lo = hi = _mm512_set1_epi32((int)0x80008000u); }
    else if (flags & IF_SHARE_DIFF) tex_fetch16(p->diffuse->texels, &bc, m, &lo, &hi);
    else {
        TexSet ds;
        tex_set(p->diffuse, PLANE(a, VI_DIFF_U, X, Y), PLANE(a, VI_DIFF_V, X, Y), rw,
                GRAD(a, VI_DIFF_U, 1), GRAD(a, VI_DIFF_U, 2), GRAD(a, VI_DIFF_V, 1), GRAD(a, VI_DIFF_V, 2),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &ds);
        tex_sample16(p->diffuse, &ds, m, &lo, &hi);
    }
    const float i255 = 1.0f / (255.0f * 256.0f);
    VF cr = _mm512_mul_ps(field_lo(lo), _mm512_set1_ps(p->diffuseColor[0] * i255));
    VF cg = _mm512_mul_ps(field_lo(hi), _mm512_set1_ps(p->diffuseColor[1] * i255));
    VF cb = _mm512_mul_ps(field_hi(lo), _mm512_set1_ps(p->diffuseColor[2] * i255));
    VF ca = _mm512_mul_ps(field_hi(hi), _mm512_set1_ps(p->diffuseColor[3] * i255));

    if (flags & IF_SPECULAR) {
        const VF hx = PLANE(a, VI_H, X, Y), hy = PLANE(a, VI_H + 1, X, Y), hz = PLANE(a, VI_H + 2, X, Y);
        const VF rh = v_rsqrt(_mm512_max_ps(_mm512_fmadd_ps(hx, hx, _mm512_fmadd_ps(hy, hy, _mm512_mul_ps(hz, hz))), _mm512_set1_ps(1e-30f)));
        const VF ndh = _mm512_mul_ps(rh, _mm512_fmadd_ps(nx, hx, _mm512_fmadd_ps(ny, hy, _mm512_mul_ps(nz, hz))));
        /* the specular table: saturate((x - 0.75) * 4) ^ 2 */
        VF sp = _mm512_min_ps(_mm512_max_ps(_mm512_fmsub_ps(ndh, _mm512_set1_ps(4.0f), _mm512_set1_ps(3.0f)), zero),
                              _mm512_set1_ps(p->specularMax > 0.0f ? p->specularMax : 1.0f));
        sp = _mm512_mul_ps(sp, sp);
        const __mmask16 ms = _mm512_mask_cmp_ps_mask(m, sp, zero, _CMP_GT_OQ);
        if (ms) {                                                   /* the map only where the highlight is */
#if SW_ORACLES
            st->ltLanesSpec += (unsigned)_mm_popcnt_u32(ms);
#endif
            if (cut == 5) { lo = hi = _mm512_set1_epi32((int)0x80008000u); }
            else if (flags & IF_SHARE_SPEC) tex_fetch16(p->specular->texels, &bc, ms, &lo, &hi);
            else {
                TexSet ss;
                tex_set(p->specular, PLANE(a, VI_SPEC_U, X, Y), PLANE(a, VI_SPEC_V, X, Y), rw,
                        GRAD(a, VI_SPEC_U, 1), GRAD(a, VI_SPEC_U, 2), GRAD(a, VI_SPEC_V, 1), GRAD(a, VI_SPEC_V, 2),
                        _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &ss);
                tex_sample16(p->specular, &ss, ms, &lo, &hi);
            }
            const VF s2 = _mm512_mul_ps(sp, _mm512_set1_ps(2.0f * i255));   /* spec * 2 * map / 255; unfetched lanes are 0 * 0 */
            cr = _mm512_fmadd_ps(_mm512_mul_ps(field_lo(lo), s2), _mm512_set1_ps(p->specularColor[0]), cr);
            cg = _mm512_fmadd_ps(_mm512_mul_ps(field_lo(hi), s2), _mm512_set1_ps(p->specularColor[1]), cg);
            cb = _mm512_fmadd_ps(_mm512_mul_ps(field_hi(lo), s2), _mm512_set1_ps(p->specularColor[2]), cb);
            ca = _mm512_fmadd_ps(_mm512_mul_ps(field_hi(hi), s2), _mm512_set1_ps(p->specularColor[3]), ca);
        }
    }

    /* ---- times the light, times the vertex colour, to bytes ---- */
    {
        const VF ls = _mm512_mul_ps(ndl, _mm512_set1_ps(255.0f / 65025.0f));  /* N.L, the fields' 1/255^2, the output's 255 */
        VF r = _mm512_mul_ps(_mm512_mul_ps(cr, ls), field_lo(lightLo));
        VF g = _mm512_mul_ps(_mm512_mul_ps(cg, ls), field_lo(lightHi));
        VF b = _mm512_mul_ps(_mm512_mul_ps(cb, ls), field_hi(lightLo));
        VF al = _mm512_mul_ps(_mm512_mul_ps(ca, ls), field_hi(lightHi));
        if (flags & IF_VCOLOR) {
            r = _mm512_mul_ps(r, _mm512_mul_ps(PLANE(a, VI_COLOR, X, Y), rw));
            g = _mm512_mul_ps(g, _mm512_mul_ps(PLANE(a, VI_COLOR + 1, X, Y), rw));
            b = _mm512_mul_ps(b, _mm512_mul_ps(PLANE(a, VI_COLOR + 2, X, Y), rw));
            al = _mm512_mul_ps(al, _mm512_mul_ps(PLANE(a, VI_COLOR + 3, X, Y), rw));
        } else if (p->vertexColorAdd != 1.0f) {
            const VF k = _mm512_set1_ps(p->vertexColorAdd);
            r = _mm512_mul_ps(r, k); g = _mm512_mul_ps(g, k); b = _mm512_mul_ps(b, k); al = _mm512_mul_ps(al, k);
        }
        const VF top = _mm512_set1_ps(255.0f);
        const VI ri = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(r, zero), top));
        const VI gi = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(g, zero), top));
        const VI bi = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(b, zero), top));
        const VI ai = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(al, zero), top));
        *out = _mm512_or_si512(_mm512_or_si512(ri, _mm512_slli_epi32(gi, 8)),
                               _mm512_or_si512(_mm512_slli_epi32(bi, 16), _mm512_slli_epi32(ai, 24)));
    }
    st->lightLanesDark += (unsigned)_mm_popcnt_u32((unsigned)(mIn & ~m));
    return m;

dark:
    st->lightBlocksDark++;
    st->lightLanesDark += (unsigned)_mm_popcnt_u32(mIn);
    return 0;
}
