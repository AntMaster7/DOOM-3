/* test/sw_test.c -- the standalone harness of the software rasterizer core (plan, Stage 2).
   Synthetic scenes, each checked against an expectation and hashed; the hashes must not depend on
   the thread count or on the run. Then the micro-benches that feed the frame-time model.

     sw_test.exe                 all tests, then the benches at 1920x1080
     sw_test.exe --notests / --nobench / --res W H / --threads N / --out DIR

   Build: test\build.bat (x64, the same sw_core.c the engine will compile). */
#define _CRT_SECURE_NO_WARNINGS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stddef.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../sw_api.h"

static int g_w = 1920, g_h = 1080;
static int g_fail;
static int g_oracles;        /* --oracles: the core was built with /DSW_ORACLES=1 */
static const char *g_outDir = ".";
static float *g_depth;
static uint8_t *g_stencil;

#define CHECK(cond, ...) do { if (!(cond)) { g_fail++; printf("    FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- small helpers -------------------------------------------------------- */
typedef struct { float x, y, z, w; } V4;
typedef struct { V4 *v; int nv; int *idx; int ni; } Mesh;

static void mesh_init(Mesh *m, int maxV, int maxI) { m->v = (V4 *)malloc(sizeof(V4) * maxV); m->idx = (int *)malloc(sizeof(int) * maxI); m->nv = m->ni = 0; }
static void mesh_free(Mesh *m) { free(m->v); free(m->idx); }
static int mesh_vert(Mesh *m, V4 v) { m->v[m->nv] = v; return m->nv++; }
static void mesh_tri(Mesh *m, int a, int b, int c) { m->idx[m->ni++] = a; m->idx[m->ni++] = b; m->idx[m->ni++] = c; }

/* DOOM 3's projection (R_SetupProjection: infinite far plane), then the interface's depth
   convention z' = (z + w) / 2. ew = 1 for points, 0 for directions (shadow vertices at infinity). */
static const float ZNEAR = 3.0f;
static V4 project_eye(float ex, float ey, float ez, float ew)
{
    const float fovx = 90.0f * 3.14159265f / 180.0f;
    const float xmax = ZNEAR * tanf(fovx * 0.5f), ymax = xmax * (float)g_h / (float)g_w;
    V4 c;
    c.x = ex * (ZNEAR / xmax);
    c.y = ey * (ZNEAR / ymax);
    const float zc = -0.999f * ez - 2.0f * ZNEAR * ew;
    c.w = -ez;
    c.z = (zc + c.w) * 0.5f;
    return c;
}

static SwRect full_rect(void) { SwRect r = { 0, 0, g_w - 1, g_h - 1 }; return r; }

