/* test/sw_test_bench.h -- kernel micro-benches: what one light pass and one textured stage pass
   cost per megapixel. Included by sw_test.c after sw_test_kernels.h.

   The scene is a floor in perspective filling the frame (so mip levels vary across it, as in a
   corridor), textured with 512x512 maps with full mip chains. Depth fill, then L light passes
   depth-EQUAL and additive, exactly the state DOOM 3 draws interactions in. */

static void floor_quad(TQuad *q, float uvScale)
{
    /* a floor from just in front of the eye to far away, wider than the frustum */
    const float P[4][3] = { { -400, -30, -20 }, { 400, -30, -20 }, { 4000, -30, -2000 }, { -4000, -30, -2000 } };
    const float UV[4][2] = { { 0, 0 }, { 8, 0 }, { 8, 40 }, { 0, 40 } };
    mesh_init(&q->m, 4, 6);
    const float n[3] = { 0, 1, 0 }, t0[3] = { 1, 0, 0 }, t1[3] = { 0, 0, -1 };
    memcpy(q->n, n, sizeof(n)); memcpy(q->t0, t0, sizeof(t0)); memcpy(q->t1, t1, sizeof(t1));
    for (int i = 0; i < 4; i++) {
        mesh_vert(&q->m, project_eye(P[i][0], P[i][1], P[i][2], 1));
        TVert *v = &q->tv[i];
        memset(v, 0, sizeof(*v));
        memcpy(v->xyz, P[i], sizeof(float) * 3);
        v->st[0] = UV[i][0] * uvScale; v->st[1] = UV[i][1] * uvScale;
        memcpy(v->normal, n, sizeof(n)); memcpy(v->tan0, t0, sizeof(t0)); memcpy(v->tan1, t1, sizeof(t1));
        v->color[0] = v->color[1] = v->color[2] = v->color[3] = 255;
    }
    mesh_tri(&q->m, 0, 1, 2); mesh_tri(&q->m, 0, 2, 3);
}

/* a wall facing the eye that fills the upper part of the frame, so the whole frame is lit surface */
static void back_wall(TQuad *q)
{
    const float P[4][3] = { { -3000, -30, -900 }, { 3000, -30, -900 }, { 3000, 3000, -900 }, { -3000, 3000, -900 } };
    const float UV[4][2] = { { 0, 0 }, { 60, 0 }, { 60, 30 }, { 0, 30 } };
    mesh_init(&q->m, 4, 6);
    const float n[3] = { 0, 0, 1 }, t0[3] = { 1, 0, 0 }, t1[3] = { 0, 1, 0 };
    memcpy(q->n, n, sizeof(n)); memcpy(q->t0, t0, sizeof(t0)); memcpy(q->t1, t1, sizeof(t1));
    for (int i = 0; i < 4; i++) {
        mesh_vert(&q->m, project_eye(P[i][0], P[i][1], P[i][2], 1));
        TVert *v = &q->tv[i];
        memset(v, 0, sizeof(*v));
        memcpy(v->xyz, P[i], sizeof(float) * 3);
        v->st[0] = UV[i][0]; v->st[1] = UV[i][1];
        memcpy(v->normal, n, sizeof(n)); memcpy(v->tan0, t0, sizeof(t0)); memcpy(v->tan1, t1, sizeof(t1));
        v->color[0] = v->color[1] = v->color[2] = v->color[3] = 255;
    }
    mesh_tri(&q->m, 0, 1, 2); mesh_tri(&q->m, 0, 2, 3);
}

typedef struct BenchTex { SwImage *bump, *diff, *spec, *fall, *proj; } BenchTex;

