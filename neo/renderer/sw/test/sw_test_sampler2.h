/* test/sw_test_sampler2.h -- the sampler cases the first sampler test did not have, each one a bug or a
   wrong rule that real frames found and the harness let through (CLAUDE.md, Stage 8):

   a. a repeat map only FOUR ROWS high, sampled off the texel centres. A wrapped lower row then sits
      ABOVE its upper one inside a block; the texel window chose its row count from the lower row
      alone and read the wrong rows (13 of 23 demo frames differed, the harness was green).
   b. a footprint stretched DIAGONALLY on screen. The level comes from the major axis of the footprint
      ellipse, not from the longer of the two screen-axis derivatives: the two disagree by up to
      sqrt 2, a whole level. Both sides of the "level 0 without the rule" bound (A + C < 1.99) and
      both sides of the rounding are checked.

   A hash of its own, so the first sampler test keeps its hash. */

/* bilinear with repeat addressing, in double, as GL_LINEAR defines it */
static void ref_bilinear_repeat(const uint8_t *tex, int w, int h, double u, double v, double out[4])
{
    const double x = u * w - 0.5, y = v * h - 0.5;
    const double xf = floor(x), yf = floor(y), fx = x - xf, fy = y - yf;
    const int x0 = (((int)xf % w) + w) % w, y0 = (((int)yf % h) + h) % h, x1 = (x0 + 1) % w, y1 = (y0 + 1) % h;
    for (int c = 0; c < 4; c++) {
        const double t00 = tex[(y0 * w + x0) * 4 + c], t01 = tex[(y0 * w + x1) * 4 + c];
        const double t10 = tex[(y1 * w + x0) * 4 + c], t11 = tex[(y1 * w + x1) * 4 + c];
        out[c] = (t00 * (1 - fx) + t01 * fx) * (1 - fy) + (t10 * (1 - fx) + t11 * fx) * fy;
    }
}

static uint64_t test_sampler2(void)
{
    uint64_t h = 0;
    Mesh m; TVert tv[4];

    /* a. 8 x 4 texels, tiled 12 x 12 times. Two scales: 1 texel per pixel (a block spans the whole height:
          the five-row window with rows that wrap) and 5 pixels per texel (two- and three-row windows),
          both shifted off the texel centres so that every one of the four texels carries weight */
    {
        const int TW = 8, TH = 4;
        unsigned seed = 4242;
        uint8_t tex[8 * 4 * 4];
        for (int i = 0; i < TW * TH * 4; i++) tex[i] = (uint8_t)t_rand(&seed);
        const uint8_t *lv0[1] = { tex };
        SwImage *im = sw_image_create(TW, TH, 1, lv0, SW_WRAP_REPEAT);
        for (int pass = 0; pass < 2; pass++) {
            const int scale = pass ? 5 : 1, reps = 12;
            const int pw = TW * reps * scale, ph = TH * reps * scale;          /* 96 x 48 or 480 x 240 pixels */
            const float shiftU = 0.37f / (float)TW, shiftV = 0.41f / (float)TH;
            sw_clear_framebuffer(0xFF000000u);
            /* NOT on the 4-pixel block grid: the map repeats every 4 or 20 rows, and from a multiple of 4 every wrap
               would fall on a block boundary, where no block holds both sides of it (the first version of this test
               did that, and passed with the bug put back) */
            const int qx = 67, qy = 66;
            screen_quad(&m, tv, qx, qy, qx + pw, qy + ph, -3.0f + shiftU, -3.0f + shiftV, -3.0f + shiftU + (float)reps, -3.0f + shiftV + (float)reps);
            SwStageParms p = stage_parms(im);
            draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
            long wrong = 0; int worst = 0;
            for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
                const double u = -3.0 + shiftU + ((double)x + 0.5) / pw * reps, v = -3.0 + shiftV + ((double)y + 0.5) / ph * reps;
                double ref[4];
                ref_bilinear_repeat(tex, TW, TH, u, v, ref);
                const uint32_t got = pixel(qx + x, qy + y);
                for (int c = 0; c < 4; c++) {
                    const int d = abs((int)((got >> (c * 8)) & 0xFF) - (int)(ref[c] + 0.5));
                    if (d > worst) worst = d;
                    wrong += d > 1;
                }
            }
            CHECK(wrong == 0, "4-row repeat map at %d px per texel: %ld channel values off by more than one code (worst %d)", scale, wrong, worst);
            h ^= frame_hash() * (uint64_t)(5 + pass);
            mesh_free(&m);
        }
        sw_image_destroy(im);
    }

    /* b. u = a (x + y), v = 0.1 (x - y) texels per pixel: A = C = a^2 + 0.01, B = a^2 - 0.01, so the ellipse's
          major axis is about a * sqrt 2 while either screen-axis derivative is only a. Level = round(log2(major)). */
    {
        const int S = 256;
        uint8_t *levels[16];
        int w = S, n = 0;
        for (; w >= 1; w /= 2, n++) {
            levels[n] = (uint8_t *)malloc((size_t)w * w * 4);
            for (int i = 0; i < w * w; i++) { levels[n][i * 4] = (uint8_t)(n * 20 + 10); levels[n][i * 4 + 1] = 0; levels[n][i * 4 + 2] = 0; levels[n][i * 4 + 3] = 255; }
        }
        SwImage *mip = sw_image_create(S, S, n, (const uint8_t *const *)levels, SW_WRAP_REPEAT);
        static const struct { float a; int level; } cases[4] = {
            { 0.80f, 0 },       /* major 1.13; A + C = 1.30: level 0 WITHOUT the rule */
            { 0.95f, 0 },       /* major 1.34; A + C = 1.83: still without it, close to the bound */
            { 1.05f, 1 },       /* major 1.49; A + C = 2.23: the rule runs and rounds UP (the max rule says 1.05: level 0) */
            { 1.20f, 1 },       /* major 1.70 (the max rule says 1.20: level 0) */
        };
        for (int k = 0; k < 4; k++) {
            sw_clear_framebuffer(0xFF000000u);
            screen_quad(&m, tv, 100, 100, 100 + S, 100 + S, 0, 0, 1, 1);      /* st = pixel / S, so a matrix entry IS texels per pixel */
            SwStageParms p = stage_parms(mip);
            p.texMatrix[0][0] = cases[k].a; p.texMatrix[0][1] = cases[k].a;
            p.texMatrix[1][0] = 0.1f;       p.texMatrix[1][1] = -0.1f;
            draw_stage(&m, tv, &p, SW_BF_ONE, SW_BF_ZERO);
            long wrong = 0;
            for (int y = 8; y < S - 8; y += 7) for (int x = 8; x < S - 8; x += 5)
                wrong += (pixel(100 + x, 100 + y) & 0xFF) != (uint32_t)(cases[k].level * 20 + 10);
            CHECK(wrong == 0, "diagonal footprint, %.2f texels per pixel per axis: %ld samples did not read level %d", cases[k].a, wrong, cases[k].level);
            h ^= frame_hash() * (uint64_t)(17 + k);
            mesh_free(&m);
        }
        sw_image_destroy(mip);
        for (int i = 0; i < n; i++) free(levels[i]);
    }
    return h;
}