static SwDraw make_draw(const Mesh *m, int op)
{
    SwDraw d;
    memset(&d, 0, sizeof(d));
    d.clip = &m->v[0].x; d.clipStride = sizeof(V4);
    d.indexes = m->idx; d.numIndexes = m->ni;
    d.op = op; d.cull = SW_CULL_TWO_SIDED;
    d.scissor = full_rect();
    d.color = 0xFF000000u;
    d.depthTest = SW_DEPTH_ALWAYS;
    d.kernel = SW_KERN_FLAT; d.srcBlend = SW_BF_ONE; d.dstBlend = SW_BF_ZERO;
    return d;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* hash of the visible framebuffer plus the captured depth and stencil */
static uint64_t frame_hash(void)
{
    int pitch;
    const uint32_t *fb = sw_framebuffer(&pitch);
    uint64_t h = 14695981039346656037ull;
    for (int y = 0; y < g_h; y++) h = fnv(h, fb + (size_t)y * pitch, (size_t)g_w * 4);
    h = fnv(h, g_depth, (size_t)g_w * g_h * sizeof(float));
    h = fnv(h, g_stencil, (size_t)g_w * g_h);
    return h;
}

static uint32_t pixel(int x, int y)
{
    int pitch;
    const uint32_t *fb = sw_framebuffer(&pitch);
    return fb[(size_t)y * pitch + x];
}

static void write_tga(const char *name, int stencilView)
{
    char path[512];
    snprintf(path, sizeof(path), "%s/sw_%s_%dx%d.tga", g_outDir, name, g_w, g_h);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    uint8_t hdr[18] = { 0 };
    hdr[2] = 2; hdr[12] = (uint8_t)(g_w & 255); hdr[13] = (uint8_t)(g_w >> 8);
    hdr[14] = (uint8_t)(g_h & 255); hdr[15] = (uint8_t)(g_h >> 8); hdr[16] = 24; hdr[17] = 0x20;   /* top row first */
    fwrite(hdr, 1, 18, f);
    uint8_t *row = (uint8_t *)malloc((size_t)g_w * 3);
    for (int y = 0; y < g_h; y++) {
        for (int x = 0; x < g_w; x++) {
            uint32_t p = pixel(x, y);
            if (stencilView) {                              /* 128 grey, more = red, less = blue */
                const int s = g_stencil[(size_t)y * g_w + x];
                p = s == 128 ? 0x404040u : (s > 128 ? (uint32_t)(80 + 40 * (s - 128 > 4 ? 4 : s - 128)) : 0xFF0000u);
            }
            row[x * 3 + 0] = (uint8_t)(p >> 16); row[x * 3 + 1] = (uint8_t)(p >> 8); row[x * 3 + 2] = (uint8_t)p;
        }
        fwrite(row, 1, (size_t)g_w * 3, f);
    }
    free(row);
    fclose(f);
}

/* ---- test 1: the winding row of the conventions table --------------------- */
static uint64_t test_winding(void)
{
    /* counter-clockwise in GL's y-up window space = GL front = what CT_FRONT_SIDED culls */
    Mesh ccw, cw;
    mesh_init(&ccw, 3, 3); mesh_init(&cw, 3, 3);
    V4 a = { -0.5f, -0.5f, 0.5f, 1 }, b = { 0.5f, -0.5f, 0.5f, 1 }, c = { 0.0f, 0.5f, 0.5f, 1 };
    mesh_vert(&ccw, a); mesh_vert(&ccw, b); mesh_vert(&ccw, c); mesh_tri(&ccw, 0, 1, 2);
    mesh_vert(&cw, a); mesh_vert(&cw, b); mesh_vert(&cw, c); mesh_tri(&cw, 0, 2, 1);

    static const struct { int meshCW, cull, mirror, visible; } cases[] = {
        { 0, SW_CULL_FRONT_SIDED, 0, 0 }, { 1, SW_CULL_FRONT_SIDED, 0, 1 },
        { 0, SW_CULL_BACK_SIDED, 0, 1 },  { 1, SW_CULL_BACK_SIDED, 0, 0 },
        { 0, SW_CULL_FRONT_SIDED, 1, 1 }, { 1, SW_CULL_FRONT_SIDED, 1, 0 },
        { 0, SW_CULL_TWO_SIDED, 0, 1 },   { 1, SW_CULL_TWO_SIDED, 0, 1 },
    };
    uint64_t h = 0;
    for (int i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        sw_clear_framebuffer(0xFF000000u);
        SwRect r = full_rect();
        sw_begin_view(&r);
        sw_begin_segment(SW_SEG_COLOR, NULL);
        SwDraw d = make_draw(cases[i].meshCW ? &cw : &ccw, SW_OP_COLOR);
        d.cull = cases[i].cull; d.mirror = cases[i].mirror; d.color = 0xFFFFFFFFu;
        sw_draw(&d);
        sw_end_view();
        SwStats s; sw_get_stats(&s);
        const int visible = s.pxColor > 0;
        CHECK(visible == cases[i].visible, "winding case %d: visible %d, expected %d", i, visible, cases[i].visible);
        /* base 1 x height 1 in NDC, where the frame is 2 x 2: an eighth of it, give or take edge pixels */
        if (visible) CHECK(fabs((double)s.pxColor - 0.125 * g_w * g_h) < 2.0 * (g_w + g_h), "winding case %d: %llu px", i, (unsigned long long)s.pxColor);
        /* y must point up in clip space: the apex is in the upper half of the frame */
        if (visible) CHECK((pixel(g_w / 2, g_h / 4 + 8) & 0xFF) == 0xFF && (pixel(g_w / 8, g_h / 4 + 8) & 0xFF) == 0, "winding case %d: apex is not on top", i);
        h ^= frame_hash() * (uint64_t)(i + 1);
    }
    mesh_free(&ccw); mesh_free(&cw);
    return h;
}

/* ---- test 2: shared edges are watertight ---------------------------------- */
static void check_coverage_once(const char *what, int mustCoverAll)
{
    long over = 0, holes = 0, uncovered = 0;
    for (int y = 0; y < g_h; y++)
        for (int x = 0; x < g_w; x++) {
            const int v = (int)(pixel(x, y) & 0xFF);
            if (v > 1) over++;
            if (v == 0) {
                uncovered++;
                const int l = x > 0 && (pixel(x - 1, y) & 0xFF), r = x < g_w - 1 && (pixel(x + 1, y) & 0xFF);
                const int u = y > 0 && (pixel(x, y - 1) & 0xFF), dn = y < g_h - 1 && (pixel(x, y + 1) & 0xFF);
                if ((l && r) || (u && dn)) holes++;
            }
        }
    CHECK(over == 0, "%s: %ld pixels drawn more than once", what, over);
    CHECK(holes == 0, "%s: %ld crack pixels", what, holes);
    if (mustCoverAll) CHECK(uncovered == 0, "%s: %ld pixels not covered", what, uncovered);
}

static void draw_additive(const Mesh *m)
{
    sw_clear_framebuffer(0xFF000000u);
    SwRect r = full_rect();
    sw_begin_view(&r);
    sw_begin_segment(SW_SEG_COLOR, NULL);
    SwDraw d = make_draw(m, SW_OP_COLOR);
    d.color = 0x00010101u; d.dstBlend = SW_BF_ONE;
    sw_draw(&d);
    sw_end_view();
}

static uint64_t test_watertight(void)
{
    uint64_t h = 0;
    const int N = 61;
    /* a, b: fans in the screen plane around an off-centre sub-pixel point; b's rim is far
       outside the guard band, so every triangle is guard-band clipped and the frame is covered */
    for (int variant = 0; variant < 2; variant++) {
        Mesh m; mesh_init(&m, N + 1, N * 3);
        V4 c = { 0.1234567f, -0.0765432f, 0.5f, 1 };
        mesh_vert(&m, c);
        const float rad = variant == 0 ? 0.9f : 2000.0f;
        for (int i = 0; i < N; i++) {
            const float a = (float)i * 6.2831853f / (float)N + 0.01f;
            V4 p = { c.x + rad * cosf(a), c.y + rad * sinf(a) * (float)g_w / (float)g_h, 0.5f, 1 };
            mesh_vert(&m, p);
        }
        for (int i = 0; i < N; i++) mesh_tri(&m, 0, 1 + i, 1 + (i + 1) % N);
        draw_additive(&m);
        check_coverage_once(variant == 0 ? "fan on screen" : "fan through the guard band", variant == 1);
        if (variant == 1) { SwStats s; sw_get_stats(&s); CHECK(s.trisClipped == N && s.trisDropped == 0, "guard band fan: %d clipped, %d dropped", s.trisClipped, s.trisDropped); }
        h ^= frame_hash() * (uint64_t)(variant + 3);
        if (variant == 1) write_tga("fan_guardband", 0);
        mesh_free(&m);
    }
    /* c: a ground-plane fan in perspective whose rim passes behind the eye: near-plane clipping
       of shared edges, vertices with negative w */
    {
        Mesh m; mesh_init(&m, N + 1, N * 3);
        mesh_vert(&m, project_eye(0.37f, -8.0f, -40.0f, 1));
        for (int i = 0; i < N; i++) {
            const float a = (float)i * 6.2831853f / (float)N + 0.02f;
            mesh_vert(&m, project_eye(0.37f + 300.0f * cosf(a), -8.0f, -40.0f + 300.0f * sinf(a), 1));
        }
        for (int i = 0; i < N; i++) mesh_tri(&m, 0, 1 + i, 1 + (i + 1) % N);
        draw_additive(&m);
        check_coverage_once("ground fan through the near plane", 0);
        /* the rim is 300 units out, 8 below the eye: it ends about 0.042 NDC under the horizon */
        const int yStart = g_h / 2 + (int)(0.06f * g_h);
        long covered = 0;
        for (int y = yStart; y < g_h; y++) for (int x = 0; x < g_w; x++) covered += (pixel(x, y) & 0xFF) == 1;
        CHECK(covered == (long)(g_h - yStart) * g_w, "ground fan: the frame below the rim is not fully covered (%ld px)", covered);
        h ^= frame_hash() * 7u;
        write_tga("fan_nearplane", 0);
        mesh_free(&m);
    }
    /* saturating add */
    {
        Mesh m; mesh_init(&m, 4, 6);
        V4 q[4] = { { -1, -1, 0.5f, 1 }, { 1, -1, 0.5f, 1 }, { 1, 1, 0.5f, 1 }, { -1, 1, 0.5f, 1 } };
        for (int i = 0; i < 4; i++) mesh_vert(&m, q[i]);
        mesh_tri(&m, 0, 1, 2); mesh_tri(&m, 0, 2, 3);
        sw_clear_framebuffer(0xFF000000u);
        SwRect r = full_rect();
        sw_begin_view(&r);
        sw_begin_segment(SW_SEG_COLOR, NULL);
        SwDraw d = make_draw(&m, SW_OP_COLOR);
        d.color = 0x00603090u; d.dstBlend = SW_BF_ONE;
        for (int i = 0; i < 3; i++) sw_draw(&d);
        sw_end_view();
        CHECK(pixel(g_w / 2, g_h / 2) == 0xFFFF90FFu, "saturating add: %08x", pixel(g_w / 2, g_h / 2));
        CHECK(pixel(g_w - 1, g_h - 1) == 0xFFFF90FFu && pixel(0, 0) == 0xFFFF90FFu, "saturating add: frame corners");
        mesh_free(&m);
    }
    return h;
}

/* ---- test 3: closed volumes and the two stencil counts -------------------- */
static void wall(Mesh *m, float ez)
{
    mesh_init(m, 4, 6);
    const float s = 4.0f * -ez;
    mesh_vert(m, project_eye(-s, -s, ez, 1)); mesh_vert(m, project_eye(s, -s, ez, 1));
    mesh_vert(m, project_eye(s, s, ez, 1)); mesh_vert(m, project_eye(-s, s, ez, 1));
    mesh_tri(m, 0, 1, 2); mesh_tri(m, 0, 2, 3);
}

/* a closed hexahedron from 8 eye-space corners (w per corner): its outside is DOOM's front */
static void hexa(Mesh *m, const float p[8][4])
{
    mesh_init(m, 8, 36);
    for (int i = 0; i < 8; i++) mesh_vert(m, project_eye(p[i][0], p[i][1], p[i][2], p[i][3]));
    /* corners: bit0 = +x, bit1 = +y, bit2 = far. Wound so that from OUTSIDE a face is clockwise
       in GL's y-up window space. */
    static const int f[6][4] = { {0,2,3,1}, {4,5,7,6}, {0,1,5,4}, {2,6,7,3}, {0,4,6,2}, {1,3,7,5} };
    for (int i = 0; i < 6; i++) { mesh_tri(m, f[i][0], f[i][1], f[i][2]); mesh_tri(m, f[i][0], f[i][2], f[i][3]); }
}

static void run_volume(const Mesh *wallMesh, const Mesh *vol, int op)
{
    sw_clear_framebuffer(0xFF000000u);
    SwRect r = full_rect();
    sw_begin_view(&r);
    sw_begin_segment(SW_SEG_DEPTH, NULL);
    SwDraw d = make_draw(wallMesh, SW_OP_DEPTH_FILL);
    sw_draw(&d);
    sw_begin_segment(SW_SEG_SHADOW, &r);
    SwDraw s = make_draw(vol, op);
    s.offsetUnits = 1.0f;                                   /* RB_StencilShadowPass: (0, +1) by default */
    sw_draw(&s);
    sw_end_view();
}

static uint64_t test_volumes(void)
{
    uint64_t h = 0;
    Mesh w; wall(&w, -100.0f);
    const size_t n = (size_t)g_w * g_h;
    uint8_t *ref = (uint8_t *)malloc(n);

    /* a rotated box straddling the wall, one entirely in front of it, one entirely behind */
    for (int variant = 0; variant < 3; variant++) {
        const float zc = variant == 0 ? -100.0f : (variant == 1 ? -50.0f : -160.0f);
        float p[8][4];
        for (int i = 0; i < 8; i++) {
            const float x = (i & 1) ? 30.0f : -30.0f, y = (i & 2) ? 22.0f : -22.0f, z = (i & 4) ? -25.0f : 25.0f;
            const float ca = cosf(0.5f), sa = sinf(0.5f), cb = cosf(0.3f), sb = sinf(0.3f);
            const float x1 = x * ca - z * sa, z1 = x * sa + z * ca;
            const float y2 = y * cb - z1 * sb, z2 = y * sb + z1 * cb;
            p[i][0] = x1 + 7.0f; p[i][1] = y2 - 3.0f; p[i][2] = zc + z2; p[i][3] = 1;
        }
        Mesh box; hexa(&box, p);
        run_volume(&w, &box, SW_OP_STENCIL_ZPASS);
        memcpy(ref, g_stencil, n);
        long shadowed = 0, bad = 0;
        for (size_t i = 0; i < n; i++) { shadowed += ref[i] == 129; bad += ref[i] != 128 && ref[i] != 129; }
        CHECK(bad == 0, "box %d, depth pass: %ld pixels with a count other than 128 or 129", variant, bad);
        if (variant == 0) CHECK(shadowed > 1000, "box 0 straddles the wall but shadows %ld pixels", shadowed);
        else CHECK(shadowed == 0, "box %d does not touch the wall but shadows %ld pixels", variant, shadowed);
        h ^= frame_hash() * (uint64_t)(11 + variant);
        if (variant == 0) write_tga("box_zpass", 1);

        run_volume(&w, &box, SW_OP_STENCIL_ZFAIL);
        long diff = 0;
        for (size_t i = 0; i < n; i++) diff += ref[i] != g_stencil[i];
        CHECK(diff == 0, "box %d: depth fail differs from depth pass on %ld pixels", variant, diff);
        h ^= frame_hash() * (uint64_t)(17 + variant);
        mesh_free(&box);
    }

    /* the eye INSIDE a volume that starts behind it and ends behind the wall: the volume is cut
       by the near plane, which is the case the depth-fail count exists for. Expected: the wall
       is shadowed exactly where it lies inside the box. */
    {
        float p[8][4];
        for (int i = 0; i < 8; i++) {
            p[i][0] = (i & 1) ? 30.0f : -30.0f; p[i][1] = (i & 2) ? 22.0f : -22.0f;
            p[i][2] = (i & 4) ? -120.0f : 50.0f; p[i][3] = 1;
        }
        Mesh box; hexa(&box, p);
        run_volume(&w, &box, SW_OP_STENCIL_ZFAIL);
        write_tga("box_eye_inside_zfail", 1);
        const float tx = tanf(45.0f * 3.14159265f / 180.0f), ty = tx * (float)g_h / (float)g_w;
        long wrong = 0, shadowed = 0;
        for (int y = 0; y < g_h; y++)
            for (int x = 0; x < g_w; x++) {
                const float wx = (((float)x + 0.5f) / (float)g_w * 2 - 1) * tx * 100.0f;
                const float wy = (1 - ((float)y + 0.5f) / (float)g_h * 2) * ty * 100.0f;
                const float mx = fabsf(fabsf(wx) - 30.0f), my = fabsf(fabsf(wy) - 22.0f);
                if (mx < 0.2f || my < 0.2f) continue;       /* within a pixel or so of the boundary */
                const int inside = fabsf(wx) < 30.0f && fabsf(wy) < 22.0f;
                shadowed += inside;
                if (g_stencil[(size_t)y * g_w + x] != 128 + inside) wrong++;
            }
        CHECK(wrong == 0, "eye inside the box, depth fail: %ld pixels disagree with the analytic shadow", wrong);
        CHECK(shadowed > 1000, "eye inside the box: only %ld shadowed pixels expected", shadowed);
        h ^= frame_hash() * 31u;
        mesh_free(&box);
    }

    /* an occluder quad extruded to infinity away from a light: four corners with w = 1, four
       with w = 0. It leaves the screen on the right and its far end is unbounded on screen. */
    {
        const float L[3] = { -60.0f, 40.0f, -20.0f };
        const float q[4][3] = { { 0, -10, -60 }, { 40, -10, -60 }, { 40, 25, -55 }, { 0, 25, -55 } };
        float p[8][4];
        /* corner bits: bit0 = +x, bit1 = +y, bit2 = far (the extruded copy) */
        static const int corner[4] = { 0, 1, 3, 2 };
        for (int i = 0; i < 4; i++) {
            const int c = corner[i];
            p[c][0] = q[i][0]; p[c][1] = q[i][1]; p[c][2] = q[i][2]; p[c][3] = 1;
            p[c + 4][0] = q[i][0] - L[0]; p[c + 4][1] = q[i][1] - L[1]; p[c + 4][2] = q[i][2] - L[2]; p[c + 4][3] = 0;
        }
        Mesh vol; hexa(&vol, p);
        run_volume(&w, &vol, SW_OP_STENCIL_ZFAIL);
        memcpy(ref, g_stencil, n);
        write_tga("infinite_volume_zfail", 1);
        SwStats s; sw_get_stats(&s);
        CHECK(s.trisDropped == 0, "infinite volume: %d triangles dropped", s.trisDropped);

        /* analytic expectation: the wall point under a pixel is shadowed iff the segment from
           the light to it crosses the occluder quad (two triangles, Moeller-Trumbore) */
        const float fovx = 90.0f * 3.14159265f / 180.0f;
        const float tx = tanf(fovx * 0.5f), ty = tx * (float)g_h / (float)g_w;
        long wrong = 0, shadowed = 0;
        for (int y = 0; y < g_h; y += 3)
            for (int x = 0; x < g_w; x += 3) {
                int votes[3] = { 0, 0, 0 };                 /* centre and two offsets: skip silhouette pixels */
                for (int k = 0; k < 3; k++) {
                    const float fx = ((float)x + 0.5f + (k == 1 ? 1.5f : (k == 2 ? -1.5f : 0))) / (float)g_w * 2 - 1;
                    const float fy = 1 - ((float)y + 0.5f + (k == 1 ? 1.5f : (k == 2 ? -1.5f : 0))) / (float)g_h * 2;
                    const float W[3] = { fx * tx * 100.0f, fy * ty * 100.0f, -100.0f };
                    const float dir[3] = { W[0] - L[0], W[1] - L[1], W[2] - L[2] };
                    for (int t = 0; t < 2 && !votes[k]; t++) {
                        const float *a = q[0], *b = q[t + 1], *c = q[t + 2];
                        const float e1[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, e2[3] = { c[0] - a[0], c[1] - a[1], c[2] - a[2] };
                        const float pv[3] = { dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2], dir[0] * e2[1] - dir[1] * e2[0] };
                        const float det = e1[0] * pv[0] + e1[1] * pv[1] + e1[2] * pv[2];
                        if (fabsf(det) < 1e-9f) continue;
                        const float tv[3] = { L[0] - a[0], L[1] - a[1], L[2] - a[2] };
                        const float u = (tv[0] * pv[0] + tv[1] * pv[1] + tv[2] * pv[2]) / det;
                        const float qv[3] = { tv[1] * e1[2] - tv[2] * e1[1], tv[2] * e1[0] - tv[0] * e1[2], tv[0] * e1[1] - tv[1] * e1[0] };
                        const float v = (dir[0] * qv[0] + dir[1] * qv[1] + dir[2] * qv[2]) / det;
                        const float tt = (e2[0] * qv[0] + e2[1] * qv[1] + e2[2] * qv[2]) / det;
                        if (u >= 0 && v >= 0 && u + v <= 1 && tt > 0 && tt < 1) votes[k] = 1;
                    }
                }
                if (votes[0] != votes[1] || votes[0] != votes[2]) continue;
                const int got = ref[(size_t)y * g_w + x];
                shadowed += votes[0];
                if (got != 128 + votes[0]) wrong++;
            }
        CHECK(wrong == 0, "infinite volume, depth fail: %ld sampled pixels disagree with the analytic shadow", wrong);
        CHECK(shadowed > 500, "infinite volume: the analytic shadow covers only %ld samples", shadowed);
        h ^= frame_hash() * 23u;

        /* the view is outside this volume, so the depth-pass count must give the same image */
        run_volume(&w, &vol, SW_OP_STENCIL_ZPASS);
        long diff = 0;
        for (size_t i = 0; i < n; i++) diff += ref[i] != g_stencil[i];
        CHECK(diff == 0, "infinite volume: depth pass differs from depth fail on %ld pixels", diff);
        h ^= frame_hash() * 29u;
        mesh_free(&vol);
    }
    free(ref);
    mesh_free(&w);
    return h;
}

/* ---- test 4: depth EQUAL succeeds on every pixel (I2), with differing scissors (I3) ---- */
static uint64_t test_depth_equal(void)
{
    const int G = 96;
    Mesh m; mesh_init(&m, (G + 1) * (G + 1), G * G * 6);
    for (int j = 0; j <= G; j++)
        for (int i = 0; i <= G; i++) {
            /* a bumpy sheet facing the eye, larger than the frame so border triangles leave the
               viewport and the plane origin clamps; no part of it hides another */
            const float x = ((float)i / G - 0.5f) * 260.0f, y = ((float)j / G - 0.5f) * 170.0f;
            const float z = -80.0f + 6.0f * sinf(x * 0.11f) * cosf(y * 0.13f) + 0.05f * x;
            mesh_vert(&m, project_eye(x, y, z, 1));
        }
    for (int j = 0; j < G; j++)
        for (int i = 0; i < G; i++) {
            const int a = j * (G + 1) + i, b = a + 1, c = a + G + 1, e = c + 1;
            mesh_tri(&m, a, b, e); mesh_tri(&m, a, e, c);
        }
    sw_clear_framebuffer(0xFF000000u);
    sw_debug_count_equal_failures(1);
    SwRect r = full_rect();
    sw_begin_view(&r);
    sw_begin_segment(SW_SEG_DEPTH, NULL);
    SwDraw d = make_draw(&m, SW_OP_DEPTH_FILL);
    d.color = 0xFF000000u;
    sw_draw(&d);
    sw_begin_segment(SW_SEG_LIGHT, NULL);
    /* the same triangles again, as a light would send them: depth EQUAL, additive, and cut into
       scissor rectangles that do not line up with tiles, blocks or each other */
    const int xs[4] = { 0, g_w / 3 + 5, (2 * g_w) / 3 - 3, g_w }, ys[3] = { 0, g_h / 2 + 7, g_h };
    for (int sy = 0; sy < 2; sy++)
        for (int sx = 0; sx < 3; sx++) {
            SwDraw l = make_draw(&m, SW_OP_COLOR);
            l.depthTest = SW_DEPTH_EQUAL; l.dstBlend = SW_BF_ONE; l.color = 0x00010101u;
            l.scissor.x0 = xs[sx]; l.scissor.x1 = xs[sx + 1] - 1; l.scissor.y0 = ys[sy]; l.scissor.y1 = ys[sy + 1] - 1;
            sw_draw(&l);
        }
    sw_end_view();
    sw_debug_count_equal_failures(0);
    SwStats s; sw_get_stats(&s);
    CHECK(s.equalFailures == 0, "depth EQUAL failed on %llu covered pixels", (unsigned long long)s.equalFailures);
    CHECK(s.pxColor == s.pxDepth, "light pass shaded %llu pixels, depth fill wrote %llu", (unsigned long long)s.pxColor, (unsigned long long)s.pxDepth);
    long wrong = 0;
    for (int y = 0; y < g_h; y++) for (int x = 0; x < g_w; x++) wrong += (pixel(x, y) & 0xFF) != 1;
    CHECK(wrong == 0, "%ld pixels were not lit exactly once", wrong);
    const uint64_t h = frame_hash();
    mesh_free(&m);
    return h;
}

#include "sw_test_kernels.h"
#include "sw_test_bench.h"
#include "sw_test_xform.h"
#include "sw_test_sampler2.h"

typedef uint64_t (*TestFn)(void);
static const struct { const char *name; TestFn fn; } TESTS[] = {
    { "winding", test_winding }, { "watertight", test_watertight },
    { "volumes", test_volumes }, { "depth-equal", test_depth_equal },
    { "sampler", test_sampler }, { "blend", test_blend },
    { "perspective", test_perspective }, { "interaction", test_interaction },
    { "cube", test_cube }, { "xform", test_xform }, { "sampler2", test_sampler2 },
};
#define NTESTS ((int)(sizeof(TESTS) / sizeof(TESTS[0])))

static void run_tests(int threads, uint64_t *hashes, int verbose)
{
    sw_shutdown();
    if (!sw_init(threads)) { printf("this CPU lacks AVX-512 F/BW/DQ/VL\n"); exit(2); }
    sw_resize(g_w, g_h);
    sw_debug_capture(g_depth, g_stencil);
    for (int i = 0; i < NTESTS; i++) {
        if (verbose) printf("  %-12s (%d threads)\n", TESTS[i].name, sw_num_threads());
        hashes[i] = TESTS[i].fn();
    }
}

/* ---- micro-bench: stencil fill --------------------------------------------
   Stage 0 measured up to 26 shadow volume faces per pixel on average over demo1 (54 Mpx per
   frame at 1080p) and 91 at worst. This prices it: a wall at mid depth and K large quads, half
   in front of the wall (the count changes) and half behind it (the z-range reject's case). */
static void bench_stencil(int quads, int reps, int thin)
{
    Mesh w; wall(&w, -100.0f);
    Mesh q; mesh_init(&q, quads * 4, quads * 6);
    unsigned seed = 12345;
    for (int i = 0; i < quads; i++) {
        float rnd[6];
        for (int k = 0; k < 6; k++) { seed = seed * 1664525u + 1013904223u; rnd[k] = (float)(seed >> 8) / 16777216.0f; }
        const float ez = (i & 1) ? -40.0f - 40.0f * rnd[0] : -130.0f - 60.0f * rnd[0];
        const float ang = rnd[4] * 6.28f, tilt = (rnd[5] - 0.5f) * 0.3f;
        const int base = q.nv;
        if (!thin) {
            const float half = -ez * (0.55f + 0.35f * rnd[1]);  /* covers roughly a third to all of the frame */
            const float cx = -ez * (rnd[2] - 0.5f), cy = -ez * (rnd[3] - 0.5f) * 0.6f;
            for (int k = 0; k < 4; k++) {
                const float a = ang + (float)k * 1.5707963f;
                mesh_vert(&q, project_eye(cx + half * cosf(a), cy + half * sinf(a), ez * (1.0f + tilt * cosf(a)), 1));
            }
        } else {
            /* a strip like a silhouette quad: 140 - 420 x 20 pixels, any direction, anywhere */
            const float px = -ez * 2.0f / 1920.0f;             /* one 1080p pixel in eye units at this depth: the strips keep
                                                                  their share of the frame at any resolution, as real volumes do */
            const float len = px * (140.0f + 280.0f * rnd[1]) * 0.5f, wid = px * 10.0f;
            const float cx = -ez * (rnd[2] - 0.5f) * 2.0f, cy = -ez * (rnd[3] - 0.5f) * 1.1f;
            const float dx = cosf(ang), dy = sinf(ang);
            const float ox[4] = { -len, len, len, -len }, oy[4] = { -wid, -wid, wid, wid };
            for (int k = 0; k < 4; k++)
                mesh_vert(&q, project_eye(cx + ox[k] * dx - oy[k] * dy, cy + ox[k] * dy + oy[k] * dx, ez * (1.0f + tilt * 0.1f * ox[k] / len), 1));
        }
        mesh_tri(&q, base, base + 1, base + 2); mesh_tri(&q, base, base + 2, base + 3);
    }
    static const struct { const char *name; int hier, zrange, fast; } arms[] = {
        { "all on", 1, 1, 1 }, { "no cell fast path", 1, 1, 0 }, { "no z-range rejects", 1, 0, 0 }, { "no hierarchy", 0, 0, 0 },
    };
    printf("  stencil fill, %d %s quads = %d triangles, %dx%d, %d threads, op = depth pass:\n",
           quads, thin ? "thin" : "huge", quads * 2, g_w, g_h, sw_num_threads());
    for (int a = 0; a < 4; a++) {
        sw_set_option(SW_OPT_HIER, arms[a].hier); sw_set_option(SW_OPT_ZRANGE, arms[a].zrange); sw_set_option(SW_OPT_CELL_FAST, arms[a].fast);
        double best = 1e9, sum = 0, baseBest = 1e9, setupSum = 0, sortSum = 0;
        SwStats s = { 0 };
        for (int r = 0; r < reps + 3; r++) {
            SwRect rect = full_rect();
            for (int withQuads = 0; withQuads < 2; withQuads++) {
                sw_begin_view(&rect);
                sw_begin_segment(SW_SEG_DEPTH, NULL);
                SwDraw d = make_draw(&w, SW_OP_DEPTH_FILL);
                sw_draw(&d);
                if (withQuads) {
                    sw_begin_segment(SW_SEG_SHADOW, &rect);
                    SwDraw sd = make_draw(&q, SW_OP_STENCIL_ZPASS);
                    sd.offsetUnits = 1.0f;
                    sw_draw(&sd);
                }
                sw_end_view();
                SwStats cur; sw_get_stats(&cur);
                if (r < 3) continue;                        /* warm-up */
                if (withQuads) { s = cur; sum += cur.tilesMs; setupSum += cur.setupMs; sortSum += cur.sortMs; if (cur.tilesMs < best) best = cur.tilesMs; }
                else if (cur.tilesMs < baseBest) baseBest = cur.tilesMs;
            }
        }
        const double mpxWritten = (double)s.pxStencil * 1e-6;
        const double ms = sum / reps - baseBest;
        printf("    %-19s setup %5.2f ms  sort %5.3f ms  tiles %6.2f ms (min %6.2f, wall-only %5.2f) = +%5.2f ms  writes %5.1f Mpx  blocks %8llu  cells fast %6llu rej %6llu  tri-tile rej %6llu\n",
               arms[a].name, setupSum / reps, sortSum / reps, sum / reps, best, baseBest, ms, mpxWritten, (unsigned long long)s.blocksVisited,
               (unsigned long long)s.cellsFast, (unsigned long long)s.cellsRejected, (unsigned long long)s.triTileRejected);
    }
    sw_set_option(SW_OPT_HIER, 1); sw_set_option(SW_OPT_ZRANGE, 1); sw_set_option(SW_OPT_CELL_FAST, 1);
    mesh_free(&w); mesh_free(&q);
}

int main(int argc, char **argv)
{
    int doTests = 1, doBench = 1, doStencil = 1, threads = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--notests")) doTests = 0;
        else if (!strcmp(argv[i], "--nobench")) doBench = 0;
        else if (!strcmp(argv[i], "--nostencil")) doStencil = 0;
        else if (!strcmp(argv[i], "--oracles")) g_oracles = 1;
        else if (!strcmp(argv[i], "--res") && i + 2 < argc) { g_w = atoi(argv[i + 1]); g_h = atoi(argv[i + 2]); i += 2; }
        else if (!strcmp(argv[i], "--threads") && i + 1 < argc) threads = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_outDir = argv[++i];
    }
    g_depth = (float *)malloc((size_t)g_w * g_h * sizeof(float));
    g_stencil = (uint8_t *)malloc((size_t)g_w * g_h);

    if (doTests) {
        uint64_t hN[NTESTS], h1[NTESTS], hAgain[NTESTS];
        printf("tests at %dx%d\n", g_w, g_h);
        run_tests(threads, hN, 1);
        run_tests(1, h1, 0);
        run_tests(threads, hAgain, 0);
        for (int i = 0; i < NTESTS; i++) {
            printf("  %-12s hash %016llx\n", TESTS[i].name, (unsigned long long)hN[i]);
            CHECK(hN[i] == h1[i], "%s: the hash depends on the thread count (I1): %016llx with 1 thread", TESTS[i].name, (unsigned long long)h1[i]);
            CHECK(hN[i] == hAgain[i], "%s: the hash differs between two runs", TESTS[i].name);
        }
        printf(g_fail ? "%d CHECKS FAILED\n" : "all checks passed\n", g_fail);
    }
    if (doBench) {
        sw_shutdown();
        sw_init(threads);
        sw_resize(g_w, g_h);
        sw_debug_capture(NULL, NULL);
        sw_clear_framebuffer(0xFF000000u);
        printf("benches\n");
        bench_kernels(20);
        if (!doStencil) { sw_shutdown(); return g_fail ? 1 : 0; }
        bench_stencil(60, 30, 0);
        bench_stencil(9600, 30, 1);                         /* demo1's mean: 19.2k shadow triangles a frame */
        bench_stencil(22600, 30, 1);                        /* demo1's worst frame: 45.3k */
        sw_shutdown(); sw_init(1); sw_resize(g_w, g_h);
        bench_stencil(60, 5, 0);
        bench_stencil(9600, 5, 1);
    }
    sw_shutdown();
    return g_fail ? 1 : 0;
}
