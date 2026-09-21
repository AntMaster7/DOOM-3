/* geom/vertex.c -- the vertex halves of the kernels: what the ARB vertex programs compute,
   evaluated per triangle corner in the geometry phase (so it runs in the pool, not on the
   engine's thread). A vertex shared by six triangles is evaluated six times; measure before
   caching per vertex array (plan 6.3).

   Varying layouts (the fragment halves index planes by these):
     STAGE        0 u, 1 v, 2 q (cube lookups), 3-6 colour rgba
     INTERACTION  0-2 tc0 = light vector in tangent space (not normalized)
                  3-4 bump uv, 5 falloff s, 6-8 projection s t q, 9-10 diffuse uv, 11-12 specular uv
                  13-15 tc6 = half angle in tangent space (not normalized), 16-19 colour rgba */
#include "../core/base.h"
#include "../core/config.h"
#include "../core/types.h"

enum { VS_U, VS_V, VS_Q, VS_R, VS_G, VS_B, VS_A, VS_CLIP, VS_COUNT };     /* q: the third coordinate of a cube lookup; clip: distance to a mirror's plane */
enum { VI_L = 0, VI_BUMP_U = 3, VI_BUMP_V, VI_FALLOFF, VI_PROJ_S, VI_PROJ_T, VI_PROJ_Q,
       VI_DIFF_U, VI_DIFF_V, VI_SPEC_U, VI_SPEC_V, VI_H = 13, VI_COLOR = 16, VI_COUNT = 20 };

enum { VD_S0, VD_T0, VD_Q0, VD_S1, VD_T1, VD_COUNT };
enum { VE_U, VE_V, VE_EYE = 2, VE_M = 5, VE_COLOR = 14, VE_COUNT = 18 };
enum { VH_BUMP_U, VH_BUMP_V, VH_MASK_U, VH_MASK_V, VH_DEFORM_X, VH_DEFORM_Y, VH_COLOR_R, VH_COLOR_G, VH_COUNT };

static __forceinline int kernel_num_varyings(int kernel)
{
    switch (kernel) {
    case SW_KERN_STAGE:         return VS_COUNT;
    case SW_KERN_INTERACTION:   return VI_COUNT;
    case SW_KERN_DUAL:          return VD_COUNT;
    case SW_KERN_ENV:           return VE_COUNT;
    case SW_KERN_SCREEN:        return VH_COUNT;
    default:                    return 0;
    }
}

