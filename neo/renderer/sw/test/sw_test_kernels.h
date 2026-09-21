/* test/sw_test_kernels.h -- the harness's tests of the sampler, the blend table, the varyings and
   the two kernels. Included by sw_test.c (one translation unit), after its helpers. */

/* idDrawVert's shape: 60 bytes */
typedef struct TVert { float xyz[3]; float st[2]; float normal[3]; float tan0[3]; float tan1[3]; uint8_t color[4]; } TVert;

static SwVertexSource tvert_source(const TVert *v)
{
    SwVertexSource s;
    memset(&s, 0, sizeof(s));
    s.base = v; s.stride = (int)sizeof(TVert);
    s.ofsXyz = (int)offsetof(TVert, xyz); s.ofsSt = (int)offsetof(TVert, st); s.ofsNormal = (int)offsetof(TVert, normal);
    s.ofsTangent0 = (int)offsetof(TVert, tan0); s.ofsTangent1 = (int)offsetof(TVert, tan1); s.ofsColor = (int)offsetof(TVert, color);
    return s;
}

static unsigned t_rand(unsigned *seed) { *seed = *seed * 1664525u + 1013904223u; return *seed >> 8; }

/* a mip chain by box filter; returns the level count and fills levels[] (caller frees) */
static int make_mips(uint8_t *level0, int w, int h, uint8_t **levels, int maxLevels)
{
    int n = 1;
    levels[0] = level0;
    while ((w > 1 || h > 1) && n < maxLevels) {
        const int nw = w > 1 ? w / 2 : 1, nh = h > 1 ? h / 2 : 1;
        uint8_t *dst = (uint8_t *)malloc((size_t)nw * nh * 4);
        const uint8_t *src = levels[n - 1];
        for (int y = 0; y < nh; y++)
            for (int x = 0; x < nw; x++)
                for (int c = 0; c < 4; c++) {
                    const int x1 = (x * 2 + 1 < w) ? x * 2 + 1 : x * 2, y1 = (y * 2 + 1 < h) ? y * 2 + 1 : y * 2;
                    dst[(y * nw + x) * 4 + c] = (uint8_t)((src[(y * 2 * w + x * 2) * 4 + c] + src[(y * 2 * w + x1) * 4 + c] +
                                                           src[(y1 * w + x * 2) * 4 + c] + src[(y1 * w + x1) * 4 + c] + 2) / 4);
                }
        levels[n++] = dst; w = nw; h = nh;
    }
    return n;
}

/* a screen-aligned quad covering pixels [x0, x1) x [y0, y1) with the given uv corners (w = 1) */
static void screen_quad(Mesh *m, TVert *tv, int x0, int y0, int x1, int y1, float u0, float v0, float u1, float v1)
{
    mesh_init(m, 4, 6);
    const float cx[4] = { (float)x0, (float)x1, (float)x1, (float)x0 }, cy[4] = { (float)y0, (float)y0, (float)y1, (float)y1 };
    const float us[4] = { u0, u1, u1, u0 }, vs[4] = { v0, v0, v1, v1 };
    for (int i = 0; i < 4; i++) {
        V4 c = { cx[i] / (float)g_w * 2 - 1, 1 - cy[i] / (float)g_h * 2, 0.5f, 1 };
        mesh_vert(m, c);
        memset(&tv[i], 0, sizeof(TVert));
        tv[i].st[0] = us[i]; tv[i].st[1] = vs[i];
        tv[i].color[0] = tv[i].color[1] = tv[i].color[2] = tv[i].color[3] = 255;
    }
    mesh_tri(m, 0, 1, 2); mesh_tri(m, 0, 2, 3);
}

static SwStageParms stage_parms(const SwImage *im)
{
    SwStageParms p;
    memset(&p, 0, sizeof(p));
    p.image = im;
    p.texMatrix[0][0] = 1; p.texMatrix[1][1] = 1;
    p.color[0] = p.color[1] = p.color[2] = p.color[3] = 1;
    p.vertexColorModulate = 0; p.vertexColorAdd = 1;
    return p;
}

static void draw_stage(const Mesh *m, const TVert *tv, const SwStageParms *p, int srcBlend, int dstBlend)
{
    SwRect r = full_rect();
    sw_begin_view(&r);
    sw_begin_segment(SW_SEG_COLOR, NULL);
    SwDraw d = make_draw(m, SW_OP_COLOR);
    d.kernel = SW_KERN_STAGE; d.kernelParms = p; d.verts = tvert_source(tv);
    d.srcBlend = srcBlend; d.dstBlend = dstBlend;
    sw_draw(&d);
    sw_end_view();
}

