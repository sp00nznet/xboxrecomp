/* Differential checks against the general interpreter, plus an optional
 * --benchmark mode. No golden images or game data are required. */
#include "nv2a_combiner.h"
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

void nv2a_rc_eval_generic(const Nv2aCombiner *, const float[4],
                          const float[4], const float[4],
                          const float[4][4], float[4]);

typedef struct {
    float v0[4], v1[4], fog[4], t[4][4];
} Pixel;

static uint32_t state = 0x31C7A159u;
static uint32_t random_u32(void)
{
    state ^= state << 13; state ^= state >> 17; state ^= state << 5;
    return state;
}

static float random_float(void)
{
    /* Include undershoot and overshoot from interpolation and fog. */
    return (float)(random_u32() & 0xFFFFFFu) / 4194304.0f - 1.0f;
}

static void fill_pixel(Pixel *p)
{
    size_t i, stage;
    for (i = 0; i < 4; i++) {
        p->v0[i] = random_float();
        p->v1[i] = random_float();
        p->fog[i] = random_float();
        for (stage = 0; stage < 4; stage++)
            p->t[stage][i] = random_float();
    }
}

static Nv2aCombiner program(uint32_t final0, int copy)
{
    Nv2aCombiner rc;
    memset(&rc, 0, sizeof rc);
    rc.control = rc.stage_program = 1;
    rc.color_icw[0] = copy ? 0x08200000u : 0x04080000u;
    rc.alpha_icw[0] = copy ? 0x14200000u : 0x14180000u;
    rc.color_ocw[0] = rc.alpha_ocw[0] = 0xC00;
    rc.final0 = final0;
    rc.final1 = 0x1C80;
    return rc;
}

static int compare(const Nv2aCombiner *rc, const Pixel *p)
{
    float got[4], want[4];
    int i;
    nv2a_rc_eval(rc, p->v0, p->v1, p->fog, p->t, got);
    nv2a_rc_eval_generic(rc, p->v0, p->v1, p->fog, p->t, want);
    for (i = 0; i < 4; i++) {
        if (memcmp(&got[i], &want[i], sizeof(float)) != 0 &&
            !(isnan(got[i]) && isnan(want[i]))) {
            printf("Mismatch color=%08X final=%08X channel=%d: %.9g != %.9g\n",
                   rc->color_icw[0], rc->final0, i, got[i], want[i]);
            return 1;
        }
    }
    return 0;
}

static int equivalence(void)
{
    static const float edge[] = {
        -INFINITY, -3.0f, -1.0f, -0.0f, 0.0f, 1.0f / 255.0f,
        0.5f, 1.0f, 1.0001f, 3.0f, INFINITY, NAN
    };
    uint32_t finals[] = { 0x130C0300u, 0x0000000Cu, 0x0000000Cu };
    size_t f, a, b, c, i;
    unsigned cases = 0;
    for (f = 0; f < 3; f++) {
        Nv2aCombiner rc = program(finals[f], f == 2);
        Pixel p;
        for (i = 0; i < 100000; i++) {
            fill_pixel(&p);
            /* Dead constants and stages must not change the shortcut. */
            rc.final_c0 = random_u32(); rc.final_c1 = random_u32();
            rc.factor0[0] = random_u32(); rc.factor1[0] = random_u32();
            rc.color_icw[7] = random_u32();
            if (compare(&rc, &p)) return 1;
            cases++;
        }
        for (a = 0; a < sizeof(edge) / sizeof(edge[0]); a++)
            for (b = 0; b < sizeof(edge) / sizeof(edge[0]); b++)
                for (c = 0; c < sizeof(edge) / sizeof(edge[0]); c++) {
                    fill_pixel(&p);
                    for (i = 0; i < 4; i++) {
                        p.v0[i] = edge[a]; p.t[0][i] = edge[b];
                        p.fog[i] = edge[c];
                    }
                    if (compare(&rc, &p)) return 1;
                    cases++;
                }
        /* Change every bit of every instruction in the guard. Each
         * variation must fall back to exactly the interpreter's behavior. */
        for (a = 0; a < 8; a++) {
            unsigned bit;
            for (bit = 0; bit < 32; bit++) {
                Nv2aCombiner mutated = rc;
                uint32_t *field[] = {
                    &mutated.control, &mutated.stage_program,
                    &mutated.color_icw[0], &mutated.alpha_icw[0],
                    &mutated.color_ocw[0], &mutated.alpha_ocw[0],
                    &mutated.final0, &mutated.final1
                };
                *field[a] ^= 1u << bit;
                for (i = 0; i < 32; i++) {
                    fill_pixel(&p);
                    if (compare(&mutated, &p)) return 1;
                    cases++;
                }
            }
        }
    }
    printf("ok: %u bitwise differential cases (NaN payloads ignored)\n", cases);
    return 0;
}

static int benchmark(void)
{
    enum { COUNT = 1024, ITERATIONS = 40000000 };
    Pixel pixels[COUNT];
    uint32_t finals[] = { 0x130C0300u, 0x0000000Cu, 0x0000000Cu };
    volatile float checksum = 0;
    int i, f, mode;
    for (i = 0; i < COUNT; i++) fill_pixel(&pixels[i]);
    for (f = 0; f < 3; f++) {
        Nv2aCombiner rc = program(finals[f], f == 2);
        double elapsed[2];
        for (mode = 0; mode < 2; mode++) {
            clock_t start = clock();
            for (i = 0; i < ITERATIONS; i++) {
                const Pixel *p = &pixels[i & (COUNT - 1)];
                float out[4];
                if (mode == 0)
                    nv2a_rc_eval_generic(&rc, p->v0, p->v1, p->fog, p->t, out);
                else
                    nv2a_rc_eval(&rc, p->v0, p->v1, p->fog, p->t, out);
                checksum += out[i & 3];
            }
            elapsed[mode] = (double)(clock() - start) / CLOCKS_PER_SEC;
        }
        printf("color=%08X final=%08X generic=%.3fs fast=%.3fs speedup=%.2fx\n",
               rc.color_icw[0], finals[f], elapsed[0], elapsed[1], elapsed[0] / elapsed[1]);
    }
    printf("checksum=%g\n", (double)checksum);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--benchmark") == 0)
        return benchmark();
    return equivalence();
}
