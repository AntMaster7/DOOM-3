/* kern/k_misc.c -- the fragment halves of everything that is neither a plain stage nor a light
   interaction (plan, Stage 7):

   k_dual     colour x tex0(s/q, t/q) x tex1(s, t), coordinates linear in OBJECT space: what the
              fixed-function texgens of RB_FogPass, RB_BlendLight and TG_SCREEN compute.
   k_env      environment.vfp (reflection of the eye about the vertex normal, LOCAL space, times
              the colour) and bumpyEnvironment.vfp (about the bump-mapped normal, GLOBAL space,
              no colour), both into a cube map.
   k_screen   heatHaze.vfp, heatHazeWithMask.vfp, heatHazeWithMaskAndVertex.vfp, colorProcess.vfp:
              the programs that read _currentRender, here the view's CAPTURE (sw_capture_point).
              Their coordinate arithmetic (fragment.position * env[1] * env[0]) is "this pixel, in
              texels of a power-of-two copy"; against a capture of the viewport's own size it is
              (pixel centre) / (viewport size), and the y offset changes sign because the frame
              is stored top row first.

   These run on a small share of a frame's pixels: clarity over the last nanosecond. They are
   NOT inlined into raster_tri: inlined, the compiler died there (C1001, toolset 14.51), and a
   call per 4x4 block is nothing next to three texture lookups. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

static __forceinline VI pack_rgba_ps(VF r, VF g, VF b, VF a)       /* 0..255 floats to a pixel, clamped, rounded */
{
    const VF zero = _mm512_setzero_ps(), top = _mm512_set1_ps(255.0f);
    const VI ri = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(r, zero), top));
    const VI gi = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(g, zero), top));
    const VI bi = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(b, zero), top));
    const VI ai = _mm512_cvtps_epi32(_mm512_min_ps(_mm512_max_ps(a, zero), top));
    return _mm512_or_si512(_mm512_or_si512(ri, _mm512_slli_epi32(gi, 8)),
                           _mm512_or_si512(_mm512_slli_epi32(bi, 16), _mm512_slli_epi32(ai, 24)));
}

/* ---- k_dual ------------------------------------------------------------------ */
static KERNEL_CALL(__mmask16) k_dual(const SwTriAttr *a, const SwDualParms *p, __mmask16 m, VF X, VF Y, VI *out)
{
    const VF zero = _mm512_setzero_ps();
    const VF rw = v_rcp(PLANE_W(a, X, Y));
    const float i255 = 1.0f / 256.0f;                               /* an 8.8 field to 0..255 */
    VF r = _mm512_set1_ps(255.0f * p->color[0]), g = _mm512_set1_ps(255.0f * p->color[1]);
    VF b = _mm512_set1_ps(255.0f * p->color[2]), al = _mm512_set1_ps(255.0f * p->color[3]);
    VI lo, hi;
    if (p->image0) {
        const VF Q = PLANE(a, VD_Q0, X, Y);
        m = _mm512_mask_cmp_ps_mask(m, Q, zero, _CMP_GT_OQ);       /* behind the projection: GL's q <= 0 is undefined; nothing */
        if (!m) return 0;
        TexSet s0;
        tex_set_smooth(p->image0, PLANE(a, VD_S0, X, Y), PLANE(a, VD_T0, X, Y), v_rcp(Q),
                GRAD(a, VD_S0, 1), GRAD(a, VD_S0, 2), GRAD(a, VD_T0, 1), GRAD(a, VD_T0, 2),
                GRAD(a, VD_Q0, 1), GRAD(a, VD_Q0, 2), &s0);
        tex_sample16(p->image0, &s0, m, &lo, &hi);
        const VF k = _mm512_set1_ps(i255 / 255.0f);
        r = _mm512_mul_ps(r, _mm512_mul_ps(field_lo(lo), k)); g = _mm512_mul_ps(g, _mm512_mul_ps(field_lo(hi), k));
        b = _mm512_mul_ps(b, _mm512_mul_ps(field_hi(lo), k)); al = _mm512_mul_ps(al, _mm512_mul_ps(field_hi(hi), k));
    }
    if (p->image1) {
        TexSet s1;
        tex_set_smooth(p->image1, PLANE(a, VD_S1, X, Y), PLANE(a, VD_T1, X, Y), rw,
                GRAD(a, VD_S1, 1), GRAD(a, VD_S1, 2), GRAD(a, VD_T1, 1), GRAD(a, VD_T1, 2),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &s1);
        tex_sample16(p->image1, &s1, m, &lo, &hi);
        const VF k = _mm512_set1_ps(i255 / 255.0f);
        r = _mm512_mul_ps(r, _mm512_mul_ps(field_lo(lo), k)); g = _mm512_mul_ps(g, _mm512_mul_ps(field_lo(hi), k));
        b = _mm512_mul_ps(b, _mm512_mul_ps(field_hi(lo), k)); al = _mm512_mul_ps(al, _mm512_mul_ps(field_hi(hi), k));
    }
    *out = pack_rgba_ps(r, g, b, al);
    return m;
}