/* ---- test 5: the sampler ---------------------------------------------------- */
static uint64_t test_sampler(void)
{
    uint64_t h = 0;
    const int S = 256;
    unsigned seed = 777;
    uint8_t *tex = (uint8_t *)malloc((size_t)S * S * 4);
    for (int i = 0; i < S * S * 4; i++) tex[i] = (uint8_t)t_rand(&seed);
    const uint8_t *lv0[1] = { tex };
    SwImage *rep = sw_image_create(S, S, 1, lv0, SW_WRAP_REPEAT), *clm = sw_image_create(S, S, 1, lv0, SW_WRAP_CLAMP);
    Mesh m; TVert tv[4];

    /* a. one texel per pixel: every pixel is its texel, byte for byte (orientation, byte order,
          the half-texel offset, bilinear weights of exactly zero) */
    sw_clear_framebuffer(0xFF000000u);
    screen_quad(&m, tv, 128, 64, 128 + S, 64 + S, 0, 0, 1, 1);
    SwStageParms p = stage_parms(rep);
    draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
    long wrong = 0;
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        uint32_t t; memcpy(&t, tex + ((size_t)y * S + x) * 4, 4);
        wrong += pixel(128 + x, 64 + y) != t;
    }
    CHECK(wrong == 0, "1:1 textured quad: %ld pixels differ from their texel", wrong);
    h ^= frame_hash();
    mesh_free(&m);

    /* b. uv from -1 to 2 over 768 pixels: repeat tiles it three times, clamp holds the edge texels */
    for (int mode = 0; mode < 2; mode++) {
        sw_clear_framebuffer(0xFF000000u);
        screen_quad(&m, tv, 64, 64, 64 + 3 * S, 64 + 3 * S, -1, -1, 2, 2);
        p = stage_parms(mode ? clm : rep);
        draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
        wrong = 0;
        for (int y = 0; y < 3 * S; y += 5) for (int x = 0; x < 3 * S; x += 3) {
            int txx = x - S, tyy = y - S;
            if (mode) { txx = txx < 0 ? 0 : (txx > S - 1 ? S - 1 : txx); tyy = tyy < 0 ? 0 : (tyy > S - 1 ? S - 1 : tyy); }
            else { txx = (txx + S) % S; tyy = (tyy + S) % S; }
            uint32_t t; memcpy(&t, tex + ((size_t)tyy * S + txx) * 4, 4);
            wrong += pixel(64 + x, 64 + y) != t;
        }
        CHECK(wrong == 0, "%s addressing: %ld sampled pixels differ", mode ? "clamp" : "repeat", wrong);
        h ^= frame_hash() * (uint64_t)(3 + mode);
        mesh_free(&m);
    }

    /* c. level selection: every level a solid colour of its own; minified 2:1, 4:1 and 8:1 must
          read levels 1, 2, 3 (GL_LINEAR_MIPMAP_NEAREST rounds log2 of the footprint) */
    {
        uint8_t *levels[16];
        int w = S, n = 0;
        for (; w >= 1; w /= 2, n++) {
            levels[n] = (uint8_t *)malloc((size_t)w * w * 4);
            for (int i = 0; i < w * w; i++) { levels[n][i * 4] = (uint8_t)(n * 20 + 10); levels[n][i * 4 + 1] = (uint8_t)(200 - n * 10); levels[n][i * 4 + 2] = 7; levels[n][i * 4 + 3] = 255; }
        }
        SwImage *mip = sw_image_create(S, S, n, (const uint8_t *const *)levels, SW_WRAP_REPEAT);
        for (int k = 0; k < 4; k++) {
            const int size = S >> k;
            sw_clear_framebuffer(0xFF000000u);
            screen_quad(&m, tv, 100, 100, 100 + size, 100 + size, 0, 0, 1, 1);
            p = stage_parms(mip);
            draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
            const uint32_t got = pixel(100 + size / 2, 100 + size / 2);
            CHECK((got & 0xFF) == (uint32_t)(k * 20 + 10), "minified %d:1 read level colour %u, expected level %d", 1 << k, got & 0xFF, k);
            mesh_free(&m);
        }
        sw_image_destroy(mip);
        for (int i = 0; i < n; i++) free(levels[i]);
    }

    /* d. vertex colour modulate and the alpha tests */
    {
        sw_clear_framebuffer(0xFF000000u);
        screen_quad(&m, tv, 128, 64, 128 + S, 64 + S, 0, 0, 1, 1);
        for (int i = 0; i < 4; i++) { tv[i].color[0] = 128; tv[i].color[1] = 255; tv[i].color[2] = 0; tv[i].color[3] = 255; }
        p = stage_parms(rep);
        p.vertexColorModulate = 1; p.vertexColorAdd = 0;
        p.alphaTest = SW_ATEST_GE_128;
        draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
        wrong = 0;
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            const uint8_t *t = tex + ((size_t)y * S + x) * 4;
            const uint32_t got = pixel(128 + x, 64 + y);
            if (t[3] < 128) { wrong += got != 0xFF000000u; continue; }
            const int er = (t[0] * 128 + 127) / 255, dr = (int)(got & 0xFF) - er, dg = (int)((got >> 8) & 0xFF) - t[1];
            wrong += abs(dr) > 1 || abs(dg) > 1 || ((got >> 16) & 0xFF) != 0;
        }
        CHECK(wrong == 0, "vertex colour modulate + alpha test: %ld pixels wrong", wrong);
        h ^= frame_hash() * 11u;
        mesh_free(&m);
    }
    sw_image_destroy(rep); sw_image_destroy(clm);
    free(tex);
    return h;
}

/* ---- test 6: every blend pair against a scalar reference --------------------- */
static float blend_ref_factor(int f, const float *s, const float *d, int c, int isSrc)
{
    switch (f) {
    case SW_BF_ZERO: return 0; case SW_BF_ONE: return 1;
    case SW_BF_SRC_COLOR: return s[c]; case SW_BF_ONE_MINUS_SRC_COLOR: return 1 - s[c];
    case SW_BF_DST_COLOR: return d[c]; case SW_BF_ONE_MINUS_DST_COLOR: return 1 - d[c];
    case SW_BF_SRC_ALPHA: return s[3]; case SW_BF_ONE_MINUS_SRC_ALPHA: return 1 - s[3];
    case SW_BF_DST_ALPHA: return d[3]; case SW_BF_ONE_MINUS_DST_ALPHA: return 1 - d[3];
    default: if (!isSrc || c == 3) return 1; { const float a = s[3], b = 1 - d[3]; return a < b ? a : b; }
    }
}

