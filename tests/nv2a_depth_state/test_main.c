/* W-buffering and polygon offset, as the GPU backends translate them
 * (nv2a_backend_common.h): the batches that take the W path, the
 * host depth bias for the others, and the W shader's constants. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "nv2a_backend_common.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)
#define NEAR(a, b) (fabsf((a) - (b)) <= 1e-6f * (fabsf(b) > 1.0f ? fabsf(b) : 1.0f))

#define Z24 0x00000128u        /* SET_SURFACE_FORMAT: A8R8G8B8, Z24S8 */
#define Z16 0x00000118u        /* the same, Z16 */
#define TRIANGLES 5u
#define LINES     2u
#define POLYGON   10u

static float u2f(uint32_t u)
{
    float f;
    memcpy(&f, &u, sizeof f);
    return f;
}

/* Quantum Redshift's 3D state, from a method log of its race. */
static void qr_state(struct nv2a_pb_vsh *v)
{
    memset(v, 0, sizeof *v);
    v->control0 = 0x110001u;
    v->depth_enable = 1;
    v->clip_min = u2f(0x407FFFFBu);
    v->clip_max = u2f(0x4B7FFFFFu);
    v->clip_set = 3;
    v->zminmax_ctl = 1u;
}

int main(void)
{
    struct nv2a_pb_vsh v;
    struct nv2a_wdepth_consts w;
    float bias, slope;

    /* Which batches take the W path. */
    qr_state(&v);
    CHECK(nv2a_w_depth_on(&v, 1, 1));
    CHECK(!nv2a_w_depth_on(&v, 0, 1));          /* vs_pass: no zw output */
    CHECK(!nv2a_w_depth_on(&v, 1, 0));          /* fs_basic: no W variant */
    v.depth_enable = 0;
    CHECK(!nv2a_w_depth_on(&v, 1, 1));          /* nothing tested or written */
    qr_state(&v);
    v.control0 = 0x100001u;                     /* Z_PERSPECTIVE off */
    CHECK(!nv2a_w_depth_on(&v, 1, 1));

    /* Polygon offset off: no host bias, whatever the values. */
    qr_state(&v);
    v.poly_factor = -0.25f;
    v.poly_bias = -1.0f;
    nv2a_depth_bias(&v, TRIANGLES, Z24, 1, &bias, &slope);
    CHECK(bias == 0.0f && slope == 0.0f);

    /* QR's decals: factor -0.25, bias -1, on Z24 at 1x: one 2^-24 step. */
    v.poly_offset = 1;
    CHECK(nv2a_poly_offset_on(&v, TRIANGLES) && nv2a_poly_offset_on(&v, POLYGON));
    CHECK(!nv2a_poly_offset_on(&v, LINES) && !nv2a_poly_offset_on(&v, 11u));
    nv2a_depth_bias(&v, TRIANGLES, Z24, 1, &bias, &slope);
    CHECK(NEAR(bias, -1.0f * 16777216.0f / 16777215.0f));
    CHECK(NEAR(slope, -0.25f));
    /* At 3x the factor follows the host pixels; the bias does not. */
    nv2a_depth_bias(&v, TRIANGLES, Z24, 3, &bias, &slope);
    CHECK(NEAR(bias, -1.0f * 16777216.0f / 16777215.0f));
    CHECK(NEAR(slope, -0.75f));
    /* Z16: one step is 1/65535 of the range, 256 float units near 1. */
    nv2a_depth_bias(&v, TRIANGLES, Z16, 1, &bias, &slope);
    CHECK(NEAR(bias, -16777216.0f / 65535.0f));
    /* A line takes no fill offset. */
    nv2a_depth_bias(&v, LINES, Z24, 1, &bias, &slope);
    CHECK(bias == 0.0f && slope == 0.0f);
    /* Scale 0 (no host yet) counts as 1x. */
    nv2a_depth_bias(&v, TRIANGLES, Z24, 0, &bias, &slope);
    CHECK(NEAR(slope, -0.25f));

    /* The W constants for QR: cull outside [about 4, 2^24 - 1]. */
    qr_state(&v);
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 2, &w);
    CHECK(w.w0[0] == 0.0f && w.w0[1] == 0.0f);  /* no offset enabled */
    CHECK(w.w0[2] == u2f(0x407FFFFBu) && w.w0[3] == 16777215.0f);
    CHECK(NEAR(w.w1[0], 1.0f / 16777215.0f));
    CHECK(w.w1[1] == 1.0f);
    CHECK(w.w1[2] == 0.0f && w.w1[3] == 0.0f);
    /* With the offset, in w units (before the divide by zmax). */
    v.poly_offset = 1;
    v.poly_factor = -0.25f;
    v.poly_bias = -1.0f;
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 2, &w);
    CHECK(w.w0[0] == -1.0f && NEAR(w.w0[1], -0.5f));
    nv2a_wdepth_consts_fill(&v, LINES, Z24, 2, &w);
    CHECK(w.w0[0] == 0.0f && w.w0[1] == 0.0f);
    /* ZCLAMP_EN (bits 7:4) 1: clamp instead of cull; bit 0 alone is cull. */
    v.zminmax_ctl = 0x10u;
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 1, &w);
    CHECK(w.w1[1] == 0.0f);
    v.zminmax_ctl = 0x11u;
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 1, &w);
    CHECK(w.w1[1] == 0.0f);
    /* No clip range written yet: the whole of [0, zmax], not [0, 0]. */
    memset(&v, 0, sizeof v);
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z16, 1, &w);
    CHECK(w.w0[2] == 0.0f && w.w0[3] == 65535.0f);
    CHECK(NEAR(w.w1[0], 1.0f / 65535.0f));
    CHECK(w.w1[1] == 1.0f);
    /* Only CLIP_MIN written: the max stays zmax, not 0 (which culls all). */
    memset(&v, 0, sizeof v);
    v.clip_min = 4.0f;
    v.clip_set = 1;
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 1, &w);
    CHECK(w.w0[2] == 4.0f && w.w0[3] == 16777215.0f);
    /* Only CLIP_MAX written: the min stays 0. */
    memset(&v, 0, sizeof v);
    v.clip_min = 9.0f;      /* not written: ignored */
    v.clip_max = 1000.0f;
    v.clip_set = 2;
    nv2a_wdepth_consts_fill(&v, TRIANGLES, Z24, 1, &w);
    CHECK(w.w0[2] == 0.0f && w.w0[3] == 1000.0f);

    printf("nv2a_depth_state: all passed\n");
    return 0;
}
