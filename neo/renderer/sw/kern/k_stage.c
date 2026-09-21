/* kern/k_stage.c -- one texture times one colour: DOOM 3's old-style shader stages, all 2D
   drawing (GUIs, console, HUD), and the alpha test of the perforated depth fill.
   Integer all the way: the fetch leaves two dwords of two 16-bit fields (R | B << 16 and
   G | A << 16), a colour scales both fields of a dword with one 16-bit multiply, and
   lo | hi << 8 is the RGBA pixel again. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

#define PLANE(a, k, X, Y) _mm512_fmadd_ps(_mm512_set1_ps((a)->p[k][2]), Y, _mm512_fmadd_ps(_mm512_set1_ps((a)->p[k][1]), X, _mm512_set1_ps((a)->p[k][0])))
#define PLANE_W(a, X, Y)  _mm512_fmadd_ps(_mm512_set1_ps((a)->w[2]), Y, _mm512_fmadd_ps(_mm512_set1_ps((a)->w[1]), X, _mm512_set1_ps((a)->w[0])))

/* a [0, 1] float lane as a mulhi multiplier: 65535 stands for one (the 1/65536 it is short of
   one is 1/256 of a code and vanishes in the final rounding) */
static __forceinline VI unit_to_mul16(VF x)
{
    return _mm512_cvtps_epi32(_mm512_mul_ps(_mm512_min_ps(_mm512_max_ps(x, _mm512_setzero_ps()), _mm512_set1_ps(1.0f)), _mm512_set1_ps(65535.0f)));
}

static __forceinline __mmask16 alpha_test(int test, float ref, VI alpha, __mmask16 m)
{
    switch (test) {
    case SW_ATEST_EQ_255: return _mm512_mask_cmpeq_epi32_mask(m, alpha, _mm512_set1_epi32(255));
    case SW_ATEST_LT_128: return _mm512_mask_cmplt_epi32_mask(m, alpha, _mm512_set1_epi32(128));
    case SW_ATEST_GE_128: return _mm512_mask_cmpge_epi32_mask(m, alpha, _mm512_set1_epi32(128));
    case SW_ATEST_GT_REF: return _mm512_mask_cmp_ps_mask(m, _mm512_cvtepi32_ps(alpha), _mm512_set1_ps(ref * 255.0f), _CMP_GT_OQ);
    default:              return m;
    }
}

/* Returns the lanes that survive the alpha test; *out = the RGBA pixels (tex * colour). */
static KERNEL_CALL(__mmask16) k_stage(const SwTriAttr *a, const SwStageParms *p, __mmask16 m, VF X, VF Y, VI *out)
{
    if (p->hasClip) {                                               /* the sign of (distance / w) is the distance's: w > 0 */
        m = _mm512_mask_cmp_ps_mask(m, PLANE(a, VS_CLIP, X, Y), _mm512_setzero_ps(), _CMP_GT_OQ);
        if (!m) return 0;
    }
    const VF rw = v_rcp(PLANE_W(a, X, Y));
    VI lo, hi;
    if (p->image && p->image->cube) {
        /* a positive per-lane scale does not move a cube lookup, but the level reads differences
           between lanes, so the direction is taken out of its 1/w */
        tex_cube_sample16(p->image, _mm512_mul_ps(PLANE(a, VS_U, X, Y), rw), _mm512_mul_ps(PLANE(a, VS_V, X, Y), rw),
                          _mm512_mul_ps(PLANE(a, VS_Q, X, Y), rw), m, &lo, &hi);
    } else if (p->image) {
        TexSet ts;
        if (p->screenAligned)
            tex_set_smooth(p->image, PLANE(a, VS_U, X, Y), PLANE(a, VS_V, X, Y), rw,
                _mm512_set1_ps(a->p[VS_U][1]), _mm512_set1_ps(a->p[VS_U][2]),
                _mm512_set1_ps(a->p[VS_V][1]), _mm512_set1_ps(a->p[VS_V][2]),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &ts);
        else
            tex_set(p->image, PLANE(a, VS_U, X, Y), PLANE(a, VS_V, X, Y), rw,
                _mm512_set1_ps(a->p[VS_U][1]), _mm512_set1_ps(a->p[VS_U][2]),
                _mm512_set1_ps(a->p[VS_V][1]), _mm512_set1_ps(a->p[VS_V][2]),
                _mm512_set1_ps(a->w[1]), _mm512_set1_ps(a->w[2]), &ts);
        tex_sample16(p->image, &ts, m, &lo, &hi);
    } else {
        lo = hi = _mm512_set1_epi32((int)0xFF00FF00u);              /* no image: white, 255.0 in 8.8 */
    }
    VI cr, cg, cb, ca;
    if (p->vertexColorModulate == 0.0f) {                           /* SVC_IGNORE: the colour is a constant */
        const float k = p->vertexColorAdd;
        cr = unit_to_mul16(_mm512_set1_ps(k * p->color[0])); cg = unit_to_mul16(_mm512_set1_ps(k * p->color[1]));
        cb = unit_to_mul16(_mm512_set1_ps(k * p->color[2])); ca = unit_to_mul16(_mm512_set1_ps(k * p->color[3]));
    } else {
        cr = unit_to_mul16(_mm512_mul_ps(PLANE(a, VS_R, X, Y), rw)); cg = unit_to_mul16(_mm512_mul_ps(PLANE(a, VS_G, X, Y), rw));
        cb = unit_to_mul16(_mm512_mul_ps(PLANE(a, VS_B, X, Y), rw)); ca = unit_to_mul16(_mm512_mul_ps(PLANE(a, VS_A, X, Y), rw));
    }
    /* 8.8 texel times the colour, still 8.8, then the one rounding to a byte */
    lo = tex_round8(_mm512_mulhi_epu16(lo, _mm512_or_si512(cr, _mm512_slli_epi32(cb, 16))));
    hi = tex_round8(_mm512_mulhi_epu16(hi, _mm512_or_si512(cg, _mm512_slli_epi32(ca, 16))));
    *out = _mm512_or_si512(lo, _mm512_slli_epi32(hi, 8));
    return p->alphaTest == SW_ATEST_NONE ? m : alpha_test(p->alphaTest, p->alphaRef, _mm512_srli_epi32(hi, 16), m);
}