/* ---- k_env ------------------------------------------------------------------- */
static KERNEL_CALL(__mmask16) k_env(const SwTriAttr *a, const SwEnvParms *p, __mmask16 m, VF X, VF Y, VI *out)
{
    const VF one = _mm512_set1_ps(1.0f), tiny = _mm512_set1_ps(1e-30f);
    const VF rw = v_rcp(PLANE_W(a, X, Y));
    /* every vector below is (varying / w); a normalization cancels the w, a product of two does not */
    VF ex = PLANE(a, VE_EYE, X, Y), ey = PLANE(a, VE_EYE + 1, X, Y), ez = PLANE(a, VE_EYE + 2, X, Y);
    const VF re = v_rsqrt(_mm512_max_ps(_mm512_fmadd_ps(ex, ex, _mm512_fmadd_ps(ey, ey, _mm512_mul_ps(ez, ez))), tiny));
    ex = _mm512_mul_ps(ex, re); ey = _mm512_mul_ps(ey, re); ez = _mm512_mul_ps(ez, re);

    VF nx, ny, nz;
    VI lo, hi;
    if (p->bump) {
        TexSet bs;
        tex_set(p->bump, PLANE(a, VE_U, X, Y), PLANE(a, VE_V, X, Y), rw,
                GRAD(a, VE_U, 1), GRAD(a, VE_U, 2), GRAD(a, VE_V, 1), GRAD(a, VE_V, 2),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &bs);
        /* The cube lookup below takes its level from differences between NEIGHBOURING lanes, in the mask or not (a GPU's
           helper pixels). A masked fetch hands those lanes a zero texel, i.e. the normal (-1, -1, -1): every block on
           a silhouette then reads a coarser cube level than its surface asks for. g_envHelpers fetches the normal map
           for all sixteen lanes (their coordinates are clamped into the map like any other's). */
        tex_sample16(p->bump, &bs, g_envHelpers ? (__mmask16)0xFFFF : m, &lo, &hi);
        const VF two255 = _mm512_set1_ps(2.0f / (255.0f * 256.0f));
        VF lx = _mm512_fmsub_ps(field_hi(hi), two255, one);        /* x from ALPHA */
        VF ly = _mm512_fmsub_ps(field_lo(hi), two255, one);
        VF lz = _mm512_fmsub_ps(field_hi(lo), two255, one);
        const VF rl = v_rsqrt(_mm512_max_ps(_mm512_fmadd_ps(lx, lx, _mm512_fmadd_ps(ly, ly, _mm512_mul_ps(lz, lz))), tiny));
        lx = _mm512_mul_ps(lx, rl); ly = _mm512_mul_ps(ly, rl); lz = _mm512_mul_ps(lz, rl);
        /* tangent space to global: rows tc2, tc3, tc4 (each still times 1/w: take it out) */
        nx = _mm512_mul_ps(rw, _mm512_fmadd_ps(lx, PLANE(a, VE_M, X, Y),     _mm512_fmadd_ps(ly, PLANE(a, VE_M + 1, X, Y), _mm512_mul_ps(lz, PLANE(a, VE_M + 2, X, Y)))));
        ny = _mm512_mul_ps(rw, _mm512_fmadd_ps(lx, PLANE(a, VE_M + 3, X, Y), _mm512_fmadd_ps(ly, PLANE(a, VE_M + 4, X, Y), _mm512_mul_ps(lz, PLANE(a, VE_M + 5, X, Y)))));
        nz = _mm512_mul_ps(rw, _mm512_fmadd_ps(lx, PLANE(a, VE_M + 6, X, Y), _mm512_fmadd_ps(ly, PLANE(a, VE_M + 7, X, Y), _mm512_mul_ps(lz, PLANE(a, VE_M + 8, X, Y)))));
    } else {
        nx = PLANE(a, VE_M, X, Y); ny = PLANE(a, VE_M + 1, X, Y); nz = PLANE(a, VE_M + 2, X, Y);
        const VF rn = v_rsqrt(_mm512_max_ps(_mm512_fmadd_ps(nx, nx, _mm512_fmadd_ps(ny, ny, _mm512_mul_ps(nz, nz))), tiny));
        nx = _mm512_mul_ps(nx, rn); ny = _mm512_mul_ps(ny, rn); nz = _mm512_mul_ps(nz, rn);
    }
    /* R = 2 (E . N) N - E */
    const VF d2 = _mm512_mul_ps(_mm512_set1_ps(2.0f), _mm512_fmadd_ps(ex, nx, _mm512_fmadd_ps(ey, ny, _mm512_mul_ps(ez, nz))));
    const VF rx = _mm512_fmsub_ps(d2, nx, ex), ry = _mm512_fmsub_ps(d2, ny, ey), rz = _mm512_fmsub_ps(d2, nz, ez);
    tex_cube_sample16(p->cube, rx, ry, rz, m, &lo, &hi);

    const VF k = _mm512_set1_ps(1.0f / 256.0f);
    VF r = _mm512_mul_ps(field_lo(lo), k), g = _mm512_mul_ps(field_lo(hi), k), b = _mm512_mul_ps(field_hi(lo), k), al = _mm512_mul_ps(field_hi(hi), k);
    if (!p->bump) {                                                 /* environment.vfp: times vertex.color */
        if (p->vertexColorModulate == 0.0f) {
            r = _mm512_mul_ps(r, _mm512_set1_ps(p->color[0])); g = _mm512_mul_ps(g, _mm512_set1_ps(p->color[1]));
            b = _mm512_mul_ps(b, _mm512_set1_ps(p->color[2])); al = _mm512_mul_ps(al, _mm512_set1_ps(p->color[3]));
        } else {
            r = _mm512_mul_ps(r, _mm512_mul_ps(PLANE(a, VE_COLOR, X, Y), rw)); g = _mm512_mul_ps(g, _mm512_mul_ps(PLANE(a, VE_COLOR + 1, X, Y), rw));
            b = _mm512_mul_ps(b, _mm512_mul_ps(PLANE(a, VE_COLOR + 2, X, Y), rw)); al = _mm512_mul_ps(al, _mm512_mul_ps(PLANE(a, VE_COLOR + 3, X, Y), rw));
        }
    } else {
        al = _mm512_set1_ps(255.0f);                                /* the program leaves alpha undefined */
    }
    *out = pack_rgba_ps(r, g, b, al);
    return m;
}

