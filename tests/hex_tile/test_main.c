/*
 * hex_tile: nv2a_hex_tile.h's C reference, which the HLSL and MSL texts
 * mirror. What would show on screen if it broke:
 *   - weights that do not sum to 1 darken or brighten the ground;
 *   - a vertex whose weight is not 1 at the vertex and 0 on the far edge
 *     leaves a seam where one triangle meets the next;
 *   - the same point read from two neighbouring triangles must give the
 *     same blend, or the cell edges show;
 *   - a hash outside [0, 1), or not fixed, moves the copies per frame.
 */
#include "nv2a_hex_tile.h"

#include <stdio.h>
#include <string.h>

static int s_fail;

#define CHECK(c, ...) do { if (!(c)) { printf("FAIL: " __VA_ARGS__); printf("\n"); s_fail = 1; } } while (0)

/* The blend at t of a texture whose value at the offset of vertex v is
 * val(v): what the shader returns for a texture that is one constant per
 * copy, the luminance being that constant. */
static float val_of(const int32_t v[2])
{
    float off[2];
    nv2a_hex_offset(v, off);
    return 0.25f + 0.5f * off[0];
}

static float blend_at(float tx, float ty)
{
    float w[3], l[3], W[3];
    int32_t v[3][2];
    int k;
    nv2a_hex_grid(tx, ty, w, v);
    for (k = 0; k < 3; k++)
        l[k] = val_of(v[k]);
    nv2a_hex_weights(w, l, W);
    return W[0] * l[0] + W[1] * l[1] + W[2] * l[2];
}

/* A vertex of the grid, back in t: the skew undone. */
static void vertex_t(const int32_t v[2], float *tx, float *ty)
{
    float ky = (float)v[1], kx = (float)v[0];
    float sy = ky / NV2A_HEX_SKEW2, sx = kx + sy * NV2A_HEX_SKEW;
    *tx = sx / NV2A_HEX_RATE;
    *ty = sy / NV2A_HEX_RATE;
}