static uint64_t test_blend(void)
{
    uint64_t h = 0;
    const uint32_t dstCols[8] = { 0xFF000000u, 0x00FFFFFFu, 0x80402010u, 0x40C080FFu, 0xC0FF00A0u, 0x10203040u, 0xFF808080u, 0x7F7F7F7Fu };
    const uint32_t srcCols[3] = { 0x80FF4020u, 0xFF10F0A0u, 0x00808080u };
    Mesh strips[8], fullq;
    TVert dummy[4];
    for (int i = 0; i < 8; i++) screen_quad(&strips[i], dummy, i * (g_w / 8), 0, (i + 1) * (g_w / 8), g_h, 0, 0, 1, 1);
    screen_quad(&fullq, dummy, 0, 0, g_w, g_h, 0, 0, 1, 1);
    long wrong = 0; int worst = 0;
    for (int sf = SW_BF_ZERO; sf <= SW_BF_SRC_ALPHA_SATURATE; sf++)
        for (int df = SW_BF_ZERO; df <= SW_BF_ONE_MINUS_DST_ALPHA; df++) {
            const uint32_t sc = srcCols[(sf + df) % 3];
            SwRect r = full_rect();
            sw_begin_view(&r);
            sw_begin_segment(SW_SEG_COLOR, NULL);
            for (int i = 0; i < 8; i++) { SwDraw d = make_draw(&strips[i], SW_OP_COLOR); d.color = dstCols[i]; sw_draw(&d); }
            SwDraw b = make_draw(&fullq, SW_OP_COLOR);
            b.color = sc; b.srcBlend = sf; b.dstBlend = df;
            sw_draw(&b);
            sw_end_view();
            for (int i = 0; i < 8; i++) {
                const uint32_t got = pixel(i * (g_w / 8) + 7, g_h / 3 + i);
                float s[4], d[4];
                for (int c = 0; c < 4; c++) { s[c] = (float)((sc >> (8 * c)) & 255) / 255.0f; d[c] = (float)((dstCols[i] >> (8 * c)) & 255) / 255.0f; }
                for (int c = 0; c < 4; c++) {
                    float v = s[c] * blend_ref_factor(sf, s, d, c, 1) + d[c] * blend_ref_factor(df, s, d, c, 0);
                    v = v > 1 ? 1 : v;
                    const int e = abs((int)((got >> (8 * c)) & 255) - (int)(v * 255.0f + 0.5f));
                    if (e > worst) worst = e;
                    wrong += e > 1;
                }
            }
            h ^= frame_hash() * (uint64_t)(sf * 16 + df + 1);
        }
    CHECK(wrong == 0, "blend table: %ld channel values off by more than one code (worst %d)", wrong, worst);

    /* write masks: RGB only must leave alpha alone, alpha only must leave colour alone */
    for (int k = 0; k < 2; k++) {
        SwRect r = full_rect();
        sw_begin_view(&r);
        sw_begin_segment(SW_SEG_COLOR, NULL);
        SwDraw d = make_draw(&fullq, SW_OP_COLOR); d.color = 0x11223344u; sw_draw(&d);
        SwDraw b = make_draw(&fullq, SW_OP_COLOR); b.color = 0xAABBCCDDu; b.writeMask = k ? SW_WRITE_A : SW_WRITE_RGB; sw_draw(&b);
        sw_end_view();
        const uint32_t expect = k ? 0xAA223344u : 0x11BBCCDDu;
        CHECK(pixel(g_w / 2, g_h / 2) == expect && pixel(3, g_h - 2) == expect, "write mask %d: %08x, expected %08x", k, pixel(g_w / 2, g_h / 2), expect);
    }
    for (int i = 0; i < 8; i++) mesh_free(&strips[i]);
    mesh_free(&fullq);
    return h;
}

/* ---- a tilted quad in eye space, shared by the perspective and the interaction tests ---- */
typedef struct TQuad { Mesh m; TVert tv[4]; float n[3], t0[3], t1[3]; } TQuad;

static void tilted_quad(TQuad *q, float uvScale)
{
    /* corners of a floor-like plane rising away from the eye; local space IS eye space */
    const float P[4][3] = { { -60, -28, -45 }, { 70, -30, -50 }, { 95, 22, -170 }, { -85, 25, -160 } };
    const float UV[4][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 1 } };
    mesh_init(&q->m, 4, 6);
    /* an orthonormal frame on the plane */
    const float e1[3] = { P[1][0] - P[0][0], P[1][1] - P[0][1], P[1][2] - P[0][2] }, e2[3] = { P[3][0] - P[0][0], P[3][1] - P[0][1], P[3][2] - P[0][2] };
    float n[3] = { e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0] };
    float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]); for (int i = 0; i < 3; i++) n[i] /= l;
    float t0[3]; l = sqrtf(e1[0] * e1[0] + e1[1] * e1[1] + e1[2] * e1[2]); for (int i = 0; i < 3; i++) t0[i] = e1[i] / l;
    const float t1[3] = { n[1] * t0[2] - n[2] * t0[1], n[2] * t0[0] - n[0] * t0[2], n[0] * t0[1] - n[1] * t0[0] };
    memcpy(q->n, n, sizeof(n)); memcpy(q->t0, t0, sizeof(t0)); memcpy(q->t1, t1, sizeof(t1));
    for (int i = 0; i < 4; i++) {
        /* corner 2 is pushed off the plane a little?  No: keep it planar so one ray cast serves both triangles */
        mesh_vert(&q->m, project_eye(P[i][0], P[i][1], P[i][2], 1));
        TVert *v = &q->tv[i];
        memset(v, 0, sizeof(*v));
        memcpy(v->xyz, P[i], sizeof(float) * 3);
        v->st[0] = UV[i][0] * uvScale; v->st[1] = UV[i][1] * uvScale;
        memcpy(v->normal, n, sizeof(n)); memcpy(v->tan0, t0, sizeof(t0)); memcpy(v->tan1, t1, sizeof(t1));
        v->color[0] = (uint8_t)(255 - 40 * i); v->color[1] = (uint8_t)(120 + 40 * i); v->color[2] = 200; v->color[3] = 255;
    }
    mesh_tri(&q->m, 0, 1, 2); mesh_tri(&q->m, 0, 2, 3);
}