static SwImage *bench_image(int w, int h, unsigned seed, int kind, int wrap)
{
    uint8_t *px = (uint8_t *)malloc((size_t)w * h * 4);
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        uint8_t *t = px + ((size_t)y * w + x) * 4;
        if (kind == 0) {                /* bump: x in alpha */
            const double nx = 0.35 * sin(x * 0.2), ny = 0.35 * cos(y * 0.17), nz = sqrt(1 - nx * nx - ny * ny);
            t[3] = (uint8_t)((nx * 0.5 + 0.5) * 255); t[1] = (uint8_t)((ny * 0.5 + 0.5) * 255); t[2] = (uint8_t)((nz * 0.5 + 0.5) * 255); t[0] = 0;
        } else if (kind == 1) {         /* colour noise */
            for (int c = 0; c < 4; c++) t[c] = (uint8_t)(64 + t_rand(&seed) % 192);
        } else if (kind == 2) {         /* falloff / projection: white inside a black border */
            const int border = x == 0 || y == 0 || x == w - 1 || y == h - 1;
            t[0] = t[1] = t[2] = t[3] = (uint8_t)(border ? 0 : 255);
        }
    }
    uint8_t *lv[16];
    const int n = make_mips(px, w, h, lv, 16);
    if (kind == 2) {
        /* idImage::GenerateImage: TR_CLAMP_TO_ZERO mips with preserveBorder, so the black border
           survives on every level ("don't let mip mapping smear the texture into the clamped border") */
        int lw = w, lh = h;
        for (int i = 0; i < n; i++) {
            for (int x = 0; x < lw; x++) { memset(lv[i] + (size_t)x * 4, 0, 4); memset(lv[i] + ((size_t)(lh - 1) * lw + x) * 4, 0, 4); }
            for (int y = 0; y < lh; y++) { memset(lv[i] + (size_t)y * lw * 4, 0, 4); memset(lv[i] + ((size_t)y * lw + lw - 1) * 4, 0, 4); }
            lw = lw > 1 ? lw / 2 : 1; lh = lh > 1 ? lh / 2 : 1;
        }
    }
    SwImage *im = sw_image_create(w, h, n, (const uint8_t *const *)lv, wrap);
    for (int i = 1; i < n; i++) free(lv[i]);
    free(px);
    return im;
}

/* one light's parameters over a surface with tangent frame (t0, t1, n): the projection covers a
   square of side `size` around `centre` on the surface plane */
static void bench_light(SwInteractionParms *p, const TQuad *q, const BenchTex *tx, const float centre[3], float size, float height, int specular)
{
    memset(p, 0, sizeof(*p));
    for (int i = 0; i < 3; i++) {
        p->localLightOrigin[i] = centre[i] + q->n[i] * height;
        p->lightProjectS[i] = q->t0[i] / size; p->lightProjectT[i] = q->t1[i] / size;
        p->lightFalloffS[i] = q->n[i] / (2.0f * height);      /* along the normal: 0.5 at the surface */
    }
    p->lightProjectQ[3] = 1.0f;
    p->lightProjectS[3] = 0.5f - (p->lightProjectS[0] * centre[0] + p->lightProjectS[1] * centre[1] + p->lightProjectS[2] * centre[2]);
    p->lightProjectT[3] = 0.5f - (p->lightProjectT[0] * centre[0] + p->lightProjectT[1] * centre[1] + p->lightProjectT[2] * centre[2]);
    p->lightFalloffS[3] = 0.5f - (p->lightFalloffS[0] * centre[0] + p->lightFalloffS[1] * centre[1] + p->lightFalloffS[2] * centre[2]);
    for (int r = 0; r < 2; r++) { p->bumpMatrix[r][r] = 1; p->diffuseMatrix[r][r] = 1; p->specularMatrix[r][r] = 1; }
    p->vertexColorModulate = 0; p->vertexColorAdd = 1;
    p->diffuseColor[0] = p->diffuseColor[1] = p->diffuseColor[2] = p->diffuseColor[3] = 0.25f;
    if (specular) p->specularColor[0] = p->specularColor[1] = p->specularColor[2] = p->specularColor[3] = 0.3f;
    p->bump = tx->bump; p->diffuse = tx->diff; p->specular = tx->spec; p->falloff = tx->fall; p->projection = tx->proj;
}

