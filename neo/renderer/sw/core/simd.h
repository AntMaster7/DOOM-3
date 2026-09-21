/* core/simd.h -- AVX-512 helpers and the span lane tables. From CRenderer's core/simd.h and
   frame/frame.h. 16 lanes = one 4x4 block of pixels; lane i is pixel (i % 4, i / 4). */
#pragma once
#include "base.h"
#include "config.h"

#define VF __m512
#define VI __m512i

static __forceinline VF v_rcp(VF x) {              /* Newton-refined 1/x */
    VF r = _mm512_rcp14_ps(x);
    return _mm512_mul_ps(r, _mm512_fnmadd_ps(x, r, _mm512_set1_ps(2.0f)));
}
static __forceinline VF v_rsqrt(VF x) {            /* Newton-refined 1/sqrt(x) */
    VF r = _mm512_rsqrt14_ps(x);
    VF h = _mm512_mul_ps(_mm512_set1_ps(0.5f), x);
    return _mm512_mul_ps(r, _mm512_fnmadd_ps(_mm512_mul_ps(h, r), r, _mm512_set1_ps(1.5f)));
}
static __forceinline float vmax16(VF mx) {         /* horizontal max of 16 lanes */
    __m256 h = _mm256_max_ps(_mm512_castps512_ps256(mx), _mm512_extractf32x8_ps(mx, 1));
    __m128 q = _mm_max_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
    q = _mm_max_ps(q, _mm_movehl_ps(q, q));
    q = _mm_max_ps(q, _mm_shuffle_ps(q, q, 1));
    return _mm_cvtss_f32(q);
}
static __forceinline float vmin16(VF mn) {         /* horizontal min of 16 lanes */
    __m256 h = _mm256_min_ps(_mm512_castps512_ps256(mn), _mm512_extractf32x8_ps(mn, 1));
    __m128 q = _mm_min_ps(_mm256_castps256_ps128(h), _mm256_extractf128_ps(h, 1));
    q = _mm_min_ps(q, _mm_movehl_ps(q, q));
    q = _mm_min_ps(q, _mm_shuffle_ps(q, q, 1));
    return _mm_cvtss_f32(q);
}

/* lane offsets inside a block */
static const __declspec(align(64)) float SPAN_LX[16] = { 0,1,2,3, 0,1,2,3, 0,1,2,3, 0,1,2,3 };
static const __declspec(align(64)) float SPAN_LY[16] = { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3 };
static const __declspec(align(64)) int32_t SPAN_LXI[16] = { 0,1,2,3, 0,1,2,3, 0,1,2,3, 0,1,2,3 };
static const __declspec(align(64)) int32_t SPAN_LYI[16] = { 0,0,0,0, 1,1,1,1, 2,2,2,2, 3,3,3,3 };

/* per-axis coverage runs into a lane mask: xb is a 4-bit run of columns, yb a 4-bit run of rows,
   the block mask is their outer product */
#define SPAN_XREP(xb) ((unsigned)(xb) * 0x1111u)
static const unsigned short SPAN_YEXP[16] = {
    0x0000, 0x000F, 0x00F0, 0x00FF, 0x0F00, 0x0F0F, 0x0FF0, 0x0FFF,
    0xF000, 0xF00F, 0xF0F0, 0xF0FF, 0xFF00, 0xFF0F, 0xFFF0, 0xFFFF };
#define SPAN_RUN(lo, hi) ((2u << (hi)) - (1u << (lo)))      /* bits lo..hi set */

/* How raster_tri calls a fragment kernel. Inlined by default: out of line (__vectorcall, vector
   arguments in registers) a light pass costs 6% more (harness at 4K, 2026-09-21: 1 light 1.86
   against 1.76 ms, 8 local lights 2.46 against 2.30). SW_KERNEL_INLINE=0 is the fallback if
   MSVC's optimizer dies on raster_tri again: it is brittle there (C1001 in p2). What actually
   killed it was a chain of masked float compares in the stencil path, not the kernels' size;
   see the comment at the depth bounds test in rast/raster.c. */
#ifndef SW_KERNEL_INLINE
#define SW_KERNEL_INLINE 1
#endif
#if SW_KERNEL_INLINE
#define KERNEL_CALL(type) __forceinline type
#else
#define KERNEL_CALL(type) __declspec(noinline) type __vectorcall
#endif