/* ---- k_screen ---------------------------------------------------------------- */
/* (xp, yp) = the block's pixel centres in framebuffer pixels */
static KERNEL_CALL(__mmask16) k_screen(const SwTriAttr *a, const SwScreenParms *p, const SwImage *capture, const SwRect *captureRect,
                                        __mmask16 m, VF X, VF Y, VF xp, VF yp, VI *out)
{
    if (!capture) return 0;
    const VF zero = _mm512_setzero_ps(), one = _mm512_set1_ps(1.0f);
    const VF rw = v_rcp(PLANE_W(a, X, Y));
    const VF invW = _mm512_set1_ps(1.0f / (float)capture->w0), invH = _mm512_set1_ps(1.0f / (float)capture->h0);
    /* this pixel in the capture, 0..1 */
    VF u = _mm512_mul_ps(_mm512_sub_ps(xp, _mm512_set1_ps((float)captureRect->x0)), invW);
    VF v = _mm512_mul_ps(_mm512_sub_ps(yp, _mm512_set1_ps((float)captureRect->y0)), invH);
    VI lo, hi;

    if (p->program != SW_SCREEN_COLORPROCESS) {
        VF mx = one, my = one;
        if (p->program != SW_SCREEN_HEATHAZE && p->mask) {
            TexSet ms;
            tex_set(p->mask, PLANE(a, VH_MASK_U, X, Y), PLANE(a, VH_MASK_V, X, Y), rw,
                    GRAD(a, VH_MASK_U, 1), GRAD(a, VH_MASK_U, 2), GRAD(a, VH_MASK_V, 1), GRAD(a, VH_MASK_V, 2),
                    _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &ms);
            tex_sample16(p->mask, &ms, m, &lo, &hi);
            const VF k = _mm512_set1_ps(1.0f / (255.0f * 256.0f));
            mx = _mm512_mul_ps(field_lo(lo), k); my = _mm512_mul_ps(field_lo(hi), k);      /* mask.xy = R, G */
            if (p->program == SW_SCREEN_HEATHAZE_MASK_VERTEX) {
                mx = _mm512_mul_ps(mx, _mm512_mul_ps(PLANE(a, VH_COLOR_R, X, Y), rw));
                my = _mm512_mul_ps(my, _mm512_mul_ps(PLANE(a, VH_COLOR_G, X, Y), rw));
            }
            mx = _mm512_sub_ps(mx, _mm512_set1_ps(0.01f)); my = _mm512_sub_ps(my, _mm512_set1_ps(0.01f));
            m = _mm512_mask_cmp_ps_mask(m, mx, zero, _CMP_GE_OQ);  /* KIL mask */
            m = _mm512_mask_cmp_ps_mask(m, my, zero, _CMP_GE_OQ);
            if (!m) return 0;
        }
        TexSet bs;
        tex_set(p->bump, PLANE(a, VH_BUMP_U, X, Y), PLANE(a, VH_BUMP_V, X, Y), rw,
                GRAD(a, VH_BUMP_U, 1), GRAD(a, VH_BUMP_U, 2), GRAD(a, VH_BUMP_V, 1), GRAD(a, VH_BUMP_V, 2),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &bs);
        tex_sample16(p->bump, &bs, m, &lo, &hi);
        const VF two255 = _mm512_set1_ps(2.0f / (255.0f * 256.0f));
        const VF nx = _mm512_mul_ps(_mm512_fmsub_ps(field_hi(hi), two255, one), mx);      /* x from ALPHA */
        const VF ny = _mm512_mul_ps(_mm512_fmsub_ps(field_lo(hi), two255, one), my);
        /* MAD_SAT R0, localNormal, texcoord[2], position * env[1]; y points UP in the program */
        u = _mm512_fmadd_ps(nx, _mm512_mul_ps(PLANE(a, VH_DEFORM_X, X, Y), rw), u);
        v = _mm512_fnmadd_ps(ny, _mm512_mul_ps(PLANE(a, VH_DEFORM_Y, X, Y), rw), v);
        u = _mm512_min_ps(_mm512_max_ps(u, zero), one); v = _mm512_min_ps(_mm512_max_ps(v, zero), one);
    }
    TexSet cs; cs.u = u; cs.v = v; cs.level = _mm512_setzero_si512(); cs.level0 = g_texLevel0; cs.two = 0;
    tex_sample16(capture, &cs, m, &lo, &hi);
    const VF k = _mm512_set1_ps(1.0f / 256.0f);
    VF r = _mm512_mul_ps(field_lo(lo), k), g = _mm512_mul_ps(field_lo(hi), k), b = _mm512_mul_ps(field_hi(lo), k);
    if (p->program == SW_SCREEN_COLORPROCESS) {
        /* R0 = (r + g + b) * 0.33 * (target * fraction);  out = src * (1 - fraction) + R0 */
        const VF grey = _mm512_mul_ps(_mm512_add_ps(_mm512_add_ps(r, g), b), _mm512_set1_ps(0.33f));
        r = _mm512_fmadd_ps(r, _mm512_set1_ps(1.0f - p->parm0[0]), _mm512_mul_ps(grey, _mm512_set1_ps(p->parm1[0] * p->parm0[0])));
        g = _mm512_fmadd_ps(g, _mm512_set1_ps(1.0f - p->parm0[1]), _mm512_mul_ps(grey, _mm512_set1_ps(p->parm1[1] * p->parm0[1])));
        b = _mm512_fmadd_ps(b, _mm512_set1_ps(1.0f - p->parm0[2]), _mm512_mul_ps(grey, _mm512_set1_ps(p->parm1[2] * p->parm0[2])));
    }
    *out = pack_rgba_ps(r, g, b, _mm512_set1_ps(255.0f));
    return m;
}
