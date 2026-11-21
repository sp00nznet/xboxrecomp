/**
 * NV2A register combiners. See nv2a_combiner.h.
 *
 * Register numbers (an input or output byte's low nibble):
 *   0 zero/discard  1 c0  2 c1  3 fog  4 v0  5 v1  8-11 t0-t3  12 r0  13 r1
 *   14 v1+r0 sum and 15 E*F product (final combiner only)
 * An input byte is reg | alpha<<4 | mapping<<5. In the RGB portion the alpha
 * bit replicates .a; in the alpha portion it picks .a over .b.
 */
#include "nv2a_combiner.h"

#include <string.h>

/* The rasteriser evaluates pixels on several threads. */
#if defined(_MSC_VER)
#define NV_RC_TLS __declspec(thread)
#else
#define NV_RC_TLS __thread
#endif

enum { R_ZERO = 0, R_C0 = 1, R_C1 = 2, R_FOG = 3, R_V0 = 4, R_V1 = 5,
       R_T0 = 8, R_R0 = 12, R_R1 = 13, R_SUM = 14, R_EF = 15 };

void nv2a_rc_unpack(uint32_t c, float o[4])
{
    o[0] = (float)((c >> 16) & 0xFF) / 255.0f;
    o[1] = (float)((c >>  8) & 0xFF) / 255.0f;
    o[2] = (float)( c        & 0xFF) / 255.0f;
    o[3] = (float)( c >> 24        ) / 255.0f;
}

/* D3DCOLOR to floats, memoised: a program's constants are the same for
 * every pixel of a batch, and unpacking them per pixel was a measurable
 * share of the executor. */
static const float *unpack_cached(uint32_t c)
{
    static NV_RC_TLS uint32_t key[16];
    static NV_RC_TLS float val[16][4];
    static NV_RC_TLS int valid[16];
    unsigned slot = (c ^ (c >> 8) ^ (c >> 16) ^ (c >> 24)) & 15u;
    if (!valid[slot] || key[slot] != c) {
        nv2a_rc_unpack(c, val[slot]);
        key[slot] = c;
        valid[slot] = 1;
    }
    return val[slot];
}

static float clampf(float x, float lo, float hi)
{
    return x < lo ? lo : (x > hi ? hi : x);
}

/* PS_INPUTMAPPING, psh.c get_input_var. */
static float map_in(float x, uint32_t byte)
{
    float p = x < 0.0f ? 0.0f : x;
    switch ((byte >> 5) & 7) {
    case 0:  return p;                               /* unsigned identity */
    case 1:  return 1.0f - clampf(x, 0.0f, 1.0f);    /* unsigned invert   */
    case 2:  return 2.0f * p - 1.0f;                 /* expand normal     */
    case 3:  return -2.0f * p + 1.0f;                /* expand negate     */
    case 4:  return p - 0.5f;                        /* half-bias normal  */
    case 5:  return -p + 0.5f;                       /* half-bias negate  */
    case 6:  return x;                               /* signed identity   */
    default: return -x;                              /* signed negate     */
    }
}

/* PS_COMBINEROUTPUT scale/bias, psh.c get_output. */
static float map_out(float x, uint32_t mapping)
{
    switch (mapping) {
    case 0x08: return x - 0.5f;
    case 0x10: return x * 2.0f;
    case 0x18: return (x - 0.5f) * 2.0f;
    case 0x20: return x * 4.0f;
    case 0x30: return x * 0.5f;
    default:   return x;
    }
}

static void read_rgb(float R[16][4], uint32_t byte, float v[3])
{
    const float *r = R[byte & 0xF];
    int i;
    for (i = 0; i < 3; i++)
        v[i] = map_in((byte & 0x10) ? r[3] : r[i], byte);
}

static float read_alpha(float R[16][4], uint32_t byte)
{
    const float *r = R[byte & 0xF];
    return map_in((byte & 0x10) ? r[3] : r[2], byte);
}