/* Where the eye ray through a pixel centre meets the quad: the covering triangle and its
   barycentrics (in 3D, so interpolating with them IS perspective-correct). 0 = miss or too
   close to an edge for a fair comparison. */
static int quad_hit(const TQuad *q, int px, int py, int *tri, double bary[3])
{
    const double tx = tan(45.0 * 3.14159265358979 / 180.0), ty = tx * g_h / g_w;
    const double dir[3] = { (((double)px + 0.5) / g_w * 2 - 1) * tx, (1 - ((double)py + 0.5) / g_h * 2) * ty, -1.0 };
    const float *p0 = q->tv[0].xyz;
    /* the four corners are not exactly coplanar: intersect each triangle's own plane */
    for (int t = 0; t < 2; t++) {
        const float *a = q->tv[0].xyz, *b = q->tv[t + 1].xyz, *c = q->tv[t + 2].xyz;
        const double e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
        const double pv[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
        const double det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
        if (fabs(det) < 1e-12) continue;
        const double tvv[3] = { -a[0], -a[1], -a[2] };
        const double u = (tvv[0] * pv[0] + tvv[1] * pv[1] + tvv[2] * pv[2]) / det;
        const double qv[3] = { tvv[1] * e1[2] - tvv[2] * e1[1], tvv[2] * e1[0] - tvv[0] * e1[2], tvv[0] * e1[1] - tvv[1] * e1[0] };
        const double v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) / det;
        const double m = 0.01;
        if (u > m && v > m && u + v < 1 - m) { *tri = t; bary[0] = 1 - u - v; bary[1] = u; bary[2] = v; (void)p0; return 1; }
    }
    return 0;
}

/* ---- test 7: varyings are perspective-correct --------------------------------- */
static uint64_t test_perspective(void)
{
    const int S = 1024;
    uint8_t *tex = (uint8_t *)malloc((size_t)S * S * 4);
    for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
        uint8_t *t = tex + ((size_t)y * S + x) * 4;
        t[0] = (uint8_t)((x * 255 + 511) / 1023); t[1] = (uint8_t)((y * 255 + 511) / 1023); t[2] = 0; t[3] = 255;
    }
    const uint8_t *lv[1] = { tex };
    SwImage *im = sw_image_create(S, S, 1, lv, SW_WRAP_CLAMP);
    TQuad q; tilted_quad(&q, 1.0f);
    sw_clear_framebuffer(0xFF000000u);
    SwStageParms p = stage_parms(im);
    draw_stage(&q.m, q.tv, &p, SW_BF_ONE, SW_BF_ZERO);
    static const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    long samples = 0, wrong = 0; int worst = 0;
    for (int y = 1; y < g_h; y += 4) for (int x = 1; x < g_w; x += 4) {
        int tri; double b[3];
        if (!quad_hit(&q, x, y, &tri, b)) continue;
        double u = 0, v = 0;
        for (int k = 0; k < 3; k++) { u += b[k] * q.tv[order[tri][k]].st[0]; v += b[k] * q.tv[order[tri][k]].st[1]; }
        const uint32_t got = pixel(x, y);
        const int er = abs((int)(got & 255) - (int)(u * 255 + 0.5)), eg = abs((int)((got >> 8) & 255) - (int)(v * 255 + 0.5));
        const int e = er > eg ? er : eg;
        if (e > worst) worst = e;
        wrong += e > 2; samples++;
    }
    CHECK(samples > 20000, "perspective: only %ld samples hit the quad", samples);
    CHECK(wrong == 0, "perspective: %ld of %ld samples are off by more than 2 codes (worst %d)", wrong, samples, worst);
    const uint64_t h = frame_hash();
    write_tga("perspective_uv", 0);
    mesh_free(&q.m); sw_image_destroy(im); free(tex);
    return h;
}

/* ---- test 8: k_interaction against a scalar interaction.vfp ------------------- */
typedef struct RefTex { const uint8_t *px; int w, h, clamp; } RefTex;