static __forceinline float dot3(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

static void vertex_stage(const SwDraw *d, int index, float *var)
{
    const SwStageParms *p = (const SwStageParms *)d->kernelParms;
    const char *v = (const char *)d->verts.base + (size_t)index * d->verts.stride;
    const float *st = (const float *)(v + d->verts.ofsSt);
    const uint8_t *col = (const uint8_t *)(v + d->verts.ofsColor);
    if (p->texgen == SW_TG_EXPLICIT) {
        var[VS_U] = st[0] * p->texMatrix[0][0] + st[1] * p->texMatrix[0][1] + p->texMatrix[0][3];
        var[VS_V] = st[0] * p->texMatrix[1][0] + st[1] * p->texMatrix[1][1] + p->texMatrix[1][3];
        var[VS_Q] = 0.0f;
    } else {
        const float *dir = p->texgen == SW_TG_DYNAMIC3 && d->verts.texCoords3
            ? (const float *)((const char *)d->verts.texCoords3 + (size_t)index * d->verts.texCoords3Stride)
            : (const float *)(v + d->verts.ofsNormal);
        var[VS_U] = dir[0]; var[VS_V] = dir[1]; var[VS_Q] = dir[2];
    }
    for (int i = 0; i < 3; i++)
        var[VS_R + i] = ((float)col[i] * (1.0f / 255.0f) * p->vertexColorModulate + p->vertexColorAdd) * p->color[i];
    /* SVC_INVERSE_MODULATE inverts RGB only (GL_OPERAND1_RGB = ONE_MINUS_SRC_COLOR; the alpha
       combiner stays a plain modulate) */
    const float a = (float)col[3] * (1.0f / 255.0f);
    var[VS_A] = (p->vertexColorModulate == 0.0f ? p->vertexColorAdd : a) * p->color[3];
    if (p->hasClip) {
        const float *pos = (const float *)(v + d->verts.ofsXyz);
        var[VS_CLIP] = p->clipPlane[0] * pos[0] + p->clipPlane[1] * pos[1] + p->clipPlane[2] * pos[2] + p->clipPlane[3];
    } else var[VS_CLIP] = 0.0f;
}

/* interaction.vfp's vertex program, as written */
static void vertex_interaction(const SwDraw *d, int index, float *var)
{
    const SwInteractionParms *p = (const SwInteractionParms *)d->kernelParms;
    const char *v = (const char *)d->verts.base + (size_t)index * d->verts.stride;
    const float *pos = (const float *)(v + d->verts.ofsXyz), *st = (const float *)(v + d->verts.ofsSt);
    const float *n = (const float *)(v + d->verts.ofsNormal);
    const float *t0 = (const float *)(v + d->verts.ofsTangent0), *t1 = (const float *)(v + d->verts.ofsTangent1);
    const uint8_t *col = (const uint8_t *)(v + d->verts.ofsColor);

    const float L[3] = { p->localLightOrigin[0] - pos[0], p->localLightOrigin[1] - pos[1], p->localLightOrigin[2] - pos[2] };
    var[VI_L] = dot3(t0, L); var[VI_L + 1] = dot3(t1, L); var[VI_L + 2] = dot3(n, L);

    var[VI_BUMP_U] = st[0] * p->bumpMatrix[0][0] + st[1] * p->bumpMatrix[0][1] + p->bumpMatrix[0][3];
    var[VI_BUMP_V] = st[0] * p->bumpMatrix[1][0] + st[1] * p->bumpMatrix[1][1] + p->bumpMatrix[1][3];
    var[VI_DIFF_U] = st[0] * p->diffuseMatrix[0][0] + st[1] * p->diffuseMatrix[0][1] + p->diffuseMatrix[0][3];
    var[VI_DIFF_V] = st[0] * p->diffuseMatrix[1][0] + st[1] * p->diffuseMatrix[1][1] + p->diffuseMatrix[1][3];
    var[VI_SPEC_U] = st[0] * p->specularMatrix[0][0] + st[1] * p->specularMatrix[0][1] + p->specularMatrix[0][3];
    var[VI_SPEC_V] = st[0] * p->specularMatrix[1][0] + st[1] * p->specularMatrix[1][1] + p->specularMatrix[1][3];

    var[VI_FALLOFF] = dot3(pos, p->lightFalloffS) + p->lightFalloffS[3];
    var[VI_PROJ_S] = dot3(pos, p->lightProjectS) + p->lightProjectS[3];
    var[VI_PROJ_T] = dot3(pos, p->lightProjectT) + p->lightProjectT[3];
    var[VI_PROJ_Q] = dot3(pos, p->lightProjectQ) + p->lightProjectQ[3];

    const float V[3] = { p->localViewOrigin[0] - pos[0], p->localViewOrigin[1] - pos[1], p->localViewOrigin[2] - pos[2] };
    const float rl = 1.0f / sqrtf(dot3(L, L) + 1e-30f), rv = 1.0f / sqrtf(dot3(V, V) + 1e-30f);
    const float H[3] = { L[0] * rl + V[0] * rv, L[1] * rl + V[1] * rv, L[2] * rl + V[2] * rv };
    var[VI_H] = dot3(t0, H); var[VI_H + 1] = dot3(t1, H); var[VI_H + 2] = dot3(n, H);

    for (int i = 0; i < 4; i++)
        var[VI_COLOR + i] = (float)col[i] * (1.0f / 255.0f) * p->vertexColorModulate + p->vertexColorAdd;
}

static __forceinline float plane4(const float *pl, const float *pos) { return pl[0] * pos[0] + pl[1] * pos[1] + pl[2] * pos[2] + pl[3]; }

/* the fixed-function texgens of the fog, blend-light and screen passes: object-linear planes */
static void vertex_dual(const SwDraw *d, int index, float *var)
{
    const SwDualParms *p = (const SwDualParms *)d->kernelParms;
    const float *pos = (const float *)((const char *)d->verts.base + (size_t)index * d->verts.stride + d->verts.ofsXyz);
    var[VD_S0] = plane4(p->s0, pos); var[VD_T0] = plane4(p->t0, pos); var[VD_Q0] = plane4(p->q0, pos);
    var[VD_S1] = plane4(p->s1, pos); var[VD_T1] = plane4(p->t1, pos);
}

/* environment.vfp / bumpyEnvironment.vfp */
static void vertex_env(const SwDraw *d, int index, float *var)
{
    const SwEnvParms *p = (const SwEnvParms *)d->kernelParms;
    const char *v = (const char *)d->verts.base + (size_t)index * d->verts.stride;
    const float *pos = (const float *)(v + d->verts.ofsXyz), *st = (const float *)(v + d->verts.ofsSt);
    const float *n = (const float *)(v + d->verts.ofsNormal);
    const float *t0 = (const float *)(v + d->verts.ofsTangent0), *t1 = (const float *)(v + d->verts.ofsTangent1);
    const uint8_t *col = (const uint8_t *)(v + d->verts.ofsColor);
    const float eye[3] = { p->localViewOrigin[0] - pos[0], p->localViewOrigin[1] - pos[1], p->localViewOrigin[2] - pos[2] };
    for (int i = 0; i < VE_COUNT; i++) var[i] = 0.0f;
    if (p->bump) {
        var[VE_U] = st[0]; var[VE_V] = st[1];
        for (int r = 0; r < 3; r++) {
            var[VE_EYE + r] = dot3(eye, p->modelRows[r]);
            var[VE_M + r * 3 + 0] = dot3(t0, p->modelRows[r]);
            var[VE_M + r * 3 + 1] = dot3(t1, p->modelRows[r]);
            var[VE_M + r * 3 + 2] = dot3(n, p->modelRows[r]);
        }
    } else {
        var[VE_EYE] = eye[0]; var[VE_EYE + 1] = eye[1]; var[VE_EYE + 2] = eye[2];
        var[VE_M] = n[0]; var[VE_M + 1] = n[1]; var[VE_M + 2] = n[2];
    }
    for (int i = 0; i < 4; i++) var[VE_COLOR + i] = (float)col[i] * (1.0f / 255.0f);
}

/* the vertex program the heat haze family shares */
static void vertex_screen(const SwDraw *d, int index, float *var)
{
    const SwScreenParms *p = (const SwScreenParms *)d->kernelParms;
    const char *v = (const char *)d->verts.base + (size_t)index * d->verts.stride;
    const float *pos = (const float *)(v + d->verts.ofsXyz), *st = (const float *)(v + d->verts.ofsSt);
    const uint8_t *col = (const uint8_t *)(v + d->verts.ofsColor);
    var[VH_BUMP_U] = st[0] + p->parm0[0]; var[VH_BUMP_V] = st[1] + p->parm0[1];
    var[VH_MASK_U] = st[0]; var[VH_MASK_V] = st[1];
    /* the size of one unit at this depth, in the projection's x, capped */
    const float z = plane4(p->mvRow2, pos);
    float r1 = p->projRow0[0] + p->projRow0[2] * z + p->projRow0[3];
    float r2 = p->projRow3[0] + p->projRow3[2] * z + p->projRow3[3];
    if (r2 < 1.0f) r2 = 1.0f;
    r1 /= r2;
    if (r1 > 0.02f) r1 = 0.02f;
    var[VH_DEFORM_X] = r1 * p->parm1[0]; var[VH_DEFORM_Y] = r1 * p->parm1[1];
    var[VH_COLOR_R] = (float)col[0] * (1.0f / 255.0f); var[VH_COLOR_G] = (float)col[1] * (1.0f / 255.0f);
}

static __forceinline void vertex_half(const SwDraw *d, int index, float *var)
{
    switch (d->kernel) {
    case SW_KERN_INTERACTION:   vertex_interaction(d, index, var); break;
    case SW_KERN_DUAL:          vertex_dual(d, index, var); break;
    case SW_KERN_ENV:           vertex_env(d, index, var); break;
    case SW_KERN_SCREEN:        vertex_screen(d, index, var); break;
    default:                    vertex_stage(d, index, var); break;
    }
}