static void bench_interaction(const char *name, int lights, float lightSize, int specular, int reps)
{
    TQuad fl, wl; floor_quad(&fl, 1.0f); back_wall(&wl);
    BenchTex tx;
    tx.bump = bench_image(512, 512, 1, 0, SW_WRAP_REPEAT); tx.diff = bench_image(512, 512, 2, 1, SW_WRAP_REPEAT);
    tx.spec = bench_image(512, 512, 3, 1, SW_WRAP_REPEAT);
    tx.fall = bench_image(64, 16, 4, 2, SW_WRAP_CLAMP); tx.proj = bench_image(128, 128, 5, 2, SW_WRAP_CLAMP);
    SwInteractionParms *pf = (SwInteractionParms *)malloc(sizeof(SwInteractionParms) * (size_t)lights * 2);
    unsigned seed = 99;
    for (int i = 0; i < lights; i++) {
        /* lights strewn over the visible floor and wall; lightSize >= 1e5 covers everything */
        const float fx = ((float)(t_rand(&seed) % 1000) / 1000.0f - 0.5f), fz = (float)(t_rand(&seed) % 1000) / 1000.0f;
        const float cf[3] = { fx * 600.0f, -30.0f, -60.0f - fz * 800.0f }, cw[3] = { fx * 1500.0f, 200.0f + fz * 500.0f, -900.0f };
        bench_light(&pf[i * 2], &fl, &tx, cf, lightSize, 200.0f, specular);
        bench_light(&pf[i * 2 + 1], &wl, &tx, cw, lightSize * 2.0f, 300.0f, specular);
    }
    double best = 1e9, sum = 0, baseBest = 1e9, setupSum = 0;
    SwStats s = { 0 };
    for (int r = 0; r < reps + 3; r++)
        for (int withLights = 0; withLights < 2; withLights++) {
            SwRect rect = full_rect();
            sw_begin_view(&rect);
            sw_begin_segment(SW_SEG_DEPTH, NULL);
            SwDraw d0 = make_draw(&fl.m, SW_OP_DEPTH_FILL), d1 = make_draw(&wl.m, SW_OP_DEPTH_FILL);
            sw_draw(&d0); sw_draw(&d1);
            for (int i = 0; withLights && i < lights; i++) {
                sw_begin_segment(SW_SEG_LIGHT, NULL);
                for (int k = 0; k < 2; k++) {
                    const TQuad *q = k ? &wl : &fl;
                    SwDraw l = make_draw(&q->m, SW_OP_COLOR);
                    l.kernel = SW_KERN_INTERACTION; l.kernelParms = &pf[i * 2 + k]; l.verts = tvert_source(q->tv);
                    l.depthTest = SW_DEPTH_EQUAL; l.srcBlend = SW_BF_ONE; l.dstBlend = SW_BF_ONE;
                    sw_draw(&l);
                }
            }
            sw_end_view();
            SwStats cur; sw_get_stats(&cur);
            if (r < 3) continue;
            if (withLights) { s = cur; sum += cur.tilesMs; setupSum += cur.setupMs; if (cur.tilesMs < best) best = cur.tilesMs; }
            else if (cur.tilesMs < baseBest) baseBest = cur.tilesMs;
        }
    const double ms = sum / reps - baseBest, asked = (double)s.kernelLanes * 1e-6, lit = (double)s.pxColor * 1e-6;
    printf("    %-34s tiles %7.2f ms (depth-only %5.2f) = +%6.2f ms | asked %7.2f Mpx, lit %7.2f Mpx, %5.1f%% of blocks dark | %6.3f ms per asked Mpx, %6.3f per lit Mpx\n",
           name, sum / reps, baseBest, ms, asked, lit, s.kernelCalls ? 100.0 * (double)s.lightBlocksDark / (double)s.kernelCalls : 0.0,
           asked > 0 ? ms / asked : 0.0, lit > 0 ? ms / lit : 0.0);
    free(pf);
    sw_image_destroy(tx.bump); sw_image_destroy(tx.diff); sw_image_destroy(tx.spec); sw_image_destroy(tx.fall); sw_image_destroy(tx.proj);
    mesh_free(&fl.m); mesh_free(&wl.m);
}

