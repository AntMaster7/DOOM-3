/* kern/blend.c -- the blend stage: GL's finite factor table (plan, Appendix B), colour write
   masks, the masked store into the tile's swizzled colour. One block = 16 RGBA pixels = 64 bytes.

   Fast paths: ONE, ZERO is a masked store; ONE, ONE (every light pass) is a saturating byte add.
   Everything else goes through 16-bit lanes: result = sat(div255(s * sf) + div255(d * df)), with
   div255 the exact rounded division for products up to 65535. GL blends in the framebuffer's
   precision and does not pin the rounding down; this is within one code of any of them. */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/simd.h"
#include "../core/types.h"

typedef struct BlendState {
    int     src, dst;           /* SW_BF_* */
    int     chan;               /* SW_WRITE_* bits, 1..15 */
    int     fast;               /* 1 = replace, 2 = saturating add, 0 = general */
} BlendState;

static __forceinline BlendState blend_state(const SwDraw *d)
{
    BlendState b;
    b.src = d->srcBlend; b.dst = d->dstBlend;
    b.chan = d->writeMask ? (d->writeMask & 15) : SW_WRITE_RGBA;
    b.fast = (b.src == SW_BF_ONE && b.dst == SW_BF_ZERO) ? 1 : ((b.src == SW_BF_ONE && b.dst == SW_BF_ONE) ? 2 : 0);
    return b;
}

/* x / 255 rounded, exact for x <= 65535, in unsigned 16-bit lanes */
static __forceinline VI div255_epu16(VI x)
{
    const VI t = _mm512_add_epi16(x, _mm512_set1_epi16(128));
    return _mm512_srli_epi16(_mm512_add_epi16(t, _mm512_srli_epi16(t, 8)), 8);
}

/* the alpha word of every pixel replicated into its four channel words (a pixel = one qword) */
static __forceinline VI alpha_words(VI px16)
{
    static const __declspec(align(64)) int8_t pick[64] = {
        6,7,6,7,6,7,6,7, 14,15,14,15,14,15,14,15,  6,7,6,7,6,7,6,7, 14,15,14,15,14,15,14,15,
        6,7,6,7,6,7,6,7, 14,15,14,15,14,15,14,15,  6,7,6,7,6,7,6,7, 14,15,14,15,14,15,14,15 };
    return _mm512_shuffle_epi8(px16, _mm512_load_si512((const void *)pick));
}

static __forceinline VI blend_factor(int f, VI s, VI d, int isSrc)
{
    const VI c255 = _mm512_set1_epi16(255);
    switch (f) {
    case SW_BF_ZERO:                return _mm512_setzero_si512();
    case SW_BF_SRC_COLOR:           return s;
    case SW_BF_ONE_MINUS_SRC_COLOR: return _mm512_sub_epi16(c255, s);
    case SW_BF_DST_COLOR:           return d;
    case SW_BF_ONE_MINUS_DST_COLOR: return _mm512_sub_epi16(c255, d);
    case SW_BF_SRC_ALPHA:           return alpha_words(s);
    case SW_BF_ONE_MINUS_SRC_ALPHA: return _mm512_sub_epi16(c255, alpha_words(s));
    case SW_BF_DST_ALPHA:           return alpha_words(d);
    case SW_BF_ONE_MINUS_DST_ALPHA: return _mm512_sub_epi16(c255, alpha_words(d));
    case SW_BF_SRC_ALPHA_SATURATE:
        if (isSrc) {                /* (f, f, f, 1) with f = min(As, 1 - Ad) */
            const VI f3 = _mm512_min_epu16(alpha_words(s), _mm512_sub_epi16(c255, alpha_words(d)));
            return _mm512_mask_mov_epi16(f3, 0x88888888u, c255);
        }
        return c255;
    default:                        return c255;
    }
}

/* one half block: 8 pixels as 32 unsigned words */
static __forceinline __m256i blend_half(__m256i s8, __m256i d8, int sf, int df)
{
    const VI s = _mm512_cvtepu8_epi16(s8), d = _mm512_cvtepu8_epi16(d8);
    const VI a = div255_epu16(_mm512_mullo_epi16(s, blend_factor(sf, s, d, 1)));
    const VI b = div255_epu16(_mm512_mullo_epi16(d, blend_factor(df, s, d, 0)));
    return _mm512_cvtusepi16_epi8(_mm512_add_epi16(a, b));          /* saturates at 255 */
}

/* blend 16 source pixels into the block under the lane mask */
static __forceinline void blend_store(uint32_t *dstp, __mmask16 m, VI src, const BlendState *b)
{
    if (b->chan == SW_WRITE_RGBA) {
        if (b->fast == 1) { _mm512_mask_store_epi32(dstp, m, src); return; }
        if (b->fast == 2) {
            _mm512_mask_store_epi32(dstp, m, _mm512_adds_epu8(_mm512_load_si512((const void *)dstp), src));
            return;
        }
    }
    const VI dst = _mm512_load_si512((const void *)dstp);
    VI out;
    if (b->fast == 1) out = src;
    else if (b->fast == 2) out = _mm512_adds_epu8(dst, src);
    else {
        const __m256i lo = blend_half(_mm512_castsi512_si256(src), _mm512_castsi512_si256(dst), b->src, b->dst);
        const __m256i hi = blend_half(_mm512_extracti64x4_epi64(src, 1), _mm512_extracti64x4_epi64(dst, 1), b->src, b->dst);
        out = _mm512_inserti64x4(_mm512_castsi256_si512(lo), hi, 1);
    }
    /* lane mask -> byte mask, times the channel bits: a nibble per pixel, no carries */
    const unsigned long long bytes = _pdep_u64((unsigned long long)m, 0x1111111111111111ull) * (unsigned long long)b->chan;
    _mm512_store_si512((void *)dstp, _mm512_mask_mov_epi8(dst, (__mmask64)bytes, out));
}