static void ref_sample(const RefTex *t, double u, double v, double out[4])
{
    double x, y;
    if (t->clamp) {
        x = u * t->w - 0.5; y = v * t->h - 0.5;
        x = x < 0 ? 0 : (x > t->w - 1 ? t->w - 1 : x); y = y < 0 ? 0 : (y > t->h - 1 ? t->h - 1 : y);
    } else {
        x = (u - floor(u)) * t->w - 0.5; y = (v - floor(v)) * t->h - 0.5;
    }
    const double xf = floor(x), yf = floor(y), fx = x - xf, fy = y - yf;
    int x0 = (int)xf, y0 = (int)yf, x1 = x0 + 1, y1 = y0 + 1;
    if (t->clamp) { x1 = x1 > t->w - 1 ? t->w - 1 : x1; y1 = y1 > t->h - 1 ? t->h - 1 : y1; }
    else { x0 = (x0 + t->w) % t->w; y0 = (y0 + t->h) % t->h; x1 = (x1 + t->w) % t->w; y1 = (y1 + t->h) % t->h; }
    for (int c = 0; c < 4; c++) {
        const double a = t->px[(y0 * t->w + x0) * 4 + c], b = t->px[(y0 * t->w + x1) * 4 + c];
        const double cc = t->px[(y1 * t->w + x0) * 4 + c], d = t->px[(y1 * t->w + x1) * 4 + c];
        out[c] = ((a * (1 - fx) + b * fx) * (1 - fy) + (cc * (1 - fx) + d * fx) * fy) / 255.0;
    }
}

static uint8_t *noise_tex(int w, int h, unsigned seed, int lo, int hi)
{
    uint8_t *t = (uint8_t *)malloc((size_t)w * h * 4);
    for (int i = 0; i < w * h * 4; i++) t[i] = (uint8_t)(lo + (int)(t_rand(&seed) % (unsigned)(hi - lo + 1)));
    return t;
}

typedef struct LightSetup {
    SwInteractionParms p;
    uint8_t *bump, *diff, *spec, *fall, *proj;
    int specSize;
    SwImage *ibump, *idiff, *ispec, *ifall, *iproj;
} LightSetup;

static void light_setup(LightSetup *L, const TQuad *q, int specSize, int mips)
{
    memset(L, 0, sizeof(*L));
    const int B = 64;
    L->specSize = specSize;
    /* a smooth bump field: x goes to ALPHA, as DOOM 3 stores it */
    L->bump = (uint8_t *)malloc((size_t)B * B * 4);
    for (int y = 0; y < B; y++) for (int x = 0; x < B; x++) {
        const double nx = 0.45 * sin(x * 0.39), ny = 0.45 * cos(y * 0.31), nz = sqrt(1 - nx * nx - ny * ny);
        uint8_t *t = L->bump + (y * B + x) * 4;
        t[3] = (uint8_t)((nx * 0.5 + 0.5) * 255 + 0.5); t[1] = (uint8_t)((ny * 0.5 + 0.5) * 255 + 0.5); t[2] = (uint8_t)((nz * 0.5 + 0.5) * 255 + 0.5); t[0] = 0;
    }
    L->diff = noise_tex(B, B, 11, 60, 255);
    L->spec = noise_tex(specSize, specSize, 23, 0, 255);
    /* falloff: a ramp along s with black borders; projection: a disc with a black border */
    L->fall = (uint8_t *)malloc(64 * 8 * 4);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 64; x++) {
        const int v = (x == 0 || x == 63 || y == 0 || y == 7) ? 0 : (int)(255 * (1.0 - fabs(x - 31.5) / 40.0));
        uint8_t *t = L->fall + (y * 64 + x) * 4; t[0] = t[1] = t[2] = t[3] = (uint8_t)v;
    }
    L->proj = (uint8_t *)malloc((size_t)B * B * 4);
    for (int y = 0; y < B; y++) for (int x = 0; x < B; x++) {
        const double d = sqrt((x - 31.5) * (x - 31.5) + (y - 31.5) * (y - 31.5)) / 30.0;
        const int v = d >= 1 ? 0 : (int)(255 * (1 - d * d));
        uint8_t *t = L->proj + (y * B + x) * 4; t[0] = (uint8_t)v; t[1] = (uint8_t)(v * 9 / 10); t[2] = (uint8_t)(v * 7 / 10); t[3] = (uint8_t)v;
    }
    uint8_t *lv[16];
    #define MAKE(img, data, w, h, wrap) do { const int n = mips ? make_mips(data, w, h, lv, 16) : (lv[0] = data, 1); \
        img = sw_image_create(w, h, n, (const uint8_t *const *)lv, wrap); for (int i_ = 1; i_ < n; i_++) free(lv[i_]); } while (0)
    MAKE(L->ibump, L->bump, B, B, SW_WRAP_REPEAT); MAKE(L->idiff, L->diff, B, B, SW_WRAP_REPEAT);
    MAKE(L->ispec, L->spec, specSize, specSize, SW_WRAP_REPEAT);
    MAKE(L->ifall, L->fall, 64, 8, SW_WRAP_CLAMP); MAKE(L->iproj, L->proj, B, B, SW_WRAP_CLAMP);
    #undef MAKE

    SwInteractionParms *p = &L->p;
    memset(p, 0, sizeof(*p));               /* specularMax 0 = the table's end at 1, which the reference below models */
    /* a light above the far half of the quad, the eye at the origin */
    p->localLightOrigin[0] = 10; p->localLightOrigin[1] = 45; p->localLightOrigin[2] = -105;
    /* the projection looks down the plane normal: s, t along the tangents, q grows with distance */
    const float *c0 = q->tv[0].xyz;
    const float centre[3] = { 5, -3, -105 };
    for (int i = 0; i < 3; i++) {
        p->lightProjectS[i] = q->t0[i] / 180.0f + (-q->n[i]) * 0.5f / 60.0f;
        p->lightProjectT[i] = q->t1[i] / 180.0f + (-q->n[i]) * 0.5f / 60.0f;
        p->lightProjectQ[i] = -q->n[i] / 60.0f;
        p->lightFalloffS[i] = q->t0[i] / 260.0f;
    }
    /* offsets so that the centre of the quad sits near s/q = t/q = 0.5 and falloff 0.5 */
    const float qc = -(q->n[0] * (centre[0] - p->localLightOrigin[0]) + q->n[1] * (centre[1] - p->localLightOrigin[1]) + q->n[2] * (centre[2] - p->localLightOrigin[2])) / 60.0f;
    p->lightProjectQ[3] = qc - (p->lightProjectQ[0] * centre[0] + p->lightProjectQ[1] * centre[1] + p->lightProjectQ[2] * centre[2]);
    p->lightProjectS[3] = 0.5f * qc - (p->lightProjectS[0] * centre[0] + p->lightProjectS[1] * centre[1] + p->lightProjectS[2] * centre[2]);
    p->lightProjectT[3] = 0.5f * qc - (p->lightProjectT[0] * centre[0] + p->lightProjectT[1] * centre[1] + p->lightProjectT[2] * centre[2]);
    p->lightFalloffS[3] = 0.5f - (p->lightFalloffS[0] * centre[0] + p->lightFalloffS[1] * centre[1] + p->lightFalloffS[2] * centre[2]);
    (void)c0;
    for (int r = 0; r < 2; r++) { p->bumpMatrix[r][r] = 1; p->diffuseMatrix[r][r] = 1; p->specularMatrix[r][r] = 1; }
    p->specularMatrix[0][3] = specSize == 64 ? 0.0f : 0.25f;       /* a scrolled specular map in the unshared variant */
    p->vertexColorModulate = 1; p->vertexColorAdd = 0;
    p->diffuseColor[0] = 1.4f; p->diffuseColor[1] = 1.2f; p->diffuseColor[2] = 1.0f; p->diffuseColor[3] = 1.0f;
    p->specularColor[0] = 0.9f; p->specularColor[1] = 0.9f; p->specularColor[2] = 1.1f; p->specularColor[3] = 1.0f;
    p->bump = L->ibump; p->diffuse = L->idiff; p->specular = L->ispec; p->falloff = L->ifall; p->projection = L->iproj;
}