static void bench_stage(const char *name, int passes, int srcBlend, int dstBlend, int perspective, int reps)
{
    SwImage *im = bench_image(512, 512, 7, 1, SW_WRAP_REPEAT);
    TQuad fl, wl; Mesh sq; TVert sv[4];
    floor_quad(&fl, 1.0f); back_wall(&wl);
    screen_quad(&sq, sv, 0, 0, g_w, g_h, 0, 0, (float)g_w / 512.0f, (float)g_h / 512.0f);   /* one texel per pixel, as a GUI draws */
    SwStageParms p = stage_parms(im);
    p.color[3] = 0.6f;
    double sum = 0, baseBest = 1e9;
    SwStats s = { 0 };
    for (int r = 0; r < reps + 3; r++)
        for (int on = 0; on < 2; on++) {
            SwRect rect = full_rect();
            sw_begin_view(&rect);
            sw_begin_segment(SW_SEG_COLOR, NULL);
            SwDraw base = make_draw(&sq, SW_OP_COLOR); base.color = 0xFF202020u; sw_draw(&base);
            for (int i = 0; on && i < passes; i++) {
                for (int k = 0; k < (perspective ? 2 : 1); k++) {
                    const Mesh *m = perspective ? (k ? &wl.m : &fl.m) : &sq;
                    SwDraw d = make_draw(m, SW_OP_COLOR);
                    d.kernel = SW_KERN_STAGE; d.kernelParms = &p; d.verts = tvert_source(perspective ? (k ? wl.tv : fl.tv) : sv);
                    d.srcBlend = srcBlend; d.dstBlend = dstBlend;
                    sw_draw(&d);
                }
            }
            sw_end_view();
            SwStats cur; sw_get_stats(&cur);
            if (r < 3) continue;
            if (on) { s = cur; sum += cur.tilesMs; } else if (cur.tilesMs < baseBest) baseBest = cur.tilesMs;
        }
    const double ms = sum / reps - baseBest, mpx = (double)s.kernelLanes * 1e-6;
    printf("    %-34s tiles %7.2f ms (flat-only %5.2f) = +%6.2f ms | %7.2f Mpx shaded | %6.3f ms per Mpx\n",
           name, sum / reps, baseBest, ms, mpx, mpx > 0 ? ms / mpx : 0.0);
    sw_image_destroy(im);
    mesh_free(&fl.m); mesh_free(&wl.m); mesh_free(&sq);
}

static void bench_kernels(int reps)
{
    printf("  kernels, %dx%d (%.2f Mpx), %d threads:\n", g_w, g_h, (double)g_w * g_h * 1e-6, sw_num_threads());
    bench_interaction("1 light over everything, specular", 1, 1e6f, 1, reps);
    if (g_oracles) {    /* a /DSW_ORACLES=1 build: where the time of a light pass goes, the kernel cut short after each stage */
        static const char *cutName[5] = { "", "  oracle: cut at kernel entry", "  oracle: cut after falloff", "  oracle: cut after projection", "  oracle: cut after bump + N.L" };
        for (int cut = 1; cut <= 4; cut++) {
            sw_set_option(SW_OPT_KERNEL_CUT, cut);
            bench_interaction(cutName[cut], 1, 1e6f, 1, reps);
        }
        sw_set_option(SW_OPT_KERNEL_CUT, 0);
    }
    bench_interaction("1 light over everything, diffuse", 1, 1e6f, 0, reps);
    bench_interaction("4 lights over everything, specular", 4, 1e6f, 1, reps);
    bench_interaction("8 local lights, specular", 8, 260.0f, 1, reps);
    bench_interaction("24 local lights, specular", 24, 160.0f, 1, reps);
    sw_set_option(SW_OPT_LIGHT_CELLS, 0);
    bench_interaction("  8 local, no light-cell reject", 8, 260.0f, 1, reps);
    bench_interaction("  24 local, no light-cell reject", 24, 160.0f, 1, reps);
    sw_set_option(SW_OPT_LIGHT_CELLS, 1);
    bench_stage("stage, opaque, perspective", 1, SW_BF_ONE, SW_BF_ZERO, 1, reps);
    bench_stage("stage, alpha blend x3, 2D", 3, SW_BF_SRC_ALPHA, SW_BF_ONE_MINUS_SRC_ALPHA, 0, reps);
    bench_stage("stage, additive x3, perspective", 3, SW_BF_ONE, SW_BF_ONE, 1, reps);
}
