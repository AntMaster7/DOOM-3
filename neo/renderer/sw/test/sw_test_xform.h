/* sw_test_xform.h -- sw_transform against a scalar reference in double, both kinds, a vertex count
   that is no multiple of 16 and a 60-byte stride like idDrawVert's; then a sheet drawn through it. */

static void xf_reference(const float *m, const double p[4], double out[4])
{
    const double cx = m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12] * p[3];
    const double cy = m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13] * p[3];
    const double cz = m[2] * p[0] + m[6] * p[1] + m[10] * p[2] + m[14] * p[3];
    const double cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15] * p[3];
    out[0] = cx; out[1] = cy; out[2] = (cz + cw) * 0.5; out[3] = cw;
}

static uint64_t test_xform(void)
{
    /* OpenGL projection with an infinite far plane (R_SetupProjection's shape), fov 90 */
    const float xmax = ZNEAR, ymax = xmax * (float)g_h / (float)g_w;
    float mvp[16] = { 0 };
    mvp[0] = ZNEAR / xmax; mvp[5] = ZNEAR / ymax; mvp[10] = -0.999f; mvp[11] = -1.0f; mvp[14] = -2.0f * ZNEAR;

    enum { GX = 37, GY = 23, NV = GX * GY };
    TVert *tv = (TVert *)calloc(NV, sizeof(TVert));
    Mesh m; mesh_init(&m, NV, (GX - 1) * (GY - 1) * 6);
    for (int y = 0; y < GY; y++)
        for (int x = 0; x < GX; x++) {
            TVert *v = &tv[y * GX + x];
            v->xyz[0] = ((float)x / (GX - 1) - 0.5f) * 300.0f;
            v->xyz[1] = ((float)y / (GY - 1) - 0.5f) * 170.0f;
            v->xyz[2] = -100.0f - 20.0f * sinf((float)x * 0.7f) * cosf((float)y * 0.9f);
            V4 dummy = { 0, 0, 0, 1 };
            mesh_vert(&m, dummy);
        }
    for (int y = 0; y < GY - 1; y++)
        for (int x = 0; x < GX - 1; x++) {
            const int a = y * GX + x, b = a + 1, c = a + GX, d = c + 1;
            mesh_tri(&m, a, b, d); mesh_tri(&m, a, d, c);
        }

    /* shadow-style vertices: every position twice, w = 1 and w = 0 */
    float *sv = (float *)malloc((size_t)NV * 2 * 4 * sizeof(float));
    for (int i = 0; i < NV; i++)
        for (int k = 0; k < 2; k++) {
            float *o = sv + ((size_t)i * 2 + k) * 4;
            o[0] = tv[i].xyz[0]; o[1] = tv[i].xyz[1]; o[2] = tv[i].xyz[2]; o[3] = k ? 0.0f : 1.0f;
        }
    const float light[3] = { 10.0f, 20.0f, -30.0f };

    sw_clear_framebuffer(0xFF000000u);
    SwRect r = full_rect();
    sw_begin_view(&r);
    SwXform xf; memset(&xf, 0, sizeof(xf));
    xf.positions = tv[0].xyz; xf.stride = sizeof(TVert); xf.numVerts = NV; xf.kind = SW_XF_POINTS;
    memcpy(xf.mvp, mvp, sizeof(mvp));
    const float *clip = sw_transform(&xf);
    SwXform xs = xf;
    xs.positions = sv; xs.stride = 16; xs.numVerts = NV * 2; xs.kind = SW_XF_SHADOW;
    memcpy(xs.lightOrigin, light, sizeof(light));
    const float *clipShadow = sw_transform(&xs);
    CHECK(clip != NULL && clipShadow != NULL, "xform: the view arena returned NULL");
    CHECK(((uintptr_t)clip & 63) == 0, "xform: result is not 64-byte aligned");

    sw_begin_segment(SW_SEG_DEPTH, NULL);
    SwDraw d = make_draw(&m, SW_OP_DEPTH_FILL);
    d.clip = clip; d.clipStride = 16; d.color = 0xFF2040C0u;
    sw_draw(&d);
    sw_end_view();

    double worst = 0.0;
    for (int i = 0; i < NV; i++) {
        const double p[4] = { tv[i].xyz[0], tv[i].xyz[1], tv[i].xyz[2], 1.0 };
        double ref[4];
        xf_reference(mvp, p, ref);
        for (int k = 0; k < 4; k++) {
            const double e = fabs(ref[k] - (double)clip[i * 4 + k]) / (fabs(ref[k]) + 1.0);
            if (e > worst) worst = e;
        }
        /* the w = 1 copy is the same point, bit for bit; the w = 0 copy is the direction from the light */
        CHECK(memcmp(clip + i * 4, clipShadow + (size_t)i * 8, 16) == 0, "xform: shadow w = 1 vertex %d differs from the point", i);
        const double q[4] = { p[0] - light[0], p[1] - light[1], p[2] - light[2], 0.0 };
        xf_reference(mvp, q, ref);
        for (int k = 0; k < 4; k++) {
            const double e = fabs(ref[k] - (double)clipShadow[(size_t)i * 8 + 4 + k]) / (fabs(ref[k]) + 1.0);
            if (e > worst) worst = e;
        }
    }
    CHECK(worst < 2e-6, "xform: worst relative error %.3g", worst);

    SwStats s; sw_get_stats(&s);
    CHECK(s.trisDropped == 0 && s.pxDepth > (uint64_t)g_w * g_h / 2, "xform: the sheet covers %llu px", (unsigned long long)s.pxDepth);
    const uint64_t h = frame_hash();
    free(tv); free(sv); mesh_free(&m);
    return h;
}
