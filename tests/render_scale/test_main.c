/* The render-scale and present helpers (nv2a_backend_common.h), and the
 * guarantee they give at stock: every one is the identity at scale 1. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nv2a_backend_common.h"
#include "d3d8_hlsl.h"
#include "d3d8_msl.h"
#include "nv2a_vsh_cpu.h"

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

/* nv2a_snap_ok, nv2a_snap_uv2d and nv2a_snap_grad (D3D8_HLSL_SNAP_2D,
 * D3D8_MSL_SNAP_2D) in C, expression for expression: the reference the
 * shader text is checked against below. */
static int snap_ok(const float dx[2], const float dy[2], float z)
{
    const float n = fabsf(z), eps = 1.0f / 64.0f;
    return n > 1.0f && (z < 0.0f
        ? fabsf(dx[1]) <= fabsf(dx[0]) * eps && fabsf(dy[0]) <= fabsf(dy[1]) * eps
        : fabsf(dx[1]) <= eps && fabsf(dy[0]) <= eps
          && fabsf(fabsf(dx[0]) * n - 1.0f) <= eps && fabsf(fabsf(dy[1]) * n - 1.0f) <= eps);
}

static void snap_uv(float uv[2], const float pos[2], const float dx[2],
                    const float dy[2], float z)
{
    if (snap_ok(dx, dy, z)) {
        float n = fabsf(z);
        float d0 = (floorf(pos[0] / n) + 0.5f) * n - pos[0];
        float d1 = (floorf(pos[1] / n) + 0.5f) * n - pos[1];
        uv[0] += dx[0] * d0 + dy[0] * d1;
        uv[1] += dx[1] * d0 + dy[1] * d1;
    }
}

static void snap_grad(const float dx[2], const float dy[2], float z, float g[4])
{
    float k = snap_ok(dx, dy, z) ? fabsf(z) : 1.0f;
    g[0] = dx[0] * k; g[1] = dx[1] * k; g[2] = dy[0] * k; g[3] = dy[1] * k;
}

/* The mip level a sampler takes from gradients g (texture coordinates per
 * pixel, 0..1) on a size x size texture: log2 of the longer axis. Above 0
 * it minifies (the min filter, mip k), at or below 0 it magnifies. */
static float lod_of(const float g[4], float size)
{
    float a = sqrtf(g[0] * g[0] + g[1] * g[1]) * size;
    float b = sqrtf(g[2] * g[2] + g[3] * g[3]) * size;
    return log2f(a > b ? a : b);
}

/* What an implicit-gradient sample would see: the snapped uv's difference
 * across a quad's x pair (host pixels px and px + 1, px even). */
