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
 *   - parser: the same NV2AVshProgram for random microcode;
 *   - the oPos slice (nv2a_vsh_slice_pos): oPos and ok as the whole
 *     program's, bit for bit;
 *   - the slice cache (nv2a_vsh_pos_prog): the uncached slice, through
 *     same-set collisions and in-place rewrites.
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

/* nv2a_vsh_slice_pos: oPos and ok of the slice against the whole program,
 * bit for bit. Random words with the register fields pulled into R0..R12
 * most of the time, so the programs chain through temps, R12 (oPos) and
 * a0 the way real ones do rather than reading nothing they wrote. */
static long fuzz_slice(void)
{
    static uint32_t prog[NV2A_VSH_SLOTS][4], pos[NV2A_VSH_SLOTS][4];
    static float c[NV2A_VSH_CONSTANTS][4];
    float v[NV2A_VSH_INPUTS][4];
    Nv2aVshOut a, b;
    long it, n_it = iters(300000), bad = 0, kept = 0, total = 0;

    for (it = 0; it < n_it; it++) {
        uint32_t start = rnd() % 4, n = 1 + rnd() % 40, i, k, np;
        for (i = 0; i < NV2A_VSH_SLOTS; i++)
            for (k = 0; k < 4; k++)
                prog[i][k] = rnd();
        for (i = start; i < start + n; i++) {
            uint32_t *w = prog[i];
            if (rnd() % 4) {    /* OUT_R into R0..R12 */
                w[3] &= ~(15u << 20);
                w[3] |= (rnd() % 13) << 20;
            }
            if (rnd() % 4) {    /* sources A and B from R0..R12 */
                w[2] &= ~((15u << 28) | (15u << 13));
                w[2] |= (rnd() % 13) << 28 | (rnd() % 13) << 13;
            }
            if (rnd() % 4) {    /* muxes from R or C: chains and a0 reads */
                w[2] &= ~((3u << 26) | (3u << 11));
                w[2] |= (rnd() % 2 ? 1u : 3u) << 26 | (rnd() % 2 ? 1u : 3u) << 11;
                w[3] &= ~(3u << 28);
                w[3] |= (rnd() % 2 ? 1u : 3u) << 28;
            }
            if (rnd() % 8 == 0) {   /* ARL, so a0 is written and then read */
                w[1] &= ~(15u << 21);
                w[1] |= 13u << 21;
            }
            if (rnd() % 3 == 0) {   /* the output into oPos */
                w[3] &= ~(255u << 3);
            }
            w[3] &= ~1u;
        }
        prog[start + n - 1][3] |= 1u;               /* FINAL */
        for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
            for (k = 0; k < 4; k++)
                c[i][k] = rf();
        for (i = 0; i < NV2A_VSH_INPUTS; i++)
            for (k = 0; k < 4; k++)
                v[i][k] = rf();
        np = nv2a_vsh_slice_pos((const uint32_t (*)[4])prog, start, pos);
        nv2a_vsh_run((const uint32_t (*)[4])prog, start, c, 0, (const float (*)[4])v, &a);
        nv2a_vsh_run((const uint32_t (*)[4])pos, 0, c, 0, (const float (*)[4])v, &b);
        kept += np;
        total += n;
        if (!np || np > n || memcmp(pos[np - 1], prog[start + n - 1], 16) || a.ok != b.ok
            || memcmp(a.o[NV2A_VSH_O_POS], b.o[NV2A_VSH_O_POS], 16)) {
            if (bad++ < 5)
                printf("FAIL: slice differs, iteration %ld (%u insns, %u kept)\n", it, n, np);
        }
    }
    printf("slice: %ld programs, %ld mismatches, %.1f%% of instructions kept\n", n_it, bad,
           total ? 100.0 * (double)kept / (double)total : 0.0);
    return bad;
}

/* nv2a_vsh_pos_prog against an uncached slice. The programs share their
 * length and their first two and last slots, as XDK programs share the
 * prologue and epilogue, so many meet in one set; middle words are
 * rewritten in place now and then, which a hit must never miss. */
static long fuzz_cache(void)
{
#define CP 24
#define CL 10
    static uint32_t pool[CP][NV2A_VSH_SLOTS][4], pos[NV2A_VSH_SLOTS][4];
    static float c[NV2A_VSH_CONSTANTS][4];
    float v[NV2A_VSH_INPUTS][4];
    uint32_t base[3][4], st0[CP];
    long it, n_it = iters(300000), bad = 0;
    int p, i, k;

    nv2a_vsh_slice_clear();
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++)
        for (k = 0; k < 4; k++)
            c[i][k] = rf();
    for (i = 0; i < NV2A_VSH_INPUTS; i++)
        for (k = 0; k < 4; k++)
            v[i][k] = rf();
    for (i = 0; i < 3; i++)
        for (k = 0; k < 4; k++)
            base[i][k] = rnd();
    for (p = 0; p < CP; p++) {
        st0[p] = (uint32_t)p % 3;
        for (i = 0; i < NV2A_VSH_SLOTS; i++)
            for (k = 0; k < 4; k++)
                pool[p][i][k] = rnd();
        memcpy(pool[p][st0[p]], base[0], 16);
        memcpy(pool[p][st0[p] + 1], base[1], 16);
        memcpy(pool[p][st0[p] + CL - 1], base[2], 16);
        for (i = 0; i < CL; i++) {
            uint32_t *w = pool[p][st0[p] + i];
            w[3] &= ~1u;
            if (i >= 2 && i < CL - 1 && (rnd() & 1))
                w[3] &= ~(255u << 3);           /* into oPos */
        }
        pool[p][st0[p] + CL - 1][3] |= 1u;
    }
    for (it = 0; it < n_it; it++) {
        const uint32_t (*pp)[4];
        Nv2aVshOut a, b;
        uint32_t st, np;
        p = (int)(rnd() % CP);
        if (rnd() % 50 == 0)
            pool[p][st0[p] + 2 + rnd() % (CL - 3)][rnd() % 3] = rnd();
        st = st0[p];
        pp = nv2a_vsh_pos_prog((const uint32_t (*)[4])pool[p], &st);
        np = nv2a_vsh_slice_pos((const uint32_t (*)[4])pool[p], st0[p], pos);
        nv2a_vsh_run(pp, st, c, 0, (const float (*)[4])v, &b);
        nv2a_vsh_run((const uint32_t (*)[4])pos, 0, c, 0, (const float (*)[4])v, &a);
        if (st != 0 || memcmp(pp, pos, (size_t)np * 16u) || a.ok != b.ok
            || memcmp(a.o[NV2A_VSH_O_POS], b.o[NV2A_VSH_O_POS], 16)) {
            if (bad++ < 5)
                printf("FAIL: cached slice differs, iteration %ld (program %d)\n", it, p);
        }
    }
    printf("cache: %ld lookups, %ld mismatches, %llu hits, %llu misses\n", n_it, bad,
           (unsigned long long)nv2a_vsh_slice_hits, (unsigned long long)nv2a_vsh_slice_misses);
    return bad;
#undef CP
#undef CL
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
    long bad = fuzz_interp() + fuzz_slice() + fuzz_cache() + fuzz_parse();
    puts(bad ? "nv2a_vsh_fuzz: FAILED" : "nv2a_vsh_fuzz: all passed");
    return bad != 0;
}
