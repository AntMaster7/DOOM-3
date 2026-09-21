"""Stage 7 in the core: three kernels (dual, env, screen), a depth fill that can leave the colour
alone (subview surfaces), and the view's capture point (_currentRender) with a two-phase tile pass."""
import sys
sys.path.insert(0, 'C:/Source/DOOM-3/tools/port-x64')
from px import patch

SW = 'C:/Source/DOOM-3/neo/renderer/sw/'

# ---------------------------------------------------------------- sw_api.h
patch(SW + 'sw_api.h', [
("""enum {
    SW_KERN_FLAT,           /* the draw's constant colour; no vertex source needed */
    SW_KERN_STAGE,          /* one texture x colour: old-style stages, 2D, the perforated depth fill */
    SW_KERN_INTERACTION     /* interaction.vfp (plan, Appendix A.1) */
};
""", """enum {
    SW_KERN_FLAT,           /* the draw's constant colour; no vertex source needed */
    SW_KERN_STAGE,          /* one texture x colour: old-style stages, 2D, the perforated depth fill */
    SW_KERN_INTERACTION,    /* interaction.vfp (plan, Appendix A.1) */
    SW_KERN_DUAL,           /* colour x projective texture x second texture, object-linear texgens: fog, blend lights, TG_SCREEN */
    SW_KERN_ENV,            /* environment.vfp / bumpyEnvironment.vfp */
    SW_KERN_SCREEN          /* heatHaze*.vfp, colorProcess.vfp: reads the view's capture (sw_capture_point) */
};
"""),
("""typedef struct SwInteractionParms {""", """/* every coordinate is dot(plane, (x, y, z, 1)) of the object-space vertex */
typedef struct SwDualParms {
    const SwImage * image0;             /* looked up at (s0 / q0, t0 / q0); NULL = white */
    const SwImage * image1;             /* looked up at (s1, t1); NULL = white */
    float           s0[4], t0[4], q0[4], s1[4], t1[4];
    float           color[4];
} SwDualParms;

typedef struct SwEnvParms {
    const SwImage * cube;
    const SwImage * bump;               /* NULL: environment.vfp; else bumpyEnvironment.vfp (st is its coordinate) */
    float           localViewOrigin[3];
    float           modelRows[3][3];    /* bumpy: program.env[6..8].xyz, local to global */
    float           color[4];           /* environment.vfp multiplies by vertex.color: the stage colour ... */
    float           vertexColorModulate;/* ... or, when not 0, the vertex colours as they are */
} SwEnvParms;

enum { SW_SCREEN_HEATHAZE, SW_SCREEN_HEATHAZE_MASK, SW_SCREEN_HEATHAZE_MASK_VERTEX, SW_SCREEN_COLORPROCESS };
typedef struct SwScreenParms {
    int             program;            /* SW_SCREEN_* */
    const SwImage * bump, *mask;
    float           parm0[4], parm1[4]; /* program.local[0] and [1]: scroll and deform magnitude; colorProcess: fraction and target */
    float           mvRow2[4];          /* state.matrix.modelview.row[2] */
    float           projRow0[4], projRow3[4];
} SwScreenParms;

typedef struct SwInteractionParms {"""),
("""void            sw_end_view( void );                    /* runs geometry and tiles, then returns */
""", """/* The view's capture point (_currentRender): everything submitted so far is rendered and the
   framebuffer inside `rect` is copied; SW_KERN_SCREEN draws submitted afterwards sample that
   copy. Depth survives the point. One per view (the GL back end copies once per view too). */
void            sw_capture_point( const SwRect *rect );
void            sw_end_view( void );                    /* runs geometry and tiles, then returns */
/* copies framebuffer pixels (row 0 on top) into `rgba`, `pitch` pixels per destination row */
void            sw_read_pixels( const SwRect *rect, uint32_t *rgba, int pitch );
"""),
])

# ---------------------------------------------------------------- vertex halves
patch(SW + 'geom/vertex.c', [
("""static __forceinline int kernel_num_varyings(int kernel)
{
    return kernel == SW_KERN_STAGE ? VS_COUNT : (kernel == SW_KERN_INTERACTION ? VI_COUNT : 0);
}
""", """enum { VD_S0, VD_T0, VD_Q0, VD_S1, VD_T1, VD_COUNT };
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
"""),
("""static __forceinline void vertex_half(const SwDraw *d, int index, float *var)
{
    if (d->kernel == SW_KERN_INTERACTION) vertex_interaction(d, index, var);
    else vertex_stage(d, index, var);
}""", """static __forceinline float plane4(const float *pl, const float *pos) { return pl[0] * pos[0] + pl[1] * pos[1] + pl[2] * pos[2] + pl[3]; }

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
}"""),
])

# ---------------------------------------------------------------- raster.c
patch(SW + 'rast/raster.c', [
("""    int             optHier, optZrange, optCellFast, optLightCells, kernelCut, dbgEqual;  /* per-tile copies of the switches */
""", """    int             optHier, optZrange, optCellFast, optLightCells, kernelCut, dbgEqual;  /* per-tile copies of the switches */
    const SwImage * capture;            /* the view's capture, for SW_KERN_SCREEN */
    SwRect          captureRect;
"""),
("""    const SwInteractionParms *const iparms = (const SwInteractionParms *)d->kernelParms;
""", """    const SwInteractionParms *const iparms = (const SwInteractionParms *)d->kernelParms;
    const int fillColor = d->writeMask != SW_WRITE_NONE;   /* a subview's depth fill leaves the colour alone */
"""),
("""                _mm512_mask_store_ps(tz + blk, cov, zs);
                _mm512_mask_store_epi32(tcol + blk, cov, vColor);
                st->pxDepth += (unsigned)_mm_popcnt_u32(cov);""", """                _mm512_mask_store_ps(tz + blk, cov, zs);
                if (fillColor) _mm512_mask_store_epi32(tcol + blk, cov, vColor);
                st->pxDepth += (unsigned)_mm_popcnt_u32(cov);"""),
("""                    if (kern == SW_KERN_STAGE) cov = k_stage(attr, sparms, cov, X, Y, &src);
                    else cov = k_interaction(attr, iparms, iflags, kernelCut, cov, X, Y, &src, st);""",
"""                    if (kern == SW_KERN_STAGE) cov = k_stage(attr, sparms, cov, X, Y, &src);
                    else if (kern == SW_KERN_INTERACTION) cov = k_interaction(attr, iparms, iflags, kernelCut, cov, X, Y, &src, st);
                    else if (kern == SW_KERN_DUAL) cov = k_dual(attr, (const SwDualParms *)d->kernelParms, cov, X, Y, &src);
                    else if (kern == SW_KERN_ENV) cov = k_env(attr, (const SwEnvParms *)d->kernelParms, cov, X, Y, &src);
                    else cov = k_screen(attr, (const SwScreenParms *)d->kernelParms, c->capture, &c->captureRect, cov, X, Y,
                                        _mm512_add_ps(X, _mm512_set1_ps((float)ox)), _mm512_add_ps(Y, _mm512_set1_ps((float)oy)), &src);"""),
])
print('done')