static float quad_dudx(float px, float py, float dxu, float z)
{
    float a[2] = {dxu * px, 0}, b[2] = {dxu * (px + 1), 0};
    float pa[2] = {px, py}, pb[2] = {px + 1, py}, dx[2] = {dxu, 0}, dy[2] = {0, dxu};
    snap_uv(a, pa, dx, dy, z);
    snap_uv(b, pb, dx, dy, z);
    return b[0] - a[0];
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

/* The host x (axis 0) or y (1) of guest screen coordinate g of a vertex
 * with clip w (or rhw) w, as the MSL and HLSL epilogues place it (snap, tie
 * shift for w 1 only, NDC, viewport of size hs), unrounded. */
static float host_raw(const struct nv2a_vp_consts *c, int axis, float g, float w, uint32_t hs)
{
    float p = truncf(g * 16.0f) / 16.0f, ndc;
    if (p - floorf(p) == 0.5f && w == 1.0f)
        p -= c->c[6 + axis];
    ndc = p * c->c[axis] + c->c[4 + axis];
    return axis ? (1.0f - ndc) * 0.5f * (float)hs : (ndc + 1.0f) * 0.5f * (float)hs;
}

/* host_raw on the 1/256-pixel grid a host rasteriser snaps to: only a stock
 * tie (the NV2A's own, left on a host centre at 1x) depends on it, float
 * NDC leaving 2/480 a hair off. Always w 1, a flat vertex the shift moves. */
static float host_of(const struct nv2a_vp_consts *c, int axis, float g, uint32_t hs)
{
    return roundf(host_raw(c, axis, g, 1.0f, hs) * 256.0f) / 256.0f;
}

/* How far host edge e is from the nearest host pixel centre. */
static float centre_gap(float e)
{
    return fabsf(e - 0.5f - floorf(e));
}

/* The host pixels a span from guest a to b covers under the top-left rule
 * (a centre on the first edge is in, on the second out): *lo..*hi. */
static int host_span(const struct nv2a_vp_consts *c, int axis, float a, float b, uint32_t hs,
                     int *lo, int *hi)
{
    float e0 = host_of(c, axis, a, hs), e1 = host_of(c, axis, b, hs);
    int j;
    *lo = -1; *hi = -2;
    for (j = 0; j < (int)hs; j++) {
        if ((float)j + 0.5f >= e0 && (float)j + 0.5f < e1) {
            if (*lo < 0)
                *lo = j;
            *hi = j;
        }
    }
    return *lo >= 0;
}

static int feq(float a, float b)
{
    return fabsf(a - b) < 1e-4f;
}

/* Guest LODs against a pack entry's chain (nv2a_stage_lod). */
static int test_stage_lod(void)
{
    static const uint32_t scales[] = { 1, 2, 4, 3 };
    static const float shifts[] = { 0.0f, 1.0f, 2.0f, 1.5849625f };
    uint32_t k, gmin;
    struct nv2a_stage_lod l;

    /* Stock: what the backends' clamps always were, and no key bits. */
    l = nv2a_stage_lod(0, 3, 9, 1, 1);
    CHECK(l.mip == 0 && l.min == 0.0f && l.max == 0.0f && l.key == 0);
    l = nv2a_stage_lod(2, 1, 5, 1, 6);
    CHECK(l.mip == 2 && l.min == 1.0f && l.max == 5.0f && l.key == 0);
    l = nv2a_stage_lod(1, 0, 5, 0, 6);
    CHECK(l.mip == 1 && l.min == 0.0f && l.max == 5.0f && l.key == 0);

    /* A guest chain: max follows the shift; min moves only when the guest
     * set one, or level 0, the new detail, could never be reached. */
    for (k = 0; k < 4; k++)
        for (gmin = 0; gmin < 2; gmin++) {
            l = nv2a_stage_lod(2, gmin, 5, scales[k], 6);
            CHECK(l.mip == 2);
            CHECK(feq(l.max, 5.0f + shifts[k]));
            CHECK(feq(l.min, gmin ? 1.0f + shifts[k] : 0.0f));
            if (!gmin)
                CHECK(l.min == 0.0f);
            CHECK(k ? (l.key >> NV2A_STAGE_LOD_SCALE_BIT & 15u) == scales[k] : l.key == 0);
            CHECK(!(l.key >> NV2A_STAGE_LOD_FORCED_BIT & 1u));
        }

    /* A guest of one level under a chain: the nearest level, over the
     * levels above the guest's own. */
    for (k = 1; k < 4; k++) {
        l = nv2a_stage_lod(0, 0, 0, scales[k], 4);
        CHECK(l.mip == 1 && l.min == 0.0f && feq(l.max, shifts[k]));
        CHECK(l.key >> NV2A_STAGE_LOD_FORCED_BIT & 1u);
    }
    /* A one-level entry has nothing to follow: no mip filter of its own. */
    l = nv2a_stage_lod(0, 0, 0, 2, 1);
    CHECK(l.mip == 0 && l.min == 0.0f && l.max == 0.0f);

    /* Two scales never share a sampler. */
    CHECK(nv2a_stage_lod(1, 0, 5, 2, 6).key != nv2a_stage_lod(1, 0, 5, 3, 6).key);

    CHECK(nv2a_stage_pack_scale(64, 64) == 1 && nv2a_stage_pack_scale(64, 128) == 2
          && nv2a_stage_pack_scale(64, 192) == 3 && nv2a_stage_pack_scale(64, 256) == 4);
    CHECK(nv2a_stage_pack_scale(64, 100) == 1 && nv2a_stage_pack_scale(64, 32) == 1
          && nv2a_stage_pack_scale(0, 32) == 1 && nv2a_stage_pack_scale(64, 64 * 16) == 1);
    return 0;
}

int main(void)
{
    struct nv2a_rect r;
    uint32_t hw, hh, x, y;

    /* ---- host options: zero is stock ---- */
    if (test_stage_lod())
        return 1;
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

    /* Hor+ padding: none at stock; at 16:9 the 4:3-shaped targets get
     * round((h*16/9 - w)/2) a side, square ones the same ratio and the
     * others nothing; the mapping
     * constants centre the guest columns in the padded width. */
    CHECK(nv2a_host_pad(640, 480) == 0);
    CHECK(nv2a_host_pad(512, 512) == 0);
    CHECK(nv2a_host_window_w(640, 480, 1) == 640);   /* stock window */
    CHECK(nv2a_host_window_w(640, 480, 3) == 1920);
    {
        struct nv2a_host_opts o;
        struct nv2a_pb_vsh v;
        struct nv2a_vp_consts c;
        memset(&o, 0, sizeof o);
        memset(&v, 0, sizeof v);
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 0, &c);
        CHECK(c.c[0] == 2.0f / 640.0f && c.c[4] == -1.0f && c.c[1] == -2.0f / 480.0f);
        /* spec_ctl.z (u[6]) follows the depth test: off, the epilogue caps
         * z at w and nothing is far-clipped, as on the NV2A; on, it clips. */
        CHECK(c.u[6] == 1 && c.u[4] == 0 && c.u[5] == 0);
        v.depth_enable = 1;
        v.spec_enable = 1;
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 0, &c);
        CHECK(c.u[6] == 0 && c.u[4] == 1 && c.u[5] == 0);
        v.depth_enable = 0;
        v.spec_enable = 0;
        o.aspect_w = 16; o.aspect_h = 9;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_pad(640, 480) == 107);
        CHECK(nv2a_host_pad(320, 240) == 53);
        /* Square (screen-space reflections): widened by the frame's ratio,
         * w * (16/9 / (4/3) - 1) / 2, plus the w/8 overscan. */
        CHECK(nv2a_host_pad(256, 256) == 75);
        CHECK(nv2a_host_pad(512, 512) == 149);
        CHECK(nv2a_host_pad(512, 256) == 0);      /* other shapes keep theirs */
        CHECK(nv2a_host_pad(640, 464) == 0);      /* Burnout 3's clip: not 4:3 */
        CHECK(nv2a_host_pad(0, 480) == 0);
        /* Small squares round down to no pad until the share is half a
         * pixel: 1 gets 0, 2 and 4 get 1, 16 gets 5. */
        CHECK(nv2a_host_pad(1, 1) == 0 && nv2a_host_pad(2, 2) == 1);
        CHECK(nv2a_host_pad(4, 4) == 1 && nv2a_host_pad(16, 16) == 5);
        {
            /* A reflection read from the frame's pads lands in the square's
             * pads: frame u = -pad/w and 1 + pad/w reach w/8 in from the
             * padded square's left and right edges (the overscan a bumped
             * read has to spare). Both pads are whole pixels, so that is
             * met within a texel, not exactly. */
            static const uint32_t sq[2] = { 512, 256 };
            static const uint32_t ratio[2][2] = { { 16, 9 }, { 16, 10 } };
            int r, q;
            for (r = 0; r < 2; r++) {
                o.aspect_w = ratio[r][0]; o.aspect_h = ratio[r][1];
                nv2a_host_opts_set(&o);
                for (q = 0; q < 2; q++) {
                    uint32_t fp = nv2a_host_pad(640, 480), sp = nv2a_host_pad(sq[q], sq[q]);
                    float sc, off, pw = (float)(sq[q] + 2u * sp);
                    float ul = -(float)fp / 640.0f, ur = 1.0f + (float)fp / 640.0f;
                    nv2a_rt_guest_read(sq[q], sp, &sc, &off);
                    float os = (float)sq[q] / 8.0f;
                    CHECK(fabsf((ul * sc + off) * pw - os) <= 1.0f);
                    CHECK(fabsf((ur * sc + off) * pw - (pw - os)) <= 1.0f);
                    /* the stock columns are unchanged: u 0..1 is texels sp..sp+w */
                    CHECK(fabsf(off * pw - (float)sp) < 1e-3f);
                    CHECK(fabsf((sc + off) * pw - (float)(sp + sq[q])) < 1e-3f);
                }
            }
            o.aspect_w = 16; o.aspect_h = 9;
            nv2a_host_opts_set(&o);
        }
        /* The window takes the padded target's shape, even and at scale. */
        CHECK(nv2a_host_window_w(640, 480, 1) == 854);
        CHECK(nv2a_host_window_w(640, 480, 2) == 1708);
        o.aspect_w = 16; o.aspect_h = 10;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_pad(640, 480) == 64);
        CHECK(nv2a_host_pad(512, 512) == 115);
        CHECK(nv2a_host_pad(256, 256) == 58);
        CHECK(nv2a_host_window_w(640, 480, 1) == 768);
        o.aspect_w = 21; o.aspect_h = 9;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_window_w(640, 480, 1) == 1120);
        CHECK(nv2a_host_pad(512, 512) == 256);
        o.aspect_w = 4; o.aspect_h = 3;
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_pad(640, 480) == 0);
        CHECK(nv2a_host_pad(512, 512) == 0);
        CHECK(nv2a_host_window_w(640, 480, 1) == 640);
        o.aspect_w = 16; o.aspect_h = 9;
        nv2a_host_opts_set(&o);
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 107, &c);
        /* guest x = 0 -> -640/854, x = 640 -> +640/854, x = 320 -> 0 */
        CHECK(c.c[0] == 2.0f / 854.0f && c.c[4] == -640.0f / 854.0f);
        CHECK(fabsf(320.0f * c.c[0] + c.c[4]) < 1e-6f);
        CHECK(c.c[1] == -2.0f / 480.0f && c.c[5] == 1.0f);
        memset(&o, 0, sizeof o);
        nv2a_host_opts_set(&o);
        CHECK(nv2a_host_pad(640, 480) == 0);
    }

    /* ---- hor+ placement (display.aspect) ---- */
    {
        struct nv2a_pb_vsh v;
        struct nv2a_vp_consts c0, c;
        static const uint32_t box[4] = { 0, 0, 640, 480 }, part[4] = { 16, 8, 320, 240 };
        uint32_t out[4], ref[4];
        int p;

        memset(&v, 0, sizeof v);
        /* pad 0 (stock): every placement leaves the constants and the box as
         * they are, so a stock run is bit-identical whatever it decided. */
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 0, &c0);
        for (p = NV2A_PLACE_CENTRE; p <= NV2A_PLACE_COVER; p++) {
            c = c0;
            nv2a_aspect_map(&c, 640, 480, 0, p);
            CHECK(memcmp(&c, &c0, sizeof c) == 0);
            nv2a_host_box(part, 640, 480, 1280, 960, ref);
            nv2a_aspect_box(part, 640, 480, 0, 2, p, 1, 0, out);
            CHECK(memcmp(out, ref, sizeof out) == 0);
            nv2a_aspect_box(part, 640, 480, 0, 2, p, 0, 0, out);
            CHECK(memcmp(out, ref, sizeof out) == 0);
        }

        /* 16:9, pad 107 (854 wide): guest x 0 and 640 land at these NDC. */
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 107, &c0);
        c = c0;
        nv2a_aspect_map(&c, 640, 480, 107, NV2A_PLACE_CENTRE);
        CHECK(memcmp(&c, &c0, sizeof c) == 0);
        c = c0;
        nv2a_aspect_map(&c, 640, 480, 107, NV2A_PLACE_STRETCH);
        CHECK(c.c[0] == 2.0f / 640.0f && c.c[4] == -1.0f && c.c[1] == c0.c[1] && c.c[5] == c0.c[5]);
        c = c0;
        nv2a_aspect_map(&c, 640, 480, 107, NV2A_PLACE_COVER);
        CHECK(c.c[0] == 2.0f / 640.0f && c.c[4] == -1.0f);
        /* y about the centre by 854/640: row 240 stays at 0, row 0 above +1 */
        CHECK(fabsf(240.0f * c.c[1] + c.c[5]) < 1e-6f);
        CHECK(fabsf(c.c[5] - 854.0f / 640.0f) < 1e-6f);

        /* Scissors at 16:9, n 2 (host 1708 x 960). */
        nv2a_aspect_box(box, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 1, 0, out);
        CHECK(out[0] == 214 && out[2] == 1494 && out[1] == 0 && out[3] == 960);
        nv2a_aspect_box(box, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 0, 0, out);
        CHECK(out[0] == 0 && out[2] == 1708);                             /* 3D fills the pads */
        nv2a_aspect_box(part, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 0, 0, out);
        CHECK(out[0] == 246 && out[2] == 854);                            /* a 3D sub-box does not */
        nv2a_aspect_box(box, 640, 480, 107, 2, NV2A_PLACE_STRETCH, 1, 0, out);
        CHECK(out[0] == 0 && out[2] == 1708);
        nv2a_aspect_box(box, 640, 480, 107, 2, NV2A_PLACE_COVER, 1, 0, out);
        CHECK(out[0] == 0 && out[2] == 1708 && out[3] == 960);

        /* The HUD layer (hud-anchors 1.6): a lifted 2D batch is boxed like
         * the world; a sub-box keeps its place. Only on the canvas target,
         * only 2D that is not flat, not full width and not wholly inside. */
        nv2a_aspect_box(box, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 1, 1, out);
        CHECK(out[0] == 0 && out[2] == 1708);
        nv2a_aspect_box(part, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 1, 1, out);
        CHECK(out[0] == 246 && out[2] == 854);
        CHECK(!nv2a_aspect_hud_lift(640, 480, 1, 0, 0, -50.0f, 100.0f));     /* flag clear */
        nv2a_aspect_hud_set(1, 640, 480);
        CHECK(nv2a_aspect_hud_on(640, 480) && !nv2a_aspect_hud_on(512, 512));
        CHECK(nv2a_aspect_hud_lift(640, 480, 1, 0, 0, -50.0f, 100.0f));
        CHECK(nv2a_aspect_hud_lift(640, 480, 1, 0, 0, 600.0f, 700.0f));
        CHECK(!nv2a_aspect_hud_lift(640, 480, 1, 0, 0, 10.0f, 630.0f));      /* wholly inside */
        CHECK(!nv2a_aspect_hud_lift(640, 480, 0, 0, 0, -50.0f, 100.0f));     /* 3D */
        CHECK(!nv2a_aspect_hud_lift(640, 480, 1, 1, 0, -50.0f, 100.0f));     /* flat */
        CHECK(!nv2a_aspect_hud_lift(640, 480, 1, 0, 1, -50.0f, 100.0f));     /* full width */
        CHECK(!nv2a_aspect_hud_lift(512, 512, 1, 0, 0, -50.0f, 100.0f));     /* other target */
        nv2a_aspect_hud_set(0, 640, 480);
        CHECK(!nv2a_aspect_hud_on(640, 480));
        {
            uint32_t fw = 0, fh = 0;
            nv2a_aspect_frame_publish(640, 480);
            CHECK(nv2a_aspect_frame(&fw, &fh) && fw == 640 && fh == 480);
        }

        /* A stretch on a padded square spans the ratio's pad only (85 of
         * 149 at 16:9, 810 wide): a full-width copy of the frame into it,
         * or of it into the frame, lines up with the frame's pads, and the
         * overscan columns stay out of it. */
        {
            struct nv2a_host_opts o;
            static const uint32_t sbox[4] = { 0, 0, 512, 512 };
            float sc, off;
            memset(&o, 0, sizeof o);
            o.aspect_w = 16; o.aspect_h = 9;
            nv2a_host_opts_set(&o);
            CHECK(nv2a_host_pad(512, 512) == 149 && nv2a_host_pad_ratio(512, 512) == 85);
            CHECK(nv2a_host_pad_ratio(640, 480) == 107 && nv2a_host_pad_ratio(512, 256) == 0);
            nv2a_vp_consts_fill(&v, 0x128, 512, 512, 149, &c0);
            c = c0;
            nv2a_aspect_map(&c, 512, 512, 149, NV2A_PLACE_STRETCH);
            CHECK(fabsf(c.c[4] - (128.0f / 810.0f - 1.0f)) < 1e-6f);          /* x 0 at column 64 */
            CHECK(fabsf(512.0f * c.c[0] + c.c[4] - (1.0f - 128.0f / 810.0f)) < 1e-6f);
            nv2a_aspect_box(sbox, 512, 512, 149, 2, NV2A_PLACE_STRETCH, 1, 0, out);
            CHECK(out[0] == 128 && out[2] == 1492 && out[3] == 1024);
            /* The frame's left pad edge, read the guest way, is that column. */
            nv2a_rt_guest_read(512, 149, &sc, &off);
            CHECK(fabsf((-107.0f / 640.0f * sc + off) * 810.0f - 64.0f) <= 1.0f);
            CHECK(nv2a_rt_stretch_read(512, 512, 149, &sc, &off)
                  && fabsf(sc - 682.0f / 810.0f) < 1e-6f && fabsf(off - 64.0f / 810.0f) < 1e-6f);
            CHECK(!nv2a_rt_stretch_read(640, 480, 107, &sc, &off));   /* the frame: whole */
            /* The frame itself still stretches over all of its pads. */
            c = c0;
            nv2a_vp_consts_fill(&v, 0x128, 640, 480, 107, &c);
            nv2a_aspect_map(&c, 640, 480, 107, NV2A_PLACE_STRETCH);
            CHECK(c.c[0] == 2.0f / 640.0f && c.c[4] == -1.0f);
            memset(&o, 0, sizeof o);
            nv2a_host_opts_set(&o);
        }

        /* The placement rules. */
        CHECK(nv2a_aspect_full_width(0, 640, 640) && nv2a_aspect_full_width(-1, 641, 640));
        CHECK(!nv2a_aspect_full_width(3, 640, 640) && !nv2a_aspect_full_width(0, 637, 640));
        CHECK(nv2a_aspect_place(0, 1, 1, 0, NV2A_MOVIE_FIT) == NV2A_PLACE_CENTRE);   /* 3D */
        CHECK(nv2a_aspect_place(1, 0, 0, 0, NV2A_MOVIE_FIT) == NV2A_PLACE_CENTRE);   /* a HUD piece */
        CHECK(nv2a_aspect_place(1, 1, 0, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_STRETCH); /* fade, post */
        CHECK(nv2a_aspect_place(1, 1, 1, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_CENTRE);  /* movie */
        CHECK(nv2a_aspect_place(1, 1, 1, 0, NV2A_MOVIE_FIT) == NV2A_PLACE_STRETCH);
        CHECK(nv2a_aspect_place(1, 1, 1, 0, NV2A_MOVIE_FILL) == NV2A_PLACE_COVER);
        /* a guest picture beside a padded target is a post pass's mask
         * (BLiNX 1's glow downsample), not a movie: stretched with the
         * frame; a picture alone stays a movie */
        CHECK(nv2a_aspect_place_tex(1, 0) && !nv2a_aspect_place_tex(1, 1));
        CHECK(!nv2a_aspect_place_tex(0, 0) && !nv2a_aspect_place_tex(0, 1));
        CHECK(nv2a_aspect_place(1, 1, nv2a_aspect_place_tex(1, 1), 0, NV2A_MOVIE_BLACK)
              == NV2A_PLACE_STRETCH);
        CHECK(nv2a_aspect_place(1, 1, nv2a_aspect_place_tex(1, 0), 0, NV2A_MOVIE_BLACK)
              == NV2A_PLACE_CENTRE);
        /* a full-width composite of an unpadded target (a 256x256 blur)
         * keeps its 4:3 shape; a padded one is stretched over the pads */
        CHECK(nv2a_aspect_place(1, 1, 0, 1, NV2A_MOVIE_BLACK) == NV2A_PLACE_CENTRE);
        CHECK(nv2a_aspect_place(1, 1, 0, 1, NV2A_MOVIE_FIT) == NV2A_PLACE_CENTRE);
        CHECK(nv2a_aspect_place(0, 1, 0, 1, NV2A_MOVIE_BLACK) == NV2A_PLACE_CENTRE);
        /* A full-width 2D pass over a padded square that samples no
         * unpadded target (a blur step between padded squares) is stretched
         * over the square's pads like the frame's post passes. */
        CHECK(nv2a_aspect_place(1, 1, 0, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_STRETCH);
        /* Which reads of a target's address sample the (padded) target:
         * Burnout 3's speed blur copies its frame to a 320x240 R5G6B5
         * surface (pitch 640) and reads it back as LIN_R5G6B5;
         * decoded from guest memory it was a 4:3 picture, centred. */
        CHECK(nv2a_surf_color_bytes(0x3) == 2 && nv2a_surf_color_bytes(0x8) == 4
              && nv2a_surf_color_bytes(0x0) == 0);
        /* Surface words: colour 3 R5G6B5, 1 X1R5G5B5, 8 A8R8G8B8; type 1
         * linear (0x100), 2 swizzled (0x200). */
        CHECK(nv2a_rt_tex_reads(0x11, 640, 0x103, 640));     /* LIN_R5G6B5 */
        CHECK(nv2a_rt_tex_reads(0x05, 0, 0x203, 640));       /* R5G6B5, swizzled */
        CHECK(nv2a_rt_tex_reads(0x1C, 640, 0x101, 640));     /* LIN_X1R5G5B5 */
        CHECK(nv2a_rt_tex_reads(0x10, 640, 0x102, 640));     /* LIN_A1R5G5B5 */
        CHECK(nv2a_rt_tex_reads(0x12, 2560, 0x108, 2560));   /* LIN_A8R8G8B8 */
        CHECK(nv2a_rt_tex_reads(0x06, 0, 0x208, 2560));      /* A8R8G8B8, swizzled */
        CHECK(!nv2a_rt_tex_reads(0x11, 640, 0x108, 2560));   /* a 32-bit target as 16-bit */
        CHECK(!nv2a_rt_tex_reads(0x12, 1280, 0x103, 640));   /* a 16-bit one as 32-bit */
        CHECK(!nv2a_rt_tex_reads(0x11, 640, 0x101, 640));    /* X1R5G5B5 read as R5G6B5 */
        CHECK(!nv2a_rt_tex_reads(0x1C, 640, 0x103, 640));    /* R5G6B5 read as X1R5G5B5 */
        CHECK(!nv2a_rt_tex_reads(0x1D, 640, 0x103, 640));    /* A4R4G4B4: no such surface */
        CHECK(!nv2a_rt_tex_reads(0x1D, 640, 0x101, 640));
        CHECK(!nv2a_rt_tex_reads(0x11, 640, 0x100, 640));    /* unknown colour field */
        CHECK(!nv2a_rt_tex_reads(0x12, 2560, 0x100, 2560));
        CHECK(!nv2a_rt_tex_reads(0x05, 0, 0x103, 640));      /* swizzled read, linear target */
        CHECK(!nv2a_rt_tex_reads(0x11, 640, 0x203, 640));    /* linear read, swizzled target */
        CHECK(!nv2a_rt_tex_reads(0x11, 1280, 0x103, 640));   /* another pitch */
        CHECK(!nv2a_rt_tex_reads(0x0C, 0, 0x208, 2560));     /* DXT1 */
        CHECK(!nv2a_rt_tex_reads(0x0B, 0, 0x209, 640));      /* P8 */
        /* Which rejected reads skip the target's write-back: KUF's front end
         * reads its 1280x960 pitch-5120 A8R8G8B8 backbuffer as a 640x480
         * pitch-2560 LIN_A8R8G8B8. */
        CHECK(nv2a_rt_pitch_alias(0x12, 2560, 480, 0x108, 5120));
        CHECK(nv2a_rt_pitch_alias(0x11, 640, 480, 0x108, 2560));    /* 32-bit target as 16-bit */
        CHECK(nv2a_rt_pitch_alias(0x11, 320, 2, 0x103, 640));       /* two rows is enough */
        /* A wider pitch is the target's picture with rows skipped (a field
         * read at twice the pitch), and one row is a span of one target
         * row: both keep the write-back. */
        CHECK(!nv2a_rt_pitch_alias(0x12, 10240, 480, 0x108, 5120));
        CHECK(!nv2a_rt_pitch_alias(0x11, 1280, 240, 0x103, 640));
        CHECK(!nv2a_rt_pitch_alias(0x12, 2560, 1, 0x108, 5120));
        CHECK(!nv2a_rt_pitch_alias(0x12, 2560, 0, 0x108, 5120));
        CHECK(!nv2a_rt_pitch_alias(0x12, 2560, 480, 0x108, 2560));  /* same pitch: write back */
        CHECK(!nv2a_rt_pitch_alias(0x11, 640, 480, 0x101, 640));    /* same pitch, other format */
        /* Swizzled and DXT reads keep it whatever their pitch. */
        CHECK(!nv2a_rt_pitch_alias(0x05, 0, 256, 0x203, 640));      /* swizzled read and target */
        CHECK(!nv2a_rt_pitch_alias(0x05, 1280, 240, 0x103, 640));   /* swizzled read, linear target */
        CHECK(!nv2a_rt_pitch_alias(0x05, 320, 240, 0x103, 640));
        CHECK(!nv2a_rt_pitch_alias(0x11, 320, 240, 0x203, 640));    /* linear read, swizzled target */
        CHECK(!nv2a_rt_pitch_alias(0x0C, 1280, 256, 0x108, 2560));  /* DXT1 */
        CHECK(!nv2a_rt_pitch_alias(0x0E, 1280, 256, 0x108, 2560));  /* DXT3 */
        CHECK(!nv2a_rt_pitch_alias(0x0F, 1280, 256, 0x108, 2560));  /* DXT5 */
        CHECK(!nv2a_rt_pitch_alias(0x12, 0, 480, 0x108, 2560));     /* no pitch */
        CHECK(!nv2a_rt_pitch_alias(0x12, 2560, 480, 0x108, 0));
        /* The pitch-alias trace keeps its own seen-set, apart from the
         * reject trace's below. */
        {
            static struct nv2a_pair_set seen;
            CHECK(nv2a_pair_new(&seen, 0x108, 5120, 0x12, 2560)
                  && !nv2a_pair_new(&seen, 0x108, 5120, 0x12, 2560)
                  && nv2a_pair_new(&seen, 0x103, 640, 0x1C, 640));
        }
        /* The reject trace says each distinct pair once. */
        CHECK(nv2a_rt_reject_new(0x103, 640, 0x1C, 640) && !nv2a_rt_reject_new(0x103, 640, 0x1C, 640)
              && nv2a_rt_reject_new(0x103, 640, 0x1C, 1280));
        CHECK(nv2a_rt_no_alpha(0x11) && nv2a_rt_no_alpha(0x1E) && !nv2a_rt_no_alpha(0x12));
        /* The 16x16 fade texture is no picture; a 1024-wide movie frame and
         * a 256-wide backdrop are. */
        CHECK(!nv2a_aspect_picture(16, 640, 0) && !nv2a_aspect_picture(160, 640, 0));
        CHECK(nv2a_aspect_picture(1024, 640, 0) && nv2a_aspect_picture(256, 640, 0));
        /* A bump stage's offset map is none, whatever its size (JSRF's
         * Can boost tint warps through a 256-wide one). */
        CHECK(!nv2a_aspect_picture(256, 640, 1) && !nv2a_aspect_picture(1024, 640, 1));
        /* Screen layers: w 1, the stage HUD's fixed depths 2 and 3 and the
         * title movie's 100; a world sprite's depth is no exact whole
         * number. */
        CHECK(nv2a_aspect_layer_w(1.0f) && nv2a_aspect_layer_w(2.0f) && nv2a_aspect_layer_w(3.0f)
              && nv2a_aspect_layer_w(100.0f));
        CHECK(!nv2a_aspect_layer_w(5.64599f) && !nv2a_aspect_layer_w(259.141f)
              && !nv2a_aspect_layer_w(3.0005f));
        CHECK(!nv2a_aspect_layer_w(0.5f) && !nv2a_aspect_layer_w(-1.0f) && !nv2a_aspect_layer_w(2048.0f));
    }

    /* ---- hor+ corner HUD: one batch spanning the frame, not full width ---- */
    {
        static float xy[12][2];
        static uint32_t idx[12];
        unsigned t;

        xy[0][0] = 0;   xy[0][1] = 0;   xy[1][0] = 120; xy[1][1] = 0;   xy[2][0] = 0;   xy[2][1] = 90;
        xy[3][0] = 640; xy[3][1] = 0;   xy[4][0] = 520; xy[4][1] = 0;   xy[5][0] = 640; xy[5][1] = 90;
        xy[6][0] = 0;   xy[6][1] = 480; xy[7][0] = 120; xy[7][1] = 480; xy[8][0] = 0;   xy[8][1] = 390;
        xy[9][0] = 640; xy[9][1] = 480; xy[10][0] = 520; xy[10][1] = 480; xy[11][0] = 640; xy[11][1] = 390;
        for (t = 0; t < 12; t++)
            idx[t] = t;
        CHECK(!nv2a_aspect_full_tris((const float (*)[2])xy, idx, 4, 640));
        /* The already-wide rule (hud-anchors 1.6): with the HUD layer on
         * this target a triangle spanning the frame counts only within
         * 8 px of its edges, so the layer's stretched sprites are not
         * stretched again; off, or on another target, stock. */
        {
            static float wx[3][2] = { { -100, 0 }, { 740, 0 }, { -100, 40 } };
            static const uint32_t wi[3] = { 0, 1, 2 };
            int wide = -1;
            CHECK(nv2a_aspect_full(-100, 740, (const float (*)[2])wx, wi, 1, 640, 480, &wide)
                  && wide == 0);
            nv2a_aspect_hud_set(1, 640, 480);
            CHECK(!nv2a_aspect_full(-100, 740, (const float (*)[2])wx, wi, 1, 640, 480, &wide)
                  && wide == 1);
            CHECK(nv2a_aspect_full(-100, 740, (const float (*)[2])wx, wi, 1, 640, 464, &wide));
            CHECK(!nv2a_aspect_full(-100, 740, NULL, NULL, 0, 640, 480, &wide) && wide == 1);
            wx[0][0] = wx[2][0] = -8;
            wx[1][0] = 648;
            CHECK(nv2a_aspect_full(-8, 648, (const float (*)[2])wx, wi, 1, 640, 480, &wide)
                  && wide == 0);
            CHECK(nv2a_aspect_full(-8, 648, NULL, NULL, 0, 640, 480, &wide));
            CHECK(!nv2a_aspect_full(3, 640, NULL, NULL, 0, 640, 480, &wide) && wide == 0);
            /* Past one edge only: an oversized full-screen triangle
             * (0..2w, KUF's menu resolve) stays full width, so H5 still stretches
             * it over the pads (hand check finding 2). */
            wx[0][0] = wx[2][0] = 0;
            wx[1][0] = 1280;
            wx[2][1] = 960;
            CHECK(nv2a_aspect_full(0, 1280, (const float (*)[2])wx, wi, 1, 640, 480, &wide)
                  && wide == 0);
            /* without a list one edge proves nothing: the batch is cleared */
            CHECK(!nv2a_aspect_full(0, 1280, NULL, NULL, 0, 640, 480, &wide) && wide == 1);
            wx[0][0] = wx[2][0] = -640;
            wx[1][0] = 640;
            CHECK(nv2a_aspect_full(-640, 640, (const float (*)[2])wx, wi, 1, 640, 480, &wide)
                  && wide == 0);
            nv2a_aspect_hud_set(0, 0, 0);
        }
        /* so it is a centred 2D batch: the HUD keeps its 4:3 layout */
        CHECK(nv2a_aspect_place(1, 0, 0, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_CENTRE);
    }

    /* ---- hor+ 2D test: a program's oPos.w, one screen layer ---- */
    {
        /* mov oPos, v0 (final): w comes straight from the vertex. */
        static uint32_t prog[NV2A_VSH_SLOTS][4];
        static float cst[NV2A_VSH_CONSTANTS][4];
        float vf[4][4] = { { 10, 0, 0, 1 }, { 630, 0, 0, 1 }, { 630, 480, 0, 1 }, { 10, 480, 0, 1 } };
        float hxy[4][2], lo = 0, hi = 0;
        int slot[1] = { 0 };
        struct nv2a_aspect_gate g = { -1, 0, 0 };
        unsigned i;

        prog[0][1] = (1u << 21) | 0x1Bu;                                 /* mov, src0 v0.xyzw */
        prog[0][2] = 2u << 26;                                           /* src0 mux v */
        prog[0][3] = (0xFu << 12) | (1u << 11) | 1u;                     /* oPos.xyzw, o reg, final */
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 1);
        CHECK(g.two_d == 1 && lo == 10.0f && hi == 630.0f);
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               hxy, &lo, &hi) == 1);
        CHECK(hxy[2][0] == 630.0f && hxy[2][1] == 480.0f);
        /* The dump's question: its answer, off any program's gate, whose
         * cadence decides what Hor+ does. */
#define BATCH_2D(count, depth_test) \
    nv2a_aspect_batch_2d((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, (count), \
                         (depth_test))
        {
            struct nv2a_aspect_gate before = g;
            CHECK(BATCH_2D(4, 1) == 1);
            CHECK(BATCH_2D(4, 0) == 1);
            vf[1][2] = 0.5f;    /* an orthographic mesh with depths */
            CHECK(BATCH_2D(4, 1) == 0);
            vf[1][2] = 0.0f;
            vf[1][3] = 5.0f;    /* perspective */
            CHECK(BATCH_2D(4, 1) == 0);
            vf[1][3] = 1.0f;
            CHECK(memcmp(&before, &g, sizeof g) == 0);
            /* Its near layer, the menus of Burnout 3's front end: only with
             * the depth test off, as nv2a_aspect_flat's callers ask. What the
             * frame-affecting tests answer does not move with it. */
            for (i = 0; i < 4; i++)
                vf[i][3] = 0.1f;
            CHECK(BATCH_2D(4, 0) == 1);
            CHECK(BATCH_2D(4, 1) == 0);
            vf[2][2] = 0.5f;    /* near, but with depths */
            CHECK(BATCH_2D(4, 0) == 0);
            vf[2][2] = 0.0f;
            {
                struct nv2a_aspect_gate fresh = { -1, 0, 0 }, kept;
                float w = 0.0f, l = 0.0f, h = 0.0f;
                CHECK(nv2a_aspect_scan(&fresh, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1,
                                       slot, 4, NULL, &l, &h) == 0);
                CHECK(fresh.two_d == 0);
                kept = fresh;
                (void)BATCH_2D(4, 0);
                CHECK(memcmp(&kept, &fresh, sizeof fresh) == 0);
                CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                                       &w) == 1 && w == 0.1f);
            }
            for (i = 0; i < 4; i++)
                vf[i][3] = 0.5f;
            CHECK(BATCH_2D(4, 0) == 1);
            CHECK(BATCH_2D(4, 1) == 0);
            for (i = 0; i < 4; i++)
                vf[i][3] = 1.5f;    /* neither a whole layer nor a reciprocal */
            CHECK(BATCH_2D(4, 0) == 0);
            CHECK(BATCH_2D(4, 1) == 0);
            for (i = 0; i < 4; i++)
                vf[i][3] = 1.0f;
            /* No answer: it must not settle a hash as 3D. */
            CHECK(BATCH_2D(0, 0) == -1);
            {
                uint32_t saved = prog[0][3];
                prog[0][3] &= ~1u;    /* no final bit: runs off the program */
                CHECK(BATCH_2D(4, 0) == -1);
                {
                    struct nv2a_aspect_gate fresh = { -1, 0, 0 };
                    float l = 0.0f, h = 0.0f;
                    /* The gate's own reading of that stays what it was. */
                    CHECK(nv2a_aspect_scan(&fresh, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0],
                                           1, slot, 4, NULL, &l, &h) == 0);
                    CHECK(fresh.two_d == -1 && fresh.n == 1);
                }
                prog[0][3] = saved;
            }
            CHECK(memcmp(&before, &g, sizeof g) == 0);
        }
        /* A screen layer at a fixed depth (the stage HUD's w 3) is 2D too. */
        for (i = 0; i < 4; i++)
            vf[i][3] = 3.0f;
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 1);
        /* Perspective (a depth per vertex, the first one whole): 3D; after
         * 16 such batches the gate checks only every 256th. */
        for (i = 0; i < 4; i++)
            vf[i][3] = 5.0f + 0.5f * (float)i;
        g.two_d = -1;
        g.n = 0;
        for (i = 0; i < 16; i++)
            CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                                   NULL, &lo, &hi) == 0);
        CHECK(g.two_d == 0 && g.n == 16);
        for (i = 0; i < 4; i++)
            vf[i][3] = 1.0f;
        for (i = 16; i < 256; i++)
            CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                                   NULL, &lo, &hi) == 0);                /* skipped */
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 1);                    /* the 256th */
        CHECK(g.two_d == 1);
        CHECK(!g.vary_z);
    }

    /* ---- render.snap_2d: one layer and one depth (vary_z) ---- */
    {
        /* A layer at one z is a flat 2D batch; an orthographic 3D mesh (a
         * menu model at w 1) shares w but not z, so the scan still calls
         * it 2D for hor+ but marks vary_z, and the snap leaves it alone. */
        static uint32_t prog[NV2A_VSH_SLOTS][4];
        static float cst[NV2A_VSH_CONSTANTS][4];
        static float chain[1024][4];
        float vf[4][4] = { { 10, 0, 0.5f, 1 }, { 630, 0, 0.5f, 1 }, { 630, 480, 0.5f, 1 },
                           { 10, 480, 0.5f, 1 } };
        float lo = 0, hi = 0;
        int slot[1] = { 0 };
        struct nv2a_aspect_gate g = { 1, 0, 0 };
        unsigned i;

        prog[0][1] = (1u << 21) | 0x1Bu;
        prog[0][2] = 2u << 26;
        prog[0][3] = (0xFu << 12) | (1u << 11) | 1u;
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 1 && !g.vary_z);
        vf[2][2] = 0.75f;
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 1 && g.vary_z);
        CHECK(nv2a_snap_flat2d(1, NV2A_PLACE_CENTRE) && !nv2a_snap_flat2d(1 && !g.vary_z,
                                                                         NV2A_PLACE_CENTRE));
        /* A merged chain (snow-fps joins batches of one program, up to
         * 24k indices) is sampled at 33 vertices, step count / 32: a 3D
         * tail that the samples reach makes the whole chain 3D, and 2D
         * sprites at several depths mark vary_z, so the snap skips the
         * chain rather than snapping part of it. */
        for (i = 0; i < 1024; i++) {
            chain[i][0] = (float)(i % 640);
            chain[i][1] = (float)(i % 480);
            chain[i][2] = 0.5f;
            chain[i][3] = 1.0f;
        }
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &chain[0][0], 1, slot,
                               1024, NULL, &lo, &hi) == 1 && !g.vary_z);
        for (i = 512; i < 1024; i++)
            chain[i][2] = 0.25f;                   /* a second HUD layer, nearer */
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &chain[0][0], 1, slot,
                               1024, NULL, &lo, &hi) == 1 && g.vary_z);
        for (i = 512; i < 1024; i++)
            chain[i][3] = 5.0f + 0.01f * (float)i;  /* a perspective tail */
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &chain[0][0], 1, slot,
                               1024, NULL, &lo, &hi) == 0);
    }

    /* ---- hor+ screen quad: a perspective program's quad at one w ---- */
    {
        /* JSRF's Can boost lays its 512 square over the frame with four
         * vertices at w 0.836 from a program that also draws the world. */
        static uint32_t prog[NV2A_VSH_SLOTS][4];
        static float cst[NV2A_VSH_CONSTANTS][4];
        float vf[7][4] = { { 0.53f, 0.53f, 5e6f, 0.83617f }, { 640.53f, 0.53f, 5e6f, 0.83617f },
                           { 0.53f, 480.53f, 5e6f, 0.83617f }, { 640.53f, 480.53f, 5e6f, 0.83617f },
                           { 0, 0, 0, 0.83617f }, { 0, 0, 0, 0.83617f }, { 0, 0, 0, 0.83617f } };
        float xy[NV2A_ASPECT_QUAD_MAX][2], lo = 0, hi = 0;
        uint32_t idx[6] = { 0, 1, 2, 1, 3, 2 };
        int slot[1] = { 0 };
        struct nv2a_aspect_gate g = { 0, 300, 0 };   /* a 3D program, gate not due */
        unsigned i;

        prog[0][1] = (1u << 21) | 0x1Bu;
        prog[0][2] = 2u << 26;
        prog[0][3] = (0xFu << 12) | (1u << 11) | 1u;
        CHECK(!nv2a_aspect_due(&g));
        CHECK(nv2a_aspect_scan(&g, (const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot, 4,
                               NULL, &lo, &hi) == 0);
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      4, xy, &lo, &hi) == 1);
        CHECK(lo == 0.53f && hi == 640.53f && xy[3][1] == 480.53f);
        CHECK(nv2a_aspect_quad_span(lo, hi, 640)
              && nv2a_aspect_full_tris((const float (*)[2])xy, idx, 2, 640));
        /* so, sampling only padded targets, it is stretched */
        CHECK(nv2a_aspect_quad_ok(1, 0, 0));
        CHECK(nv2a_aspect_place(1, 1, 0, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_STRETCH);
        /* Stages that rule it out: nothing padded sampled, an unpadded
         * target, a guest picture. */
        CHECK(!nv2a_aspect_quad_ok(0, 0, 0) && !nv2a_aspect_quad_ok(1, 1, 0)
              && !nv2a_aspect_quad_ok(1, 0, 1));
        /* A camera-facing world quad overhanging both edges (a haze plane)
         * spans the width but is no frame-sized pass. */
        CHECK(nv2a_aspect_full_width(-40.0f, 700.0f, 640) && !nv2a_aspect_quad_span(-40.0f, 700.0f, 640));
        CHECK(!nv2a_aspect_quad_span(-3.0f, 640.0f, 640) && !nv2a_aspect_quad_span(0.0f, 643.0f, 640));
        CHECK(nv2a_aspect_quad_span(-2.0f, 642.0f, 640));
        {
            /* The same quad from vertices, overhanging. */
            float ov[4][4] = { { -40, 0, 0, 0.83617f }, { 700, 0, 0, 0.83617f },
                               { -40, 480, 0, 0.83617f }, { 700, 480, 0, 0.83617f } };
            CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &ov[0][0], 1, slot,
                                          4, xy, &lo, &hi) == 1);
            CHECK(!nv2a_aspect_quad_span(lo, hi, 640));
        }
        {
            /* A lens-flare sprite: a small quad at one fractional w, a
             * screen quad by w alone, but not full width. */
            float fl[4][4] = { { 300, 200, 0, 0.71f }, { 340, 200, 0, 0.71f },
                               { 300, 240, 0, 0.71f }, { 340, 240, 0, 0.71f } };
            CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &fl[0][0], 1, slot,
                                          4, xy, &lo, &hi) == 1);
            CHECK(!nv2a_aspect_quad_span(lo, hi, 640));
        }
        {
            /* Two corner triangles at one w: their union spans the frame,
             * neither triangle does. */
            float ct[6][4] = { { 0, 0, 0, 0.9f }, { 120, 0, 0, 0.9f }, { 0, 90, 0, 0.9f },
                               { 640, 480, 0, 0.9f }, { 520, 480, 0, 0.9f }, { 640, 390, 0, 0.9f } };
            uint32_t cidx[6] = { 0, 1, 2, 3, 4, 5 };
            CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &ct[0][0], 1, slot,
                                          6, xy, &lo, &hi) == 1);
            CHECK(nv2a_aspect_quad_span(lo, hi, 640)
                  && !nv2a_aspect_full_tris((const float (*)[2])xy, cidx, 2, 640));
        }
        {
            /* Flat near layers (nv2a_aspect_flat), as Burnout 3's menus
             * draw them: its grid overhangs the frame to x 656 at w 0.1. */
            float gr[4][4] = { { 0, 60, 0, 0.1f }, { 656, 60, 0, 0.1f },
                               { 0, 420, 0, 0.1f }, { 656, 420, 0, 0.1f } };
            float big[100][4];
            float fw = 0.0f;
            int j;
            CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                   &fw) == 1 && fw == 0.1f);
            CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                   NULL) == 1);
            /* A tag at a car's depth, every vertex at one computed w. */
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = 0.774536f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            gr[0][3] = gr[1][3] = gr[2][3] = 0.1f;
            /* A depth per vertex: the world. */
            gr[3][3] = 0.2f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            /* One w past 1: a camera-facing sprite or flare (and the race
             * HUD's 14..365) still reaches the pads. One w of 1 is 2D's. */
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = 0.71f * 20.0f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = 1.0f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            /* Zero, negative, NaN: not a layer. No vertices: none. */
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = 0.0f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = -0.1f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = NAN;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 0,
                                    NULL));
            /* A NaN past the first vertex breaks the one w. */
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = 0.1f;
            gr[2][3] = NAN;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                    NULL));
            gr[2][3] = 0.1f;
            /* A program the interpreter can't finish (no final slot) is
             * not a layer, though its oPos.w reads 0.1. */
            {
                static uint32_t nofin[NV2A_VSH_SLOTS][4];
                memcpy(nofin[0], prog[0], sizeof nofin[0]);
                nofin[0][3] &= ~1u;
                CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])nofin, 0, cst, &gr[0][0], 1, slot,
                                        4, NULL));
            }
            /* One ulp off rhw 10 is still the layer: the tolerance holds
             * an authored 0.1 rounded either way. */
            gr[0][3] = gr[1][3] = gr[2][3] = gr[3][3] = nextafterf(0.1f, 1.0f);
            CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &gr[0][0], 1, slot, 4,
                                   NULL));
            CHECK(nv2a_aspect_flat_w(nextafterf(0.1f, 0.0f)));
            /* A large batch is sampled (every third of 100): a stray w
             * off the sample goes unseen, one on it is found. */
            for (j = 0; j < 100; j++) {
                big[j][0] = (float)j;
                big[j][1] = 0.0f;
                big[j][2] = 0.0f;
                big[j][3] = 0.1f;
            }
            CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &big[0][0], 1, slot, 100,
                                   NULL));
            big[1][3] = 0.5f;
            CHECK(nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &big[0][0], 1, slot, 100,
                                   NULL));
            big[3][3] = 0.5f;
            CHECK(!nv2a_aspect_flat((const uint32_t (*)[4])prog, 0, cst, &big[0][0], 1, slot, 100,
                                    NULL));
            /* The sample never passes NV2A_ASPECT_FLAT_MAX vertices, and a
             * sample that size is taken whole: D3D11 gathers it first. */
            {
                uint32_t cnt, ok = 1;
                for (cnt = 1; cnt <= 20000; cnt++) {
                    uint32_t st = nv2a_aspect_flat_step(cnt), ns = (cnt + st - 1) / st;
                    if (ns > NV2A_ASPECT_FLAT_MAX || nv2a_aspect_flat_step(ns) != 1)
                        ok = 0;
                }
                CHECK(ok);
            }
            /* Clipped as 2D, its full surface clip keeps to the guest
             * columns at 16:9, n 2; as 3D it reached the pads. */
            {
                uint32_t full[4] = { 0, 0, 640, 480 }, hb[4];
                nv2a_aspect_box(full, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 1, 0, hb);
                CHECK(hb[0] == 214 && hb[2] == 1494);
                nv2a_aspect_box(full, 640, 480, 107, 2, NV2A_PLACE_CENTRE, 0, 0, hb);
                CHECK(hb[0] == 0 && hb[2] == 1708);
            }
            /* A layer's w is a literal reciprocal (rhw 2..1024); a race
             * tag's is a computed depth and keeps reaching the pads. */
            CHECK(nv2a_aspect_flat_w(0.1f) && nv2a_aspect_flat_w(1.0f / 10.0f)
                  && nv2a_aspect_flat_w(0.5f) && nv2a_aspect_flat_w(0.25f)
                  && nv2a_aspect_flat_w(1.0f / 3.0f) && nv2a_aspect_flat_w(1.0f / 1024.0f));
            CHECK(!nv2a_aspect_flat_w(0.774536f) && !nv2a_aspect_flat_w(0.543762f)
                  && !nv2a_aspect_flat_w(0.999878f) && !nv2a_aspect_flat_w(0.3f)
                  && !nv2a_aspect_flat_w(0.1001f) && !nv2a_aspect_flat_w(1.0f / 1025.0f));
            CHECK(!nv2a_aspect_flat_w(1.0f) && !nv2a_aspect_flat_w(0.0f)
                  && !nv2a_aspect_flat_w(-0.5f) && !nv2a_aspect_flat_w(INFINITY)
                  && !nv2a_aspect_flat_w(NAN));
        }
        {
            /* Letterbox band pieces (nv2a_aspect_band), as Burnout 3's
             * crash cam draws them: quads at w 24.37 on a 640x480 frame. */
            const float bw = 24.3671875f;
            unsigned bars = NV2A_BAND_TOP | NV2A_BAND_BOT;
            uint32_t stamp[2] = { 0, 0 };
            float q[4][4];
#define BAND_QUAD(x0, x1, y0, y1)                                                    \
    (q[0][0] = (x0), q[0][1] = (y0), q[1][0] = (x1), q[1][1] = (y0),                 \
     q[2][0] = (x0), q[2][1] = (y1), q[3][0] = (x1), q[3][1] = (y1),                 \
     q[0][3] = q[1][3] = q[2][3] = q[3][3] = bw,                                     \
     nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &q[0][0], 1, slot, \
                             4, xy, &lo, &hi)                                        \
         && nv2a_aspect_band((const float (*)[2])xy, 4, 640, 480, bars))
            CHECK(BAND_QUAD(0, 640, 16, 72));            /* top bar */
            CHECK(BAND_QUAD(0, 640, 408, 464));          /* bottom bar */
            CHECK(BAND_QUAD(0, 320, 72, 73.87f));        /* a half of the top line */
            CHECK(BAND_QUAD(320, 640, 406.13f, 408));    /* a half of the bottom line */
            CHECK(BAND_QUAD(0, 640, 379.07f, 408));      /* the list backing */
            CHECK(BAND_QUAD(608, 640, 349.2f, 379.07f)); /* its raised right end */
            /* Inside a band but reaching no edge: an icon, the zero-wide
             * middle of a line. Text overhanging the frame (a ticker). */
            CHECK(!BAND_QUAD(457.4f, 487.4f, 41.2f, 69.2f));
            CHECK(!BAND_QUAD(320, 320, 72, 73.87f));
            CHECK(!BAND_QUAD(550.1f, 715.6f, 382.2f, 404.8f));
            /* A full-width banner mid-screen, and one that leaves the
             * top third: not a band. */
            CHECK(!BAND_QUAD(0, 640, 200, 280));
            CHECK(!BAND_QUAD(0, 640, 100, 170));
            /* A lens flare without depth: mid-screen it keeps its place,
             * as it does at an edge outside the bands. */
            CHECK(!BAND_QUAD(300, 340, 200, 240));
            CHECK(!BAND_QUAD(0, 40, 200, 240));
            /* A quad overhanging the frame (a haze plane) is no piece. */
            CHECK(!BAND_QUAD(-40, 700, 0, 100));
            /* Nor one overhanging the top or bottom edge. */
            CHECK(!BAND_QUAD(0, 640, -40, 100));
            CHECK(!BAND_QUAD(0, 640, 400, 520));
            /* A flare that touches an edge inside a band is taken as a
             * piece of it: the rule has no way to tell them apart. */
            CHECK(BAND_QUAD(0, 40, 20, 60));
            /* A NaN corner (fminf would skip it) or an infinite one. */
            CHECK(!BAND_QUAD(0, 640, 16, NAN));
            CHECK(!BAND_QUAD(0, 640, -INFINITY, 72));
            /* No bar stretched in a third: no piece there. Burnout 3's
             * menu frame has header strips and a bottom rule that reach
             * an edge at one w, but no bar. */
            bars = 0;
            CHECK(!BAND_QUAD(0, 640, 16, 72));
            CHECK(!BAND_QUAD(320, 640, 406.13f, 408));
            bars = NV2A_BAND_TOP;
            CHECK(BAND_QUAD(0, 320, 72, 73.87f));
            CHECK(!BAND_QUAD(320, 640, 406.13f, 408));
