/*
 * nv2a_vsh_fuzz - the vertex-program decoders against frozen oracles.
 *
 * The CPU interpreter (nv2a_vsh_cpu.c) and the parser the HLSL and MSL
 * translators share (d3d8_vsh_parse.c) both decode through
 * nv2a_vsh_fields.h. Before that each had its own decoder; those versions
 * are kept under oracle/ and every random program must come out the same:
 *
 *   - interpreter: outputs, step count, constant writes and disassembly,
 *     bit for bit, over random programs, constants and inputs;
 *   - parser: the same NV2AVshProgram for random microcode.
 *
 * FUZZ_ITERS overrides the iteration count (default 300000 / 200000).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_vsh_cpu.h"
#include "d3d8_vsh_parse.h"

void oracle_vsh_run(const uint32_t prog[NV2A_VSH_SLOTS][4], uint32_t start,
                    float c[NV2A_VSH_CONSTANTS][4], int const_write,
                    const float v[NV2A_VSH_INPUTS][4], Nv2aVshOut *out);
char *oracle_vsh_disasm(const uint32_t insn[4], char *buf, int size);
void oracle_vsh_parse(const uint32_t *microcode, int num_insns,
                      NV2AVshProgram *program);

static uint64_t s_rng = 88172645463325252ull;
static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 7; s_rng ^= s_rng << 17;
    return (uint32_t)s_rng;
}

/* Mostly small values, with the edge cases a decoder bug would hide in. */
static float rf(void)
{
    uint32_t r = rnd();
    switch (r & 63) {
    case 0: return 0.0f;
    case 1: return -0.0f;
    case 2: return 1e30f;
    default: return (float)((int)(r >> 8) % 8000) / 1000.0f - 4.0f;
    }
}

static long iters(long dflt)
{
    const char *e = getenv("FUZZ_ITERS");
    return e ? atol(e) : dflt;
}

static long fuzz_interp(void)
{
    static uint32_t prog[NV2A_VSH_SLOTS][4];
    static float c1[NV2A_VSH_CONSTANTS][4], c2[NV2A_VSH_CONSTANTS][4];
    float v[NV2A_VSH_INPUTS][4];
    Nv2aVshOut a, b;
    char d1[256], d2[256];
    long it, n_it = iters(300000), bad = 0;

    for (it = 0; it < n_it; it++) {
        int n = 1 + (int)(rnd() % 12), i, k, cw;
        for (i = 0; i < NV2A_VSH_SLOTS; i++)
            for (k = 0; k < 4; k++)
                prog[i][k] = rnd();
        for (i = 0; i < n; i++)
            prog[i][3] &= ~1u;
        prog[n - 1][3] |= 1u;                       /* FINAL */
        for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
            for (k = 0; k < 4; k++)
                c1[i][k] = c2[i][k] = rf();
        for (i = 0; i < NV2A_VSH_INPUTS; i++)
            for (k = 0; k < 4; k++)
                v[i][k] = rf();
        memset(&a, 0x55, sizeof a);
        memset(&b, 0x55, sizeof b);
        cw = (int)(rnd() & 1);
        oracle_vsh_run((const uint32_t (*)[4])prog, 0, c1, cw,
                       (const float (*)[4])v, &a);
        nv2a_vsh_run((const uint32_t (*)[4])prog, 0, c2, cw,
                     (const float (*)[4])v, &b);
        if (memcmp(&a, &b, sizeof a) || memcmp(c1, c2, sizeof c1)) {
            if (bad++ < 5)
                printf("FAIL: interpreter differs, iteration %ld (%d insns)\n", it, n);
        }
        for (i = 0; i < n; i++) {
            oracle_vsh_disasm(prog[i], d1, sizeof d1);
            nv2a_vsh_disasm(prog[i], d2, sizeof d2);
            if (strcmp(d1, d2) && bad++ < 5)
                printf("FAIL: disasm '%s' vs '%s'\n", d1, d2);
        }
    }
    printf("interpreter: %ld programs, %ld mismatches\n", n_it, bad);
    return bad;
}

static long fuzz_parse(void)
{
    static uint32_t code[NV2A_VSH_SLOTS * 4];
    static NV2AVshProgram a, b;
    long it, n_it = iters(200000), bad = 0;

    for (it = 0; it < n_it; it++) {
        int n = 1 + (int)(rnd() % 16), i;
        for (i = 0; i < n * 4; i++)
            code[i] = rnd();
        memset(&a, 0xAB, sizeof a);
        memset(&b, 0xAB, sizeof b);
        oracle_vsh_parse(code, n, &a);
        d3d8_vsh_parse(code, n, &b);
        if (memcmp(&a, &b, sizeof a) && bad++ < 5)
            printf("FAIL: parser differs, iteration %ld (%d insns)\n", it, n);
    }
    printf("parser: %ld programs, %ld mismatches\n", n_it, bad);
    return bad;
}

int main(void)
{
    long bad = fuzz_interp() + fuzz_parse();
    puts(bad ? "nv2a_vsh_fuzz: FAILED" : "nv2a_vsh_fuzz: all passed");
    return bad != 0;
}