static void eval_generic(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4])
{
    float R[16][4];
    uint32_t n = rc->control & 0xFF, flags = rc->control >> 8, s;
    int i;

    if (n > 8)
        n = 8;
    /* Only the registers a program can read before writing need a value:
     * zero, r0, r1 and the final-combiner specials. */
    for (i = 0; i < 4; i++) {
        R[0][i] = R[6][i] = R[7][i] = 0.0f;
        R[12][i] = R[13][i] = R[14][i] = R[15][i] = 0.0f;
    }
    for (i = 0; i < 4; i++) {
        R[R_FOG][i] = fog[i];
        R[R_V0][i] = v0[i];
        R[R_V1][i] = v1[i];
        R[R_T0 + 0][i] = t[0][i];
        R[R_T0 + 1][i] = t[1][i];
        R[R_T0 + 2][i] = t[2][i];
        R[R_T0 + 3][i] = t[3][i];
    }
    /* r0.a starts as texture 0's alpha, or 1 with stage 0 off (psh.c). */
    R[R_R0][3] = (rc->stage_program & 0x1F) ? t[0][3] : 1.0f;

    for (s = 0; s < n; s++) {
        uint32_t icw = rc->color_icw[s], ocw = rc->color_ocw[s];
        uint32_t aicw = rc->alpha_icw[s], aocw = rc->alpha_ocw[s];
        uint32_t fl = ocw >> 12, afl = aocw >> 12;
        float A[3], B[3], C[3], D[3], ab[3], cd[3], ms[3];
        float aA, aB, aC, aD, aab, acd, ams;
        int mux_cd;

        /* One c0/c1 for every stage, or one per stage (COMBINERCOUNT). */
        memcpy(R[R_C0], unpack_cached(rc->factor0[(flags & 0x010) ? s : 0]), 16);
        memcpy(R[R_C1], unpack_cached(rc->factor1[(flags & 0x100) ? s : 0]), 16);
        mux_cd = (flags & 1) ? R[R_R0][3] >= 0.5f
                             : ((int)(R[R_R0][3] * 255.0f) & 1);

        /* Both portions read before either writes (psh.c emits all the
         * arithmetic, then the assignments). */
        read_rgb(R, icw >> 24, A); read_rgb(R, icw >> 16, B);
        read_rgb(R, icw >> 8, C);  read_rgb(R, icw, D);
        aA = read_alpha(R, aicw >> 24); aB = read_alpha(R, aicw >> 16);
        aC = read_alpha(R, aicw >> 8);  aD = read_alpha(R, aicw);

        for (i = 0; i < 3; i++) {
            float dab = A[0] * B[0] + A[1] * B[1] + A[2] * B[2];
            float dcd = C[0] * D[0] + C[1] * D[1] + C[2] * D[2];
            float pab = (fl & 2) ? dab : A[i] * B[i];
            float pcd = (fl & 1) ? dcd : C[i] * D[i];
            float sum = (fl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            ab[i] = clampf(map_out(pab, fl & 0x38), -1.0f, 1.0f);
            cd[i] = clampf(map_out(pcd, fl & 0x38), -1.0f, 1.0f);
            ms[i] = clampf(map_out(sum, fl & 0x38), -1.0f, 1.0f);
        }
        {
            float pab = aA * aB, pcd = aC * aD;
            float sum = (afl & 4) ? (mux_cd ? pcd : pab) : pab + pcd;
            aab = clampf(map_out(pab, afl & 0x38), -1.0f, 1.0f);
            acd = clampf(map_out(pcd, afl & 0x38), -1.0f, 1.0f);
            ams = clampf(map_out(sum, afl & 0x38), -1.0f, 1.0f);
        }

        /* RGB destinations; blue-to-alpha also writes the destination's .a. */
        if ((ocw >> 4) & 0xF) {
            float *d = R[(ocw >> 4) & 0xF];
            d[0] = ab[0]; d[1] = ab[1]; d[2] = ab[2];
            if (fl & 0x80) d[3] = ab[2];
        }
        if (ocw & 0xF) {
            float *d = R[ocw & 0xF];
            d[0] = cd[0]; d[1] = cd[1]; d[2] = cd[2];
            if (fl & 0x40) d[3] = cd[2];
        }
        if ((ocw >> 8) & 0xF) {
            float *d = R[(ocw >> 8) & 0xF];
            d[0] = ms[0]; d[1] = ms[1]; d[2] = ms[2];
        }
        if ((aocw >> 4) & 0xF) R[(aocw >> 4) & 0xF][3] = aab;
        if (aocw & 0xF)        R[aocw & 0xF][3] = acd;
        if ((aocw >> 8) & 0xF) R[(aocw >> 8) & 0xF][3] = ams;
        /* Register 0 is zero whatever was "written" to it. */
        R[R_ZERO][0] = R[R_ZERO][1] = R[R_ZERO][2] = R[R_ZERO][3] = 0.0f;
    }

    if (rc->final0 || rc->final1) {
        uint32_t f0 = rc->final0, f1 = rc->final1, fflags = f1 & 0xFF;
        float A[3], B[3], C[3], D[3], E[3], F[3];

        memcpy(R[R_C0], unpack_cached(rc->final_c0), 16);
        memcpy(R[R_C1], unpack_cached(rc->final_c1), 16);
        /* V1R0 sum: optional complements (0x40 v1, 0x20 r0), clamp (0x80). */
        for (i = 0; i < 3; i++) {
            float a = (fflags & 0x40) ? 1.0f - R[R_V1][i] : R[R_V1][i];
            float b = (fflags & 0x20) ? 1.0f - R[R_R0][i] : R[R_R0][i];
            R[R_SUM][i] = (fflags & 0x80) ? clampf(a + b, 0.0f, 1.0f) : a + b;
        }
        R[R_SUM][3] = 0.0f;
        read_rgb(R, f1 >> 24, E);
        read_rgb(R, f1 >> 16, F);
        for (i = 0; i < 3; i++)
            R[R_EF][i] = E[i] * F[i];
        R[R_EF][3] = 0.0f;
        read_rgb(R, f0 >> 24, A); read_rgb(R, f0 >> 16, B);
        read_rgb(R, f0 >> 8, C);  read_rgb(R, f0, D);
        /* D + mix(C, B, A), alpha from G. */
        for (i = 0; i < 3; i++)
            out[i] = D[i] + C[i] * (1.0f - A[i]) + B[i] * A[i];
        out[3] = read_alpha(R, f1 >> 8);
    } else {
        for (i = 0; i < 4; i++)
            out[i] = R[R_R0][i];
    }
    for (i = 0; i < 4; i++)
        out[i] = clampf(out[i], 0.0f, 1.0f);
}

/* The usual fixed-function MODULATE and texture-copy programs have no
 * constants, mux, dot products or temporary-register dependencies. The copy
 * program takes RGB from texture 0 and alpha from the diffuse vertex colour.
 * Evaluating them directly
 * avoids constructing the register file and interpreting twelve inputs
 * for every shaded pixel. Guard every instruction that affects its result;
 * all other programs retain the general interpreter above.
 *
 * Keep the interpreter's unsigned input mapping, stage saturation and final
 * arithmetic order, including for interpolants outside [0,1]. Fog.a is an
 * unsigned input, not a saturated interpolation factor. */
void nv2a_rc_eval(const Nv2aCombiner *rc, const float v0[4],
                  const float v1[4], const float fog[4],
                  const float t[4][4], float out[4])
{
    if (rc->control == 1 && rc->stage_program == 1 &&
        ((rc->color_icw[0] == 0x04080000u &&
          rc->alpha_icw[0] == 0x14180000u) ||
         (rc->color_icw[0] == 0x08200000u &&
          rc->alpha_icw[0] == 0x14200000u &&
          rc->final0 == 0x0000000Cu)) &&
        rc->color_ocw[0] == 0x00000C00u &&
        rc->alpha_ocw[0] == 0x00000C00u &&
        rc->final1 == 0x00001C80u &&
        (rc->final0 == 0x130C0300u || rc->final0 == 0x0000000Cu)) {
        float r0[4];
        int copy = rc->color_icw[0] == 0x08200000u;
        int i;
        for (i = 0; i < 4; i++) {
            float av = copy ? (i == 3 ? v0[i] : t[0][i]) : v0[i];
            float bv = copy ? 1.0f : t[0][i];
            float a = av < 0.0f ? 0.0f : av;
            float b = bv < 0.0f ? 0.0f : bv;
            r0[i] = clampf(a * b + 0.0f, -1.0f, 1.0f);
        }
        if (rc->final0 == 0x130C0300u) {
            float a = fog[3] < 0.0f ? 0.0f : fog[3];
            for (i = 0; i < 3; i++) {
                float b = r0[i] < 0.0f ? 0.0f : r0[i];
                float c = fog[i] < 0.0f ? 0.0f : fog[i];
                out[i] = clampf(0.0f + c * (1.0f - a) + b * a,
                                0.0f, 1.0f);
            }
        } else {
            for (i = 0; i < 3; i++)
                out[i] = clampf(r0[i] + 0.0f + 0.0f, 0.0f, 1.0f);
        }
        out[3] = clampf(r0[3], 0.0f, 1.0f);
        return;
    }
    eval_generic(rc, v0, v1, fog, t, out);
}

#ifdef NV2A_RC_TEST
/* Independent interpreter entry for differential tests and microbenchmarks;
 * it is absent from production builds. */
void nv2a_rc_eval_generic(const Nv2aCombiner *rc, const float v0[4],
                          const float v1[4], const float fog[4],
                          const float t[4][4], float out[4])
{
    eval_generic(rc, v0, v1, fog, t, out);
}
#endif
