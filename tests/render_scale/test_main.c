/* The render-scale and present helpers (nv2a_backend_common.h), and the
 * guarantee they give at stock: every one is the identity at scale 1. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend_common.h"
#include "d3d8_hlsl.h"
#include "d3d8_msl.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

static int rect_is(const struct nv2a_rect *r, int x, int y, int w, int h)
{
    return r->x == x && r->y == y && r->w == w && r->h == h;
}

/* A fake query pool for the tracker: every query reports s_samples. */
static uint64_t s_samples;
static int s_q;
static void *q_get(void *c) { (void)c; return &s_q; }
static void q_put(void *c, void *q) { (void)c; (void)q; }
static void q_begin(void *c, void *q) { (void)c; (void)q; }
static void q_end(void *c, void *q) { (void)c; (void)q; }
static int q_result(void *c, void *q, int f, uint64_t *n) { (void)c; (void)q; (void)f; *n = s_samples; return 1; }
static uint64_t q_now(void *c) { (void)c; return 0; }

static uint32_t occ_run(unsigned scale, uint64_t samples)
{
    static const struct nv2a_occ_ops ops = { q_get, q_put, q_begin, q_end, q_result, q_now, NULL };
    static struct nv2a_occ o;
    static uint32_t mem[64];
    nv2a_occ_init(&o, &ops, (uint8_t *)mem);
    nv2a_occ_set_scale(&o, scale);
    s_samples = samples;
    nv2a_occ_zpass(&o, 1);
    nv2a_occ_zpass(&o, 0);
    nv2a_occ_report(&o, 16, 1);
    return mem[4 + 2];       /* the report's count word */
}

/* nv2a_snap_uv (D3D8_HLSL_SNAP_UV, D3D8_MSL_SNAP_UV) in C, expression for
 * expression: the reference the shader text is checked against below. */
static void snap_uv(float uv[2], const float pos[2], const float dx[2],
                    const float dy[2], float n)
{
    const float eps = 1.0f / 64.0f;
    if (n > 1.0f && fabsf(dx[1]) <= eps && fabsf(dy[0]) <= eps
            && fabsf(fabsf(dx[0]) * n - 1.0f) <= eps && fabsf(fabsf(dy[1]) * n - 1.0f) <= eps) {
        float d0 = (floorf(pos[0] / n) + 0.5f) * n - pos[0];
        float d1 = (floorf(pos[1] / n) + 0.5f) * n - pos[1];
        uv[0] += dx[0] * d0 + dy[0] * d1;
        uv[1] += dx[1] * d0 + dy[1] * d1;
    }
}

/* The texel coordinate host pixel (px, py) gets for the affine mapping
 * uv = o + dx * px + dy * py, after the snap. */
static int snap_at(float px, float py, float ou, float ov, float dxu, float dxv,
                   float dyu, float dyv, float n, float eu, float ev)
{
    float uv[2], pos[2] = {px, py}, dx[2] = {dxu, dxv}, dy[2] = {dyu, dyv};
    uv[0] = ou + dxu * px + dyu * py;
    uv[1] = ov + dxv * px + dyv * py;
    snap_uv(uv, pos, dx, dy, n);
    return fabsf(uv[0] - eu) < 1e-3f && fabsf(uv[1] - ev) < 1e-3f;
}

static struct nv2a_stage stage(int mode, uint32_t color, uint32_t filter)
{
    struct nv2a_stage t;
    memset(&t, 0, sizeof t);
    t.mode = mode;
    t.color = color;
    t.filter = filter;
    t.width = 1024;
    t.height = 512;
    return t;
}