static void light_free(LightSetup *L)
{
    sw_image_destroy(L->ibump); sw_image_destroy(L->idiff); sw_image_destroy(L->ispec); sw_image_destroy(L->ifall); sw_image_destroy(L->iproj);
    free(L->bump); free(L->diff); free(L->spec); free(L->fall); free(L->proj);
}

/* the vertex program of interaction.vfp, in double */
static void ref_vertex(const SwInteractionParms *p, const TVert *v, double var[20])
{
    const double L[3] = { p->localLightOrigin[0] - v->xyz[0], p->localLightOrigin[1] - v->xyz[1], p->localLightOrigin[2] - v->xyz[2] };
    const double V[3] = { p->localViewOrigin[0] - v->xyz[0], p->localViewOrigin[1] - v->xyz[1], p->localViewOrigin[2] - v->xyz[2] };
    #define D3(a, b) ((a)[0] * (b)[0] + (a)[1] * (b)[1] + (a)[2] * (b)[2])
    var[0] = D3(v->tan0, L); var[1] = D3(v->tan1, L); var[2] = D3(v->normal, L);
    var[3] = v->st[0] * p->bumpMatrix[0][0] + v->st[1] * p->bumpMatrix[0][1] + p->bumpMatrix[0][3];
    var[4] = v->st[0] * p->bumpMatrix[1][0] + v->st[1] * p->bumpMatrix[1][1] + p->bumpMatrix[1][3];
    var[5] = D3(v->xyz, p->lightFalloffS) + p->lightFalloffS[3];
    var[6] = D3(v->xyz, p->lightProjectS) + p->lightProjectS[3];
    var[7] = D3(v->xyz, p->lightProjectT) + p->lightProjectT[3];
    var[8] = D3(v->xyz, p->lightProjectQ) + p->lightProjectQ[3];
    var[9] = v->st[0] * p->diffuseMatrix[0][0] + v->st[1] * p->diffuseMatrix[0][1] + p->diffuseMatrix[0][3];
    var[10] = v->st[0] * p->diffuseMatrix[1][0] + v->st[1] * p->diffuseMatrix[1][1] + p->diffuseMatrix[1][3];
    var[11] = v->st[0] * p->specularMatrix[0][0] + v->st[1] * p->specularMatrix[0][1] + p->specularMatrix[0][3];
    var[12] = v->st[0] * p->specularMatrix[1][0] + v->st[1] * p->specularMatrix[1][1] + p->specularMatrix[1][3];
    const double ll = sqrt(D3(L, L)), lv = sqrt(D3(V, V));
    const double H[3] = { L[0] / ll + V[0] / lv, L[1] / ll + V[1] / lv, L[2] / ll + V[2] / lv };
    var[13] = D3(v->tan0, H); var[14] = D3(v->tan1, H); var[15] = D3(v->normal, H);
    for (int i = 0; i < 4; i++) var[16 + i] = v->color[i] / 255.0 * p->vertexColorModulate + p->vertexColorAdd;
    #undef D3
}