int main(void)
{
    int i, j, k;
    float maxjump = 0.0f;

    /* Weights: non-negative, sum 1, over a sweep that crosses many cells
     * and negative coordinates. */
    for (i = -300; i <= 300; i++)
        for (j = -300; j <= 300; j += 7) {
            float w[3];
            int32_t v[3][2];
            float tx = (float)i * 0.0137f, ty = (float)j * 0.0091f;
            nv2a_hex_grid(tx, ty, w, v);
            CHECK(w[0] >= -1e-5f && w[1] >= -1e-5f && w[2] >= -1e-5f,
                  "negative weight at %g %g", tx, ty);
            CHECK(fabsf(w[0] + w[1] + w[2] - 1.0f) < 1e-4f, "weights sum %g at %g %g",
                  w[0] + w[1] + w[2], tx, ty);
            /* three distinct vertices of one triangle: neighbours on the
             * skewed lattice */
            for (k = 0; k < 3; k++) {
                int a = (k + 1) % 3;
                int dx = v[k][0] - v[a][0], dy = v[k][1] - v[a][1];
                CHECK((dx || dy) && dx >= -1 && dx <= 1 && dy >= -1 && dy <= 1,
                      "vertices %d,%d and %d,%d not neighbours", v[k][0], v[k][1],
                      v[a][0], v[a][1]);
            }
        }

    /* At a vertex its weight is 1 and the blend is that copy alone. */
    for (i = -5; i <= 5; i++)
        for (j = -5; j <= 5; j++) {
            int32_t g[2] = { i, j }, v[3][2];
            float tx, ty, w[3];
            int hit = 0;
            vertex_t(g, &tx, &ty);
            /* a hair inside, so floor() picks a cell that holds it */
            nv2a_hex_grid(tx + 1e-5f, ty + 1e-5f, w, v);
            for (k = 0; k < 3; k++)
                if (v[k][0] == i && v[k][1] == j) {
                    CHECK(w[k] > 0.999f, "vertex %d,%d weight %g at itself", i, j, w[k]);
                    hit = 1;
                }
            CHECK(hit, "vertex %d,%d not in the triangle at its own point", i, j);
            CHECK(fabsf(blend_at(tx + 1e-5f, ty + 1e-5f) - val_of(g)) < 1e-3f,
                  "blend at vertex %d,%d is not its copy", i, j);
        }

    /* Continuity: a fine walk along both axes and a diagonal, through
     * every kind of cell edge. The largest step stays small next to the
     * values' spread (0.25 to 0.75). */
    for (k = 0; k < 3; k++) {
        float prev = 0.0f;
        for (i = 0; i < 20000; i++) {
            float s = (float)i * 0.0002f;
            float tx = k == 1 ? 0.123f : s, ty = k == 0 ? 0.321f : s;
            float b = blend_at(tx, ty);
            if (i && fabsf(b - prev) > maxjump)
                maxjump = fabsf(b - prev);
            prev = b;
        }
    }
    CHECK(maxjump < 0.01f, "blend jumps by %g between steps of 0.0002", maxjump);

    /* The hash: fixed bits (the shaders' integer form), in [0, 1). */
    {
        int32_t a[2] = { 0, 0 }, b[2] = { 1, 0 }, c[2] = { -1, 7 };
        float oa[2], ob[2], oc[2], oc2[2];
        nv2a_hex_offset(a, oa);
        nv2a_hex_offset(b, ob);
        nv2a_hex_offset(c, oc);
        nv2a_hex_offset(c, oc2);
        CHECK(oa[0] == 0.0f && oa[1] == 0.0f, "offset of 0,0 is %g %g", oa[0], oa[1]);
        CHECK(memcmp(oc, oc2, sizeof oc) == 0, "offset not fixed");
        CHECK(ob[0] != oc[0] && ob[1] != oc[1], "neighbours share an offset");
        /* vertex 1,0: n = H1 * H1 (mod 2^32), so a change to the
         * constants shows */
        {
            uint32_t n = 1597334673u * 1597334673u;
            CHECK(ob[0] == (float)(n >> 16) / 65536.0f, "offset x bits");
            CHECK(ob[1] == (float)((n * 3812015801u) >> 16) / 65536.0f, "offset y bits");
        }
        for (i = -1000; i <= 1000; i += 13)
            for (j = -1000; j <= 1000; j += 17) {
                int32_t v[2] = { i, j };
                float o[2];
                nv2a_hex_offset(v, o);
                CHECK(o[0] >= 0.0f && o[0] < 1.0f && o[1] >= 0.0f && o[1] < 1.0f,
                      "offset %g %g out of [0,1)", o[0], o[1]);
            }
    }

    /* The blend weights: normalised, the luminance lifting a bright copy. */
    {
        float w[3] = { 0.5f, 0.3f, 0.2f }, flat[3] = { 0.5f, 0.5f, 0.5f };
        float bright[3] = { 0.1f, 1.0f, 0.1f }, W[3], Wb[3];
        nv2a_hex_weights(w, flat, W);
        CHECK(fabsf(W[0] + W[1] + W[2] - 1.0f) < 1e-5f, "blend weights sum %g",
              W[0] + W[1] + W[2]);
        CHECK(W[0] > W[1] && W[1] > W[2], "w^7 order lost");
        nv2a_hex_weights(w, bright, Wb);
        CHECK(Wb[1] > W[1], "a bright copy does not gain");
    }

    /* The shader texts carry the reference's constants. */
    {
        static const char *const texts[2] = { D3D8_HLSL_HEX, D3D8_MSL_HEX };
        for (k = 0; k < 2; k++) {
            CHECK(strstr(texts[k], "t * 3.46410162;") != NULL, "text %d: rate", k);
            CHECK(strstr(texts[k], "k.x - k.y * 0.57735027, k.y * 1.15470054") != NULL,
                  "text %d: skew", k);
            CHECK(strstr(texts[k], "uint2(1597334673u, 3812015801u)") != NULL
                  && strstr(texts[k], "(q.x ^ q.y) * 1597334673u") != NULL
                  && strstr(texts[k], "(n * 3812015801u) >> 16") != NULL, "text %d: hash", k);
            CHECK(strstr(texts[k], "w2 * w2 * w2 * w *") != NULL
                  && strstr(texts[k], "l, 0.6)") != NULL, "text %d: blend", k);
        }
    }

    /* Only wrap on both axes samples hex-tiled: clamp (3, 5), mirror (2)
     * and border (4) keep the stock read; the high bits are not the mode. */
    {
        uint32_t m;
        CHECK(nv2a_hex_wrap_ok(1u, 1u) && nv2a_hex_wrap_ok(0x11u, 0x101u), "wrap refused");
        for (m = 0; m < 16; m++)
            if (m != 1u)
                CHECK(!nv2a_hex_wrap_ok(m, 1u) && !nv2a_hex_wrap_ok(1u, m),
                      "address mode %u passes", m);
    }

    printf("hex_tile: largest step %g\n", maxjump);
    if (s_fail)
        return 1;
    printf("hex_tile: ok\n");
    return 0;
}