int main(void)
{
    struct nv2a_rect r;
    uint32_t hw, hh, x, y;

    /* ---- host options: zero is stock ---- */
    CHECK(nv2a_host_opts()->render_scale == 0);
    CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_NEAREST);
    CHECK(nv2a_host_opts()->fullscreen == 0);
    CHECK(nv2a_host_render_scale() == 1);

    /* ---- host size ---- */
    CHECK(nv2a_host_size(640, 480, 0, 16384, &hw, &hh) == 1 && hw == 640 && hh == 480);
    CHECK(nv2a_host_size(640, 480, 1, 16384, &hw, &hh) == 1 && hw == 640 && hh == 480);
    CHECK(nv2a_host_size(640, 480, 2, 16384, &hw, &hh) == 2 && hw == 1280 && hh == 960);
    CHECK(nv2a_host_size(256, 256, 3, 16384, &hw, &hh) == 3 && hw == 768 && hh == 768);
    CHECK(nv2a_host_size(640, 480, 4, 16384, &hw, &hh) == 4 && hw == 2560 && hh == 1920);
    /* Clamp: a 4096 surface at 4x does not fit 8192, 2x does. */
    CHECK(nv2a_host_size(4096, 64, 4, 8192, &hw, &hh) == 2 && hw == 8192 && hh == 128);
    CHECK(nv2a_host_size(4096, 4096, 4, 4096, &hw, &hh) == 1 && hw == 4096);

    /* ---- present rectangle ---- */
    /* Exact fit is the whole output, every filter. */
    CHECK(nv2a_present_rect(640, 480, 640, 480, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 640, 480));
    CHECK(nv2a_present_rect(640, 480, 640, 480, NV2A_PRESENT_INTEGER, &r) == 1 && rect_is(&r, 0, 0, 640, 480));
    /* Fit: pillarbox in 16:9, letterbox in a tall output. */
    CHECK(nv2a_present_rect(640, 480, 1920, 1080, NV2A_PRESENT_LINEAR, &r) == 0 && rect_is(&r, 240, 0, 1440, 1080));
    CHECK(nv2a_present_rect(640, 480, 800, 1000, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 200, 800, 600));
    /* Integer: 640x480 in 1920x1080 is 2x, centred. */
    CHECK(nv2a_present_rect(640, 480, 1920, 1080, NV2A_PRESENT_INTEGER, &r) == 1 && rect_is(&r, 320, 60, 1280, 960));
    /* Integer fallback: a 2x frame in a smaller output is the fit. */
    CHECK(nv2a_present_rect(1280, 960, 1024, 768, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 0, 0, 1024, 768));
    CHECK(nv2a_present_rect(1280, 960, 1000, 700, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 33, 0, 933, 700));
    /* Odd sizes round to the nearest pixel and stay inside. */
    CHECK(nv2a_present_rect(640, 480, 1001, 333, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 278, 0, 444, 333));
    /* Empty in, empty out. */
    CHECK(nv2a_present_rect(0, 480, 640, 480, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 0, 0));
    CHECK(nv2a_present_rect(640, 480, 0, 0, NV2A_PRESENT_INTEGER, &r) == 0 && rect_is(&r, 0, 0, 0, 0));

    /* ---- upscale / downscale ---- */
    {
        enum { W = 5, H = 3 };
        uint32_t src[W * H], up[W * 3 * H * 3], back[W * H];
        unsigned n;
        for (x = 0; x < W * H; x++)
            src[x] = 0x01020304u * (x + 1) ^ 0xA5C3F00Fu;
        for (n = 1; n <= 3; n++) {
            nv2a_upscale_nearest32((const uint8_t *)src, W * 4, W, H, n, (uint8_t *)up, W * n * 4);
            /* Every n x n block is the source pixel. */
            for (y = 0; y < H * n; y++)
                for (x = 0; x < W * n; x++)
                    CHECK(up[y * W * n + x] == src[(y / n) * W + x / n]);
            /* Round trip: box of a nearest upscale is the input. */
            nv2a_downscale_box32((const uint8_t *)up, W * n * 4, W, H, n, (uint8_t *)back, W * 4);
            for (x = 0; x < W * H; x++)
                CHECK(back[x] == src[x]);
        }
    }
    {
        /* Box average per channel, rounded: 2x2 of 0, 255, 255, 255 -> 191;
         * 3x3 of eight 0 and one 10 -> 1 (10/9 = 1.1). Bytes, so BGRA order
         * is irrelevant; a pitch wider than the row is honoured. */
        uint8_t s2[2 * 12] = {0}, d2[8] = {0};
        uint8_t s3[3 * 12] = {0}, d3[4] = {0};
        int c;
        for (c = 0; c < 4; c++) {
            s2[4 + c] = 255;          /* (1,0) */
            s2[12 + c] = 255;         /* (0,1), pitch 12 */
            s2[16 + c] = 255;         /* (1,1) */
        }
        nv2a_downscale_box32(s2, 12, 1, 1, 2, d2, 8);
        for (c = 0; c < 4; c++)
            CHECK(d2[c] == 191);
        s3[12 + 4] = 10;              /* centre pixel's first byte */
        nv2a_downscale_box32(s3, 12, 1, 1, 3, d3, 4);
        CHECK(d3[0] == 1 && d3[1] == 0);
    }

    /* ---- visibility counts ---- */
    CHECK(nv2a_occ_scale_count(0, 1) == 0);
    CHECK(nv2a_occ_scale_count(12345, 1) == 12345);
    CHECK(nv2a_occ_scale_count(12345, 0) == 12345);
    CHECK(nv2a_occ_scale_count(0, 2) == 0);
    CHECK(nv2a_occ_scale_count(1, 2) == 1);          /* visible stays visible */
    CHECK(nv2a_occ_scale_count(3, 2) == 1);
    CHECK(nv2a_occ_scale_count(4, 2) == 1);
    CHECK(nv2a_occ_scale_count(6, 2) == 2);          /* 1.5 rounds up */
    CHECK(nv2a_occ_scale_count(4000, 2) == 1000);
    CHECK(nv2a_occ_scale_count(9 * 307200ull, 3) == 307200);
    CHECK(nv2a_occ_scale_count(4, 3) == 1);          /* (4+4)/9 rounds to 0 */
    for (uint64_t n = 1; n <= 8; n++)
        CHECK(nv2a_occ_scale_count(n, 3) != 0);
    for (uint64_t n = 1; n <= 15; n++)
        CHECK(nv2a_occ_scale_count(n, 4) != 0);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 1) == 0xFFFFFFFFu);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 2) == 0xFFFFFFFFu);
    CHECK(nv2a_occ_scale_count(0x400000000ull, 4) == 0x40000000u);
    /* Through the tracker: the report's word is the scaled count. */
    CHECK(occ_run(1, 4000) == 4000);
    CHECK(occ_run(2, 4000) == 1000);
    CHECK(occ_run(2, 1) == 1);
    CHECK(occ_run(2, 0) == 0);

    /* ---- rect snap: which stages (tex_scale[n].z) ---- */
    {
        enum { LIN = 0x12, SWZ = 0x06, DXT = 0x0C };      /* A8R8G8B8 linear, swizzled; DXT1 */
        const uint32_t bil = 2u << 16 | 2u << 24, pnt = 1u << 16 | 1u << 24;
        const uint32_t mag_only = 1u << 16 | 2u << 24, min_only = 2u << 16 | 1u << 24;
        struct nv2a_stage t = stage(1, LIN, bil);
        CHECK(nv2a_rect_snap_scale(&t, 2) == 2);
        CHECK(nv2a_rect_snap_scale(&t, 3) == 3);
        CHECK(nv2a_rect_snap_scale(&t, 1) == 0);          /* stock: never */
        CHECK(nv2a_rect_snap_scale(&t, 0) == 0);
        t = stage(1, LIN, mag_only);
        CHECK(nv2a_rect_snap_scale(&t, 2) == 2);
        t = stage(1, LIN, min_only);
        CHECK(nv2a_rect_snap_scale(&t, 2) == 2);
        t = stage(1, LIN, pnt);                             /* one texel, no bleed */
        CHECK(nv2a_rect_snap_scale(&t, 2) == 0);
        t = stage(1, SWZ, bil);                             /* normalised coordinates */
        CHECK(nv2a_rect_snap_scale(&t, 2) == 0);
        t = stage(1, DXT, bil);
        CHECK(nv2a_rect_snap_scale(&t, 2) == 0);
        t = stage(0, LIN, bil);                             /* stage off */
        CHECK(nv2a_rect_snap_scale(&t, 2) == 0);
        t = stage(4, LIN, bil);                             /* pass-through */
        CHECK(nv2a_rect_snap_scale(&t, 2) == 0);
        t = stage(2, LIN, bil);                             /* other modes sample as 2D */
        CHECK(nv2a_rect_snap_scale(&t, 2) == 2);
    }
    {
        /* Through the fill: .z follows the host scale; a render-target
         * stage gets 1/size and .z = 0; .w stays 0. */
        static struct nv2a_pb_vsh v;
        struct nv2a_stage st[4];
        struct nv2a_ps_consts pc;
        struct nv2a_host_opts o = { .render_scale = 2, .present_filter = NV2A_PRESENT_NEAREST };
        st[0] = stage(1, 0x12, 2u << 16 | 2u << 24);
        st[1] = stage(1, 0x06, 2u << 16 | 2u << 24);
        st[2] = stage(4, 0x12, 2u << 16 | 2u << 24);
        st[3] = stage(1, 0x12, 1u << 16 | 1u << 24);
        nv2a_ps_consts_fill(&v, st, &pc);
        CHECK(pc.tex_scale[0][2] == 0.0f && pc.tex_scale[1][2] == 0.0f);   /* scale 1 */
        nv2a_host_opts_set(&o);
        nv2a_ps_consts_fill(&v, st, &pc);
        CHECK(pc.tex_scale[0][0] == 1.0f / 1024.0f && pc.tex_scale[0][1] == 1.0f / 512.0f);
        CHECK(pc.tex_scale[0][2] == 2.0f && pc.tex_scale[0][3] == 0.0f);
        CHECK(pc.tex_scale[1][2] == 0.0f && pc.tex_scale[2][2] == 0.0f && pc.tex_scale[3][2] == 0.0f);
        nv2a_ps_consts_rt(&pc, 0, &st[0], 640, 480);
        CHECK(pc.tex_scale[0][0] == 1.0f / 640.0f && pc.tex_scale[0][1] == 1.0f / 480.0f);
        CHECK(pc.tex_scale[0][2] == 0.0f);
        nv2a_ps_consts_rt(&pc, 1, &st[1], 640, 480);   /* swizzled: normalised, kept */
        CHECK(pc.tex_scale[1][0] == 1.0f && pc.tex_scale[1][2] == 0.0f);
        o.render_scale = 0;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_render_scale() == 1);
    }

    /* ---- rect snap: the shader arithmetic ---- */
    /* The movie at scale 2: guest column 639 is host 1278.5 and 1279.5,
     * sampled at 639.25 and 639.75 before; both read 639.5, the 1x point. */
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.5f, 0, 0, 0.5f, 2, 639.5f, 300.5f));
    CHECK(snap_at(1278.5f, 600.5f, 0, 0, 0.5f, 0, 0, 0.5f, 2, 639.5f, 300.5f));
    CHECK(snap_at(1278.5f, 601.5f, 0, 0, 0.5f, 0, 0, 0.5f, 2, 639.5f, 300.5f));
    CHECK(snap_at(0.5f, 0.5f, 0, 0, 0.5f, 0, 0, 0.5f, 2, 0.5f, 0.5f));
    /* Scale 3: the three host pixels of guest column 639 (639.1667,
     * 639.5, 639.8333) all read 639.5. */
    CHECK(snap_at(1917.5f, 1.5f, 0, 0, 1.0f / 3, 0, 0, 1.0f / 3, 3, 639.5f, 0.5f));
    CHECK(snap_at(1918.5f, 1.5f, 0, 0, 1.0f / 3, 0, 0, 1.0f / 3, 3, 639.5f, 0.5f));
    CHECK(snap_at(1919.5f, 1.5f, 0, 0, 1.0f / 3, 0, 0, 1.0f / 3, 3, 639.5f, 0.5f));
    /* A flipped V (dy.y < 0) is still a unity blit. */
    CHECK(snap_at(10.5f, 959.5f, 0, 480.0f, 0.5f, 0, 0, -0.5f, 2, 5.5f, 0.5f));
    /* Within the 1/64 tolerance: a mapping 1/128 off unity still snaps. */
    CHECK(snap_at(1279.5f, 0.5f, 0, 0, 0.5f + 1.0f / 256, 0, 0, 0.5f, 2,
                  (0.5f + 1.0f / 256) * 1279.5f - (0.5f + 1.0f / 256) * 0.5f, 0.5f));
    /* Not a unity blit: a stretch (dx.x * N = 0.933) or a cross term of
     * 1/32 keeps the host-rate sample. */
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.933f / 2, 0, 0, 0.5f, 2, 0.933f / 2 * 1279.5f, 300.25f));
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.5f, 1.0f / 32, 0, 0.5f, 2, 639.75f, 300.25f + 1279.5f / 32));
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.5f, 0, 1.0f / 32, 0.5f, 2, 639.75f + 600.5f / 32, 300.25f));
    /* Magnified 2x at scale 2 (one texel per host pixel): not snapped. */
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.25f, 0, 0, 0.25f, 2, 319.875f, 150.125f));
    /* .z = 0 (stock, or not eligible) and .z = 1: the input as it is. */
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.5f, 0, 0, 0.5f, 0, 639.75f, 300.25f));
    CHECK(snap_at(639.5f, 300.5f, 0, 0, 1, 0, 0, 1, 1, 639.5f, 300.5f));
    /* The shader text is the reference's, in both languages. */
    {
        static const char *const text[2] = { D3D8_HLSL_SNAP_UV, D3D8_MSL_SNAP_UV };
        int k;
        for (k = 0; k < 2; k++) {
            CHECK(strstr(text[k], "float n = ts.z, eps = 1.0 / 64.0;"));
            CHECK(strstr(text[k], "if (n > 1.0 && abs(dx.y) <= eps && abs(dy.x) <= eps\n"
                                  "            && abs(abs(dx.x) * n - 1.0) <= eps"
                                  " && abs(abs(dy.y) * n - 1.0) <= eps) {"));
            CHECK(strstr(text[k], "float2 d = (floor(pos / n) + 0.5) * n - pos;"));
            CHECK(strstr(text[k], "uv += dx * d.x + dy * d.y;"));
        }
        CHECK(strstr(text[0], "float2 dx = ddx(uv), dy = ddy(uv);"));
        CHECK(strstr(text[1], "float2 dx = dfdx(uv), dy = dfdy(uv);"));
        /* derivatives before the branch */
        CHECK(strstr(text[0], "ddx(uv)") < strstr(text[0], "if (n > 1.0"));
        CHECK(strstr(text[1], "dfdx(uv)") < strstr(text[1], "if (n > 1.0"));
    }

    /* ---- the options round-trip; init picks the scale up ---- */
    {
        struct nv2a_host_opts o = { .render_scale = 3, .present_filter = NV2A_PRESENT_INTEGER,
                                    .fullscreen = 1 };
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_render_scale() == 3);
        CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_INTEGER && nv2a_host_opts()->fullscreen == 1);
        {
            static const struct nv2a_occ_ops ops = { q_get, q_put, q_begin, q_end, q_result, q_now, NULL };
            struct nv2a_occ oc;
            static uint32_t mem[8];
            nv2a_occ_init(&oc, &ops, (uint8_t *)mem);
            CHECK(oc.scale == 3);
        }
    }

    printf("render_scale: all passed\n");
    return 0;
}