static uint64_t test_interaction(void)
{
    uint64_t h = 0;
    TQuad q; tilted_quad(&q, 6.0f);
    Mesh w; wall(&w, -400.0f);
    static const int order[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    for (int variant = 0; variant < 2; variant++) {
        LightSetup L; light_setup(&L, &q, variant ? 64 : 32, 0);   /* 64: specular shares the bump coordinates */
        sw_clear_framebuffer(0xFF000000u);
        SwRect r = full_rect();
        sw_begin_view(&r);
        sw_begin_segment(SW_SEG_DEPTH, NULL);
        SwDraw dd = make_draw(&q.m, SW_OP_DEPTH_FILL); sw_draw(&dd);
        sw_begin_segment(SW_SEG_LIGHT, NULL);
        SwDraw ld = make_draw(&q.m, SW_OP_COLOR);
        ld.kernel = SW_KERN_INTERACTION; ld.kernelParms = &L.p; ld.verts = tvert_source(q.tv);
        ld.depthTest = SW_DEPTH_EQUAL; ld.srcBlend = SW_BF_ONE; ld.dstBlend = SW_BF_ONE;
        sw_draw(&ld);
        sw_end_view();
        SwStats s; sw_get_stats(&s);

        const RefTex rb = { L.bump, 64, 64, 0 }, rd = { L.diff, 64, 64, 0 }, rs = { L.spec, L.specSize, L.specSize, 0 };
        const RefTex rf = { L.fall, 64, 8, 1 }, rp = { L.proj, 64, 64, 1 };
        double vv[4][20];
        for (int i = 0; i < 4; i++) ref_vertex(&L.p, &q.tv[i], vv[i]);
        long samples = 0, lit = 0, wrong = 0; int worst = 0; double sumErr = 0;
        uint8_t *errMap = (uint8_t *)calloc((size_t)g_w * g_h, 3);
        for (int y = 2; y < g_h; y += 3) for (int x = 2; x < g_w; x += 3) {
            int tri; double b[3];
            if (!quad_hit(&q, x, y, &tri, b)) continue;
            double v[20];
            for (int k = 0; k < 20; k++) v[k] = b[0] * vv[order[tri][0]][k] + b[1] * vv[order[tri][1]][k] + b[2] * vv[order[tri][2]][k];
            double out[4] = { 0, 0, 0, 0 };
            if (v[8] > 0) {
                double n4[4], pr[4], fa[4], di[4], sp[4];
                ref_sample(&rb, v[3], v[4], n4);
                const double N[3] = { n4[3] * 2 - 1, n4[1] * 2 - 1, n4[2] * 2 - 1 };
                const double ll = sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                const double ndl = (N[0] * v[0] + N[1] * v[1] + N[2] * v[2]) / ll;
                ref_sample(&rp, v[6] / v[8], v[7] / v[8], pr);
                ref_sample(&rf, v[5], 0.5, fa);
                ref_sample(&rd, v[9], v[10], di);
                ref_sample(&rs, v[11], v[12], sp);
                const double lh = sqrt(v[13] * v[13] + v[14] * v[14] + v[15] * v[15]);
                double sc = ((N[0] * v[13] + N[1] * v[14] + N[2] * v[15]) / lh - 0.75) * 4;
                sc = sc < 0 ? 0 : (sc > 1 ? 1 : sc); sc *= sc;
                for (int c = 0; c < 4; c++) {
                    const double color = di[c] * L.p.diffuseColor[c] + sc * L.p.specularColor[c] * 2 * sp[c];
                    const double o = color * ndl * pr[c] * fa[c] * v[16 + c];
                    out[c] = o < 0 ? 0 : (o > 1 ? 1 : o);
                }
            }
            const uint32_t got = pixel(x, y);
            int e = 0;
            for (int c = 0; c < 3; c++) { const int ec = abs((int)((got >> (8 * c)) & 255) - (int)(out[c] * 255 + 0.5)); if (ec > e) e = ec; }
            if (e > worst) worst = e;
            sumErr += e; wrong += e > 4; samples++;
            lit += out[0] > 0.02;
            for (int yy = y - 1; yy <= y + 1; yy++) for (int xx = x - 1; xx <= x + 1; xx++) {
                if (xx < 0 || yy < 0 || xx >= g_w || yy >= g_h) continue;
                uint8_t *ep = errMap + ((size_t)yy * g_w + xx) * 3;
                ep[2] = (uint8_t)(e * 10 > 255 ? 255 : e * 10);      /* red = error x 10 */
                ep[1] = (uint8_t)(out[1] * 255);                    /* green = the reference image */
            }
        }
        if (!variant) {
            char path[512];
            snprintf(path, sizeof(path), "%s/sw_interaction_error_%dx%d.tga", g_outDir, g_w, g_h);
            FILE *f = fopen(path, "wb");
            if (f) {
                uint8_t hdr[18] = { 0 };
                hdr[2] = 2; hdr[12] = (uint8_t)(g_w & 255); hdr[13] = (uint8_t)(g_w >> 8);
                hdr[14] = (uint8_t)(g_h & 255); hdr[15] = (uint8_t)(g_h >> 8); hdr[16] = 24; hdr[17] = 0x20;
                fwrite(hdr, 1, 18, f); fwrite(errMap, 1, (size_t)g_w * g_h * 3, f); fclose(f);
            }
        }
        free(errMap);
        printf("    interaction (%s): %ld samples, %ld lit, worst error %d codes, mean %.3f; kernel calls %llu, dark blocks %llu\n",
               variant ? "shared coordinates" : "own specular coordinates", samples, lit, worst, sumErr / (samples ? samples : 1),
               (unsigned long long)s.kernelCalls, (unsigned long long)s.lightBlocksDark);
        CHECK(samples > 20000 && lit > 5000, "interaction: %ld samples, %ld lit: the scene does not exercise the kernel", samples, lit);
        CHECK(wrong == 0, "interaction: %ld of %ld samples are off by more than 4 codes (worst %d)", wrong, samples, worst);
        CHECK(s.equalFailures == 0, "interaction: depth EQUAL failures");
        const uint64_t hOn = frame_hash();
        h ^= hOn * (uint64_t)(5 + variant);
        if (!variant) write_tga("interaction", 0);
        {   /* the light-cell reject is exact: the same view without it is the same image, bit for bit */
            sw_set_option(SW_OPT_LIGHT_CELLS, 0);
            sw_clear_framebuffer(0xFF000000u);
            sw_begin_view(&r);
            sw_begin_segment(SW_SEG_DEPTH, NULL); sw_draw(&dd);
            sw_begin_segment(SW_SEG_LIGHT, NULL); sw_draw(&ld);
            sw_end_view();
            sw_set_option(SW_OPT_LIGHT_CELLS, 1);
            CHECK(frame_hash() == hOn, "interaction: the light-cell reject changes the image");
            CHECK(s.lightCellsDark > 0, "interaction: the light-cell reject never fired");
        }
        light_free(&L);
    }
    mesh_free(&q.m); mesh_free(&w);
    return h;
}

/* ---- test 9: cube maps: face selection and orientation against GL's table ------ */
static uint64_t test_cube(void)
{
    const int S = 64;
    uint8_t *facePx[6]; const uint8_t *lv[6][1]; const uint8_t *const *faces[6];
    for (int f = 0; f < 6; f++) {
        facePx[f] = (uint8_t *)malloc((size_t)S * S * 4);
        for (int y = 0; y < S; y++) for (int x = 0; x < S; x++) {
            uint8_t *t = facePx[f] + ((size_t)y * S + x) * 4;
            t[0] = (uint8_t)(f * 40 + 10); t[1] = (uint8_t)(x * 4); t[2] = (uint8_t)(y * 4); t[3] = 255;
        }
        lv[f][0] = facePx[f]; faces[f] = lv[f];
    }
    SwImage *cube = sw_image_create_cube(S, 1, faces);
    uint64_t h = 0;
    long wrong = 0, samples = 0; int seen[6] = { 0 }, worst = 0;
    for (int pass = 0; pass < 2; pass++) {
        const float dz = pass ? 0.6f : -0.6f;
        Mesh m; TVert tv[4];
        screen_quad(&m, tv, 0, 0, g_w, g_h, 0, 0, 1, 1);
        /* directions are an affine function of the screen position, so both triangles agree */
        const float dirs[4][3] = { { -1, 1, dz }, { 1, 1, dz }, { 1, -1, dz }, { -1, -1, dz } };
        sw_clear_framebuffer(0xFF000000u);
        SwStageParms p = stage_parms(cube);
        p.texgen = SW_TG_DYNAMIC3;
        SwRect r = full_rect();
        sw_begin_view(&r);
        sw_begin_segment(SW_SEG_COLOR, NULL);
        SwDraw d = make_draw(&m, SW_OP_COLOR);
        d.kernel = SW_KERN_STAGE; d.kernelParms = &p; d.verts = tvert_source(tv);
        d.verts.texCoords3 = dirs[0]; d.verts.texCoords3Stride = 3 * sizeof(float);
        sw_draw(&d);
        sw_end_view();
        for (int y = 3; y < g_h; y += 7) for (int x = 3; x < g_w; x += 7) {
            const double rx = ((double)x + 0.5) / g_w * 2 - 1, ry = 1 - ((double)y + 0.5) / g_h * 2, rz = dz;
            const double ax = fabs(rx), ay = fabs(ry), az = fabs(rz);
            if (fabs(ax - ay) < 0.01 || fabs(ax - az) < 0.01 || fabs(ay - az) < 0.01) continue;   /* a face seam */
            int f; double sc, tc, ma;
            if (ax >= ay && ax >= az) { f = rx > 0 ? 0 : 1; sc = rx > 0 ? -rz : rz; tc = -ry; ma = ax; }
            else if (ay >= az) { f = ry > 0 ? 2 : 3; sc = rx; tc = ry > 0 ? rz : -rz; ma = ay; }
            else { f = rz > 0 ? 4 : 5; sc = rz > 0 ? rx : -rx; tc = -ry; ma = az; }
            double tx = (sc / ma * 0.5 + 0.5) * S - 0.5, ty = (tc / ma * 0.5 + 0.5) * S - 0.5;
            tx = tx < 0 ? 0 : (tx > S - 1 ? S - 1 : tx); ty = ty < 0 ? 0 : (ty > S - 1 ? S - 1 : ty);
            const uint32_t got = pixel(x, y);
            const int eg = abs((int)((got >> 8) & 255) - (int)(tx * 4 + 0.5)), eb = abs((int)((got >> 16) & 255) - (int)(ty * 4 + 0.5));
            const int e = eg > eb ? eg : eb;
            if (e > worst) worst = e;
            wrong += (int)(got & 255) != f * 40 + 10 || e > 2;
            samples++; seen[f]++;
        }
        h ^= frame_hash() * (uint64_t)(pass + 1);
        if (!pass) write_tga("cube", 0);
        mesh_free(&m);
    }
    CHECK(wrong == 0, "cube map: %ld of %ld samples read the wrong face or are off by more than 2 codes (worst %d)", wrong, samples, worst);
    for (int f = 0; f < 6; f++) CHECK(seen[f] > 100, "cube map: face %d was hardly sampled (%d)", f, seen[f]);
    sw_image_destroy(cube);
    for (int f = 0; f < 6; f++) free(facePx[f]);
    return h;
}