#undef BAND_QUAD
            /* The thirds a bar marks, and how long the mark lasts: this
             * flip and the next. */
            {
                float bar[4][2] = { { 0, 408 }, { 640, 408 }, { 0, 464 }, { 640, 464 } };
                float mid[4][2] = { { 0, 200 }, { 640, 200 }, { 0, 280 }, { 640, 280 } };
                CHECK(nv2a_aspect_third((const float (*)[2])bar, 4, 480) == NV2A_BAND_BOT);
                CHECK(nv2a_aspect_third((const float (*)[2])mid, 4, 480) == 0);
                CHECK(nv2a_aspect_third((const float (*)[2])bar, 0, 480) == 0);
            }
            CHECK(nv2a_aspect_bars(stamp, 0) == 0);
            nv2a_aspect_bar_mark(stamp, NV2A_BAND_BOT, 10);
            CHECK(nv2a_aspect_bars(stamp, 10) == NV2A_BAND_BOT);
            CHECK(nv2a_aspect_bars(stamp, 11) == NV2A_BAND_BOT);
            CHECK(nv2a_aspect_bars(stamp, 12) == 0);
            nv2a_aspect_bar_mark(stamp, 0, 12);
            CHECK(nv2a_aspect_bars(stamp, 12) == 0);
            /* A band piece is placed as a full-width pass: stretched. */
            CHECK(nv2a_aspect_place(1, 1, 0, 0, NV2A_MOVIE_BLACK) == NV2A_PLACE_STRETCH);
        }
        {
            /* BLiNX 2's corner triangles, one per corner, in one batch:
             * the batch spans the whole height, so it is no band piece,
             * and no triangle spans the width (Hor+ H4). */
            float ct[6][4] = { { 0, 0, 0, 2 }, { 120, 0, 0, 2 }, { 0, 90, 0, 2 },
                               { 640, 480, 0, 2 }, { 520, 480, 0, 2 }, { 640, 390, 0, 2 } };
            uint32_t cidx[6] = { 0, 1, 2, 3, 4, 5 };
            CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &ct[0][0], 1, slot,
                                          6, xy, &lo, &hi) == 1);
            CHECK(!nv2a_aspect_band((const float (*)[2])xy, 6, 640, 480,
                                    NV2A_BAND_TOP | NV2A_BAND_BOT)
                  && !nv2a_aspect_full_tris((const float (*)[2])xy, cidx, 2, 640));
        }
        {
            /* A world sprite: its corners at different depths, so not a
             * screen quad, band or not. */
            float sp[4][4] = { { 0, 10, 0, 3.1f }, { 60, 10, 0, 3.2f },
                               { 0, 70, 0, 3.1f }, { 60, 70, 0, 3.2f } };
            CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &sp[0][0], 1, slot,
                                          4, xy, &lo, &hi) == 0);
        }
        CHECK(!nv2a_aspect_band((const float (*)[2])xy, 0, 640, 480,
                                NV2A_BAND_TOP | NV2A_BAND_BOT));
        /* More than NV2A_ASPECT_QUAD_MAX vertices: not a quad. */
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      7, xy, &lo, &hi) == 0);
        /* A depth per vertex: 3D. */
        vf[2][3] = 0.9f;
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      4, xy, &lo, &hi) == 0);
        /* Behind the eye (w <= 0): not a screen quad. */
        for (i = 0; i < 4; i++)
            vf[i][3] = -1.0f;
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      4, xy, &lo, &hi) == 0);
        /* A w of NaN or infinity: not a screen quad. */
        for (i = 0; i < 4; i++)
            vf[i][3] = NAN;
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      4, xy, &lo, &hi) == 0);
        for (i = 0; i < 4; i++)
            vf[i][3] = INFINITY;
        CHECK(nv2a_aspect_screen_quad((const uint32_t (*)[4])prog, 0, cst, &vf[0][0], 1, slot,
                                      4, xy, &lo, &hi) == 0);
    }

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
    /* A hor+ 16:9 frame (854 x 480 guest, 2562 x 1440 at 3x) is a hair
     * wider than 16:9: it fills a 16:9 output, not a row short of it with
     * the cleared row at the bottom. 5K too (2 rows short). */
    CHECK(nv2a_present_rect(2562, 1440, 1920, 1080, NV2A_PRESENT_LINEAR, &r) == 0 && rect_is(&r, 0, 0, 1920, 1080));
    CHECK(nv2a_present_rect(2562, 1440, 3456, 1944, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 3456, 1944));
    CHECK(nv2a_present_rect(2562, 1440, 5120, 2880, NV2A_PRESENT_LINEAR, &r) == 0 && rect_is(&r, 0, 0, 5120, 2880));
    /* An axis already 1:1 is not stretched: a NEAREST 640 x 480 frame in a
     * 642 x 480 (or 640 x 481) window keeps whole pixels, bars and all. */
    CHECK(nv2a_present_rect(640, 480, 642, 480, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 1, 0, 640, 480));
    CHECK(nv2a_present_rect(640, 480, 640, 481, NV2A_PRESENT_NEAREST, &r) == 0 && rect_is(&r, 0, 0, 640, 480));
    /* The same frame in a 16:10 output stays letterboxed. */
    CHECK(nv2a_present_rect(2562, 1440, 1920, 1200, NV2A_PRESENT_LINEAR, &r) == 0 && rect_is(&r, 0, 60, 1920, 1079));
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

    /* ---- uv snap: which stages and batches (tex_scale[n].z) ---- */
    {
        enum { LIN = 0x12, SWZ = 0x06, DXT = 0x0C };      /* A8R8G8B8 linear, swizzled; DXT1 */
        const uint32_t bil = 2u << 16 | 2u << 24, pnt = 1u << 16 | 1u << 24;
        const uint32_t mag_only = 1u << 16 | 2u << 24, min_only = 2u << 16 | 1u << 24;
        struct nv2a_stage t = stage(1, LIN, bil);
        /* Any batch: linear (rect) stages only, the shader asks for a
         * unity blit (.z = N). */
        CHECK(nv2a_uv_snap(&t, 2, 0) == 2.0f);
        CHECK(nv2a_uv_snap(&t, 3, 0) == 3.0f);
        CHECK(nv2a_uv_snap(&t, 1, 0) == 0.0f);            /* stock: never */
        CHECK(nv2a_uv_snap(&t, 0, 0) == 0.0f);
        t = stage(1, LIN, mag_only);
        CHECK(nv2a_uv_snap(&t, 2, 0) == 2.0f);
        t = stage(1, LIN, min_only);
        CHECK(nv2a_uv_snap(&t, 2, 0) == 2.0f);
        t = stage(1, LIN, pnt);                             /* one texel, no bleed */
        CHECK(nv2a_uv_snap(&t, 2, 0) == 0.0f);
        CHECK(nv2a_uv_snap(&t, 2, 1) == 0.0f);
        /* A swizzled stage in a 3D batch keeps its scaled detail. */
        t = stage(1, SWZ, bil);
        CHECK(nv2a_uv_snap(&t, 2, 0) == 0.0f);
        t = stage(1, DXT, bil);
        CHECK(nv2a_uv_snap(&t, 2, 0) == 0.0f);
        t = stage(0, LIN, bil);                             /* stage off */
        CHECK(nv2a_uv_snap(&t, 2, 0) == 0.0f);
        CHECK(nv2a_uv_snap(&t, 2, 1) == 0.0f);
        t = stage(4, LIN, bil);                             /* pass-through */
        CHECK(nv2a_uv_snap(&t, 2, 0) == 0.0f);
        CHECK(nv2a_uv_snap(&t, 2, 1) == 0.0f);
        t = stage(2, LIN, bil);                             /* other modes sample as 2D */
        CHECK(nv2a_uv_snap(&t, 2, 0) == 2.0f);
        /* A centred 2D batch: every bilinear format, any axis-aligned
         * mapping (.z = -N) -- BLiNX 2's loading tiles are swizzled DXT1. */
        t = stage(1, DXT, bil);
        CHECK(nv2a_uv_snap(&t, 2, 1) == -2.0f);
        CHECK(nv2a_uv_snap(&t, 3, 1) == -3.0f);
        CHECK(nv2a_uv_snap(&t, 1, 1) == 0.0f);            /* stock: never */
        t = stage(1, SWZ, mag_only);
        CHECK(nv2a_uv_snap(&t, 2, 1) == -2.0f);
        t = stage(1, LIN, bil);
        CHECK(nv2a_uv_snap(&t, 2, 1) == -2.0f);

        /* Which batches are flat 2D: a 2D one (two_d 1), centred. 3D (0),
         * not yet checked (-1), stretched or covering ones are not. */
        CHECK(nv2a_snap_flat2d(1, NV2A_PLACE_CENTRE));
        CHECK(!nv2a_snap_flat2d(0, NV2A_PLACE_CENTRE));
        CHECK(!nv2a_snap_flat2d(-1, NV2A_PLACE_CENTRE));
        CHECK(!nv2a_snap_flat2d(1, NV2A_PLACE_STRETCH));
        CHECK(!nv2a_snap_flat2d(1, NV2A_PLACE_COVER));
    }
    {
        /* Per batch: render-target stages keep 0, the rest follow the
         * target's scale and the batch. */
        struct nv2a_stage st[4];
        struct nv2a_ps_consts pc;
        int k;
        st[0] = stage(1, 0x0C, 2u << 16 | 2u << 24);       /* DXT1 tile */
        st[1] = stage(1, 0x12, 2u << 16 | 2u << 24);       /* rect */
        st[2] = stage(1, 0x12, 2u << 16 | 2u << 24);       /* a render target */
        st[3] = stage(1, 0x06, 1u << 16 | 1u << 24);       /* point */
        memset(&pc, 0, sizeof pc);
        for (k = 0; k < 4; k++)
            pc.tex_scale[k][2] = 7.0f;
        pc.tex_scale[2][2] = 0.0f;
        nv2a_ps_consts_snap(&pc, st, 1u << 2, 3, 1);
        CHECK(pc.tex_scale[0][2] == -3.0f && pc.tex_scale[1][2] == -3.0f);
        CHECK(pc.tex_scale[2][2] == 0.0f && pc.tex_scale[3][2] == 0.0f);
        nv2a_ps_consts_snap(&pc, st, 1u << 2, 3, 0);    /* 3D */
        CHECK(pc.tex_scale[0][2] == 0.0f && pc.tex_scale[1][2] == 3.0f);
        CHECK(pc.tex_scale[2][2] == 0.0f && pc.tex_scale[3][2] == 0.0f);
        /* A target clamped below render.scale (nv2a_host_size) snaps by
         * its own n. */
        nv2a_ps_consts_snap(&pc, st, 0, 2, 1);
        CHECK(pc.tex_scale[0][2] == -2.0f && pc.tex_scale[2][2] == -2.0f);
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
    /* .z = -N (a centred 2D batch): any axis-aligned mapping. BLiNX 2's
     * loading tile, v magnified 2x over 128 texels (dy.v = 1/256 a guest
     * pixel, 1/512 a host pixel at scale 2), WRAP: at its last guest row
     * (v 127.75/128 at 1x) the two host rows read 127.625 and 127.875
     * before -- the second blends 25% of the wrapped row 0 -- and both
     * read the 1x point after. */
    CHECK(snap_at(100.5f, 511.5f, 0, 0, 1.0f / 1024, 0, 0, 1.0f / 512, -2,
                  50.5f / 512, 255.5f / 256));
    CHECK(snap_at(100.5f, 510.5f, 0, 0, 1.0f / 1024, 0, 0, 1.0f / 512, -2,
                  50.5f / 512, 255.5f / 256));
    /* A constant u (126.5 of 128): u stays, v snaps. */
    CHECK(snap_at(100.5f, 7.5f, 126.5f / 128, 0, 0, 0, 0, 1.0f / 256, -2,
                  126.5f / 128, 3.5f / 128));
    /* Scale 3, minified 2x (two texels per guest pixel). */
    CHECK(snap_at(3.5f, 5.5f, 0, 0, 2.0f / 3, 0, 0, 2.0f / 3, -3, 3.0f, 3.0f));
    /* Not axis-aligned (a rotated sprite): the host-rate sample stays. */
    CHECK(snap_at(100.5f, 7.5f, 0, 0, 0.5f, 0.5f, -0.5f, 0.5f, -2,
                  50.25f - 3.75f, 50.25f + 3.75f));
    /* The same magnified mapping at .z = +N is no unity blit: kept. */
    CHECK(snap_at(100.5f, 511.5f, 0, 0, 1.0f / 1024, 0, 0, 1.0f / 512, 2,
                  100.5f / 1024, 511.5f / 512));
    /* .z = 0 (stock, or not eligible) and .z = 1: the input as it is. */
    CHECK(snap_at(1279.5f, 600.5f, 0, 0, 0.5f, 0, 0, 0.5f, 0, 639.75f, 300.25f));
    CHECK(snap_at(639.5f, 300.5f, 0, 0, 1, 0, 0, 1, 1, 639.5f, 300.5f));
    /* A mirrored sprite (dx.u < 0, dy.v < 0) at .z = -2: still
     * axis-aligned, snapped to the 1x point of its guest pixel. u = 1 -
     * px / 1024 at 1x is 1 - (2 * 50 + 1) / 2048 for host column 100. */
    CHECK(snap_at(100.5f, 7.5f, 1.0f, 1.0f, -1.0f / 2048, 0, 0, -1.0f / 2048, -2,
                  1.0f - 101.0f / 2048, 1.0f - 7.0f / 2048));
    /* Swapped axes (a sprite turned 90 degrees, dx = (0, a), dy = (a, 0)):
     * not axis-aligned for the test, so the host-rate sample stays. That
     * only gives up the snap; it never moves a sample wrongly. */
    CHECK(snap_at(100.5f, 7.5f, 0, 0, 0, 1.0f / 512, 1.0f / 512, 0, -2,
                  7.5f / 512, 100.5f / 512));

    /* ---- the 2D snap's sampling gradients (nv2a_snap_grad) ---- */
    {
        /* A minified, mipmapped 2D stage: 256 texels, two per guest pixel
         * (mip 1 at 1x). At scale 2 the host derivative is 1/256 a pixel. */
        const float h = 1.0f / 256, oneX[4] = {2 * h, 0, 0, 2 * h};
        float dx[2] = {h, 0}, dy[2] = {0, h}, g[4], g3[4];
        CHECK(fabsf(lod_of(oneX, 256) - 1.0f) < 1e-4f);
        /* Without the explicit gradients: inside a quad the snapped uv is
         * constant at scale 2 (LOD -inf: mip 0, the mag filter) ... */
        CHECK(quad_dudx(100.5f, 7.5f, h, -2) == 0.0f);
        /* ... and at scale 3 it is 0 in one quad, three host steps (one
         * 1x step and a half) in the next: the LOD jumps quad to quad. */
        CHECK(quad_dudx(0.5f, 7.5f, h * 2 / 3, -3) == 0.0f);
        CHECK(fabsf(quad_dudx(2.5f, 7.5f, h * 2 / 3, -3) - 2 * h) < 1e-6f);
        /* With them: the 1x derivatives, so mip 1 and the min filter, as
         * at 1x, at every scale and in every quad. */
        snap_grad(dx, dy, -2, g);
        CHECK(!memcmp(g, oneX, sizeof g));
        CHECK(fabsf(lod_of(g, 256) - 1.0f) < 1e-4f);
        dx[0] = dy[1] = h * 2 / 3;
        snap_grad(dx, dy, -3, g3);
        CHECK(fabsf(lod_of(g3, 256) - 1.0f) < 1e-4f);
        /* Magnified 2x (mag at 1x, LOD -1): still -1, still the mag filter. */
        dx[0] = dy[1] = h / 4;
        snap_grad(dx, dy, -2, g);
        CHECK(fabsf(lod_of(g, 256) + 1.0f) < 1e-4f);
        /* A mirrored mapping keeps its signs, times N. */
        dx[0] = -h; dy[1] = -h;
        snap_grad(dx, dy, -2, g);
        CHECK(g[0] == -2 * h && g[3] == -2 * h && g[1] == 0 && g[2] == 0);
        /* Not snapped (rotated, or .z = 0): the host derivatives, as an
         * implicit sample takes them. */
        dx[0] = h; dx[1] = h; dy[0] = -h; dy[1] = h;
        snap_grad(dx, dy, -2, g);
        CHECK(g[0] == h && g[1] == h && g[2] == -h && g[3] == h);
        dx[1] = dy[0] = 0;
        snap_grad(dx, dy, 0, g);
        CHECK(g[0] == h && g[3] == h);
    }

    /* ---- render.snap_2d: off by default, and only above scale 1 ---- */
    {
        struct nv2a_host_opts o;
        memset(&o, 0, sizeof o);
        nv2a_host_opts_set(&o);
        CHECK(!nv2a_snap_2d_on(1) && !nv2a_snap_2d_on(2) && !nv2a_snap_2d_on(3));
        o.snap_2d = 1;
        nv2a_host_opts_set(&o);
        CHECK(!nv2a_snap_2d_on(0) && !nv2a_snap_2d_on(1));
        CHECK(nv2a_snap_2d_on(2) && nv2a_snap_2d_on(3));
        o.snap_2d = 0;
        nv2a_host_opts_set(&o);
    }

    /* The shader text is the reference's, in both languages: the bodies
     * are the same text but for the derivative names. */
    {
        static const char *const text[2] = { D3D8_HLSL_SNAP_2D, D3D8_MSL_SNAP_2D };
        char hl[2048], *p;
        size_t len = strlen(text[0]);
        int k;
        for (k = 0; k < 2; k++) {
            CHECK(strstr(text[k], "float n = abs(ts.z), eps = 1.0 / 64.0;"));
            CHECK(strstr(text[k], "return n > 1.0 && (ts.z < 0.0\n"
                                  "        ? abs(dx.y) <= abs(dx.x) * eps"
                                  " && abs(dy.x) <= abs(dy.y) * eps\n"
                                  "        : abs(dx.y) <= eps && abs(dy.x) <= eps\n"
                                  "          && abs(abs(dx.x) * n - 1.0) <= eps"
                                  " && abs(abs(dy.y) * n - 1.0) <= eps);\n"));
            CHECK(strstr(text[k], "    if (nv2a_snap_ok(dx, dy, ts)) {\n"
                                  "        float n = abs(ts.z);\n"
                                  "        float2 d = (floor(pos / n) + 0.5) * n - pos;\n"
                                  "        uv += dx * d.x + dy * d.y;\n"));
            CHECK(strstr(text[k], "return float4(dx, dy) * (nv2a_snap_ok(dx, dy, ts)"
                                  " ? abs(ts.z) : 1.0);"));
        }
        CHECK(strstr(text[0], "float2 dx = ddx(uv), dy = ddy(uv);"));
        CHECK(strstr(text[1], "float2 dx = dfdx(uv), dy = dfdy(uv);"));
        /* derivatives before the branch */
        CHECK(strstr(text[0], "ddx(uv)") < strstr(text[0], "if (nv2a_snap_ok"));
        CHECK(strstr(text[1], "dfdx(uv)") < strstr(text[1], "if (nv2a_snap_ok"));
        /* HLSL with ddx/ddy spelt dfdx/dfdy is the MSL, byte for byte */
        CHECK(len + 16 < sizeof hl);
        hl[0] = 0;
        for (const char *s = text[0]; *s; ) {
            const char *a = strstr(s, "ddx("), *b = strstr(s, "ddy("), *q;
            q = !a ? b : !b ? a : a < b ? a : b;
            if (!q) {
                strcat(hl, s);
                break;
            }
            strncat(hl, s, (size_t)(q - s));
            strcat(hl, q[2] == 'x' ? "dfdx(" : "dfdy(");
            s = q + 4;
        }
        p = hl;
        CHECK(!strcmp(p, text[1]));
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

    /* ---- ties on guest pixel centres (nv2a_vp_tie_fill) ----
     * KUF's full-screen passes run 0..640 in D3D terms, 0.5..640.5 after
     * the XDK bias and the snap: at 1x the top-left rule keeps row and
     * column 0. Scaled, or stretched by hor+, that edge cut the first guest
     * pixel's host block and left host row/column 0 uncovered. */
    {
        struct nv2a_host_opts o;
        struct nv2a_pb_vsh v;
        struct nv2a_vp_consts c;
        uint32_t pad;
        int lo, hi;
        unsigned n;
        memset(&o, 0, sizeof o);
        memset(&v, 0, sizeof v);
        nv2a_host_opts_set(&o);
        /* Stock 1x 4:3: no shift, the NV2A's coverage. */
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, 0, &c);
        nv2a_vp_tie_fill(&c, 640, 480);
        CHECK(c.c[6] == 0.0f && c.c[7] == 0.0f);
        CHECK(host_span(&c, 0, 0.53125f, 640.53125f, 640, &lo, &hi) && lo == 0 && hi == 639);
        CHECK(host_span(&c, 1, 0.53125f, 480.53125f, 480, &lo, &hi) && lo == 0 && hi == 479);
        for (n = 2; n <= 4; n++) {
            nv2a_vp_consts_fill(&v, 0x128, 640, 480, 0, &c);
            nv2a_vp_tie_fill(&c, 640 * n, 480 * n);
            /* A tie quad covers every host pixel, as the guest covers every
             * guest pixel; without the shift row and column 0 were out. */
            CHECK(host_span(&c, 0, 0.53125f, 640.53125f, 640 * n, &lo, &hi) && lo == 0
                  && hi == (int)(640 * n) - 1);
            CHECK(host_span(&c, 1, 0.53125f, 480.53125f, 480 * n, &lo, &hi) && lo == 0
                  && hi == (int)(480 * n) - 1);
            /* The shifted edges sit a quarter host pixel from any centre:
             * no host tie, nothing left to the rasteriser's grid. */
            CHECK(fabsf(centre_gap(host_raw(&c, 0, 0.53125f, 1.0f, 640 * n)) - 0.25f) < 1e-3f);
            CHECK(fabsf(centre_gap(host_raw(&c, 1, 480.53125f, 1.0f, 480 * n)) - 0.25f) < 1e-3f);
            /* A perspective (w != 1) vertex on a tie keeps its place, so 3D
             * edges never step as they move. */
            CHECK(fabsf(host_raw(&c, 0, 100.5f, 2.0f, 640 * n) - 100.5f * (float)n) < 1e-3f);
            CHECK(fabsf(host_raw(&c, 1, 100.5f, 0.5f, 480 * n) - 100.5f * (float)n) < 1e-3f);
            /* An edge on pixel corners (D3D's -0.5 quad) does not move. */
            CHECK(host_span(&c, 0, 0.03125f, 640.03125f, 640 * n, &lo, &hi) && lo == 0
                  && hi == (int)(640 * n) - 1);
            CHECK(host_of(&c, 0, 100.25f, 640 * n) == 100.25f * (float)n);
            /* A tie inside: guest pixels 100..199, host blocks 100n..200n-1. */
            CHECK(host_span(&c, 0, 100.5f, 200.5f, 640 * n, &lo, &hi) && lo == (int)(100 * n)
                  && hi == (int)(200 * n) - 1);
        }
        /* Hor+ 16:10, stretched: 0.5 * 768/640 was 0.6, column 0 out at 1x. */
        o.aspect_w = 16; o.aspect_h = 10;
        nv2a_host_opts_set(&o);
        pad = nv2a_host_pad(640, 480);
        CHECK(pad == 64);
        for (n = 1; n <= 3; n++) {
            uint32_t pw = (640 + 2 * pad) * n;
            nv2a_vp_consts_fill(&v, 0x128, 640, 480, pad, &c);
            nv2a_aspect_map(&c, 640, 480, pad, NV2A_PLACE_STRETCH);
            nv2a_vp_tie_fill(&c, pw, 480 * n);
            CHECK(host_span(&c, 0, 0.53125f, 640.53125f, pw, &lo, &hi) && lo == 0
                  && hi == (int)pw - 1);
            CHECK(host_span(&c, 1, 0.53125f, 480.53125f, 480 * n, &lo, &hi) && lo == 0
                  && hi == (int)(480 * n) - 1);
            /* 1.2 host px a guest px at 1x: still clear of the centres. */
            CHECK(centre_gap(host_raw(&c, 0, 0.53125f, 1.0f, pw)) > 0.2f);
            CHECK(centre_gap(host_raw(&c, 0, 640.53125f, 1.0f, pw)) > 0.2f);
        }
        /* display.movie=fill covers the width and crops y: a tie quad still
         * covers every host pixel, 1x to 3x. */
        for (n = 1; n <= 3; n++) {
            uint32_t pw = (640 + 2 * pad) * n;
            nv2a_vp_consts_fill(&v, 0x128, 640, 480, pad, &c);
            nv2a_aspect_map(&c, 640, 480, pad, NV2A_PLACE_COVER);
            nv2a_vp_tie_fill(&c, pw, 480 * n);
            CHECK(c.c[6] > 0.0f && c.c[7] > 0.0f);
            CHECK(host_span(&c, 0, 0.53125f, 640.53125f, pw, &lo, &hi) && lo == 0
                  && hi == (int)pw - 1);
            CHECK(host_span(&c, 1, 0.53125f, 480.53125f, 480 * n, &lo, &hi) && lo == 0
                  && hi == (int)(480 * n) - 1);
        }
        /* Centred on the pads at 3x: the columns start at pad * 3. */
        nv2a_vp_consts_fill(&v, 0x128, 640, 480, pad, &c);
        nv2a_aspect_map(&c, 640, 480, pad, NV2A_PLACE_CENTRE);
        nv2a_vp_tie_fill(&c, (640 + 2 * pad) * 3, 480 * 3);
        CHECK(host_span(&c, 0, 0.53125f, 640.53125f, (640 + 2 * pad) * 3, &lo, &hi)
              && lo == (int)pad * 3 && hi == (int)(pad + 640) * 3 - 1);
        memset(&o, 0, sizeof o);
        nv2a_host_opts_set(&o);
    }

    printf("render_scale: all passed\n");
    return 0;
}
