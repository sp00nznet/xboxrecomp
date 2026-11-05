/*
 * nv2a_vsh - the CPU vertex program interpreter against hand-encoded programs.
 *
 * The encoder below writes fields at the positions xemu's vsh.c reads them
 * from, independently of the interpreter's decoder, so a field that the
 * decoder reads from the wrong bits fails here rather than as a scrambled
 * frame. The programs are the shapes XDK D3D actually emits: a 4x4 transform
 * by dp4 against four constants, then the viewport epilogue XDK appends to
 * every program (mul/rcc/mad against c[58] and c[59]).
 *
 *   cc -I src/kernel tests/nv2a_vsh/test_main.c src/kernel/nv2a_vsh_cpu.c -lm
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_vsh_cpu.h"

static int failures;
#define CHECK_F(name, got, want) \
    do { float g_ = (got), w_ = (want); \
         if (fabsf(g_ - w_) <= 1e-4f * (1.0f + fabsf(w_))) {} else { \
            printf("FAIL: %s (got %g, want %g)\n", name, g_, w_); failures++; } \
    } while (0)

static void put(uint32_t *w, int word, int shift, int bits, uint32_t v)
{
    w[word] &= ~(((1u << bits) - 1u) << shift);
    w[word] |= (v & ((1u << bits) - 1u)) << shift;
}

enum { R = 1, V = 2, C = 3 };
#define SWZ_XYZW 0x1B   /* x=0 y=1 z=2 w=3, two bits each from the top */
#define SWZ_X    0x00
#define SWZ_W    0xFF

static void src(uint32_t *w, int which, int mux, int reg, int swz, int neg)
{
    int sx = (swz >> 6) & 3, sy = (swz >> 4) & 3, sz = (swz >> 2) & 3, sw = swz & 3;
    switch (which) {
    case 0:
        put(w, 1, 8, 1, neg); put(w, 1, 6, 2, sx); put(w, 1, 4, 2, sy);
        put(w, 1, 2, 2, sz); put(w, 1, 0, 2, sw);
        put(w, 2, 26, 2, mux);
        if (mux == R) put(w, 2, 28, 4, reg);
        break;
    case 1:
        put(w, 2, 25, 1, neg); put(w, 2, 23, 2, sx); put(w, 2, 21, 2, sy);
        put(w, 2, 19, 2, sz); put(w, 2, 17, 2, sw);
        put(w, 2, 11, 2, mux);
        if (mux == R) put(w, 2, 13, 4, reg);
        break;
    default:
        put(w, 2, 10, 1, neg); put(w, 2, 8, 2, sx); put(w, 2, 6, 2, sy);
        put(w, 2, 4, 2, sz); put(w, 2, 2, 2, sw);
        put(w, 3, 28, 2, mux);
        if (mux == R) { put(w, 2, 0, 2, reg >> 2); put(w, 3, 30, 2, reg & 3); }
        break;
    }
    if (mux == V) put(w, 1, 9, 4, reg);
    if (mux == C) put(w, 1, 13, 8, reg);
}

/* mac op, temp dest (mask 0 = none), output dest (orb=1 -> o reg) */
static void mac(uint32_t *w, int op, int r, int rmask, int oaddr, int omask)
{
    put(w, 1, 21, 4, op);
    put(w, 3, 20, 4, r); put(w, 3, 24, 4, rmask);
    if (omask) {
        put(w, 3, 12, 4, omask); put(w, 3, 11, 1, 1);
        put(w, 3, 3, 8, oaddr); put(w, 3, 2, 1, 0);
    }
}

static void ilu(uint32_t *w, int op, int r, int rmask)
{
    put(w, 1, 25, 3, op);
    put(w, 3, 20, 4, r); put(w, 3, 16, 4, rmask);
}

static uint32_t prog[NV2A_VSH_SLOTS][4];
static float cst[NV2A_VSH_CONSTANTS][4];
static float in[NV2A_VSH_INPUTS][4];

int main(void)
{
    Nv2aVshOut o;
    char buf[128];
    int i, n = 0;

    /* dp4 R12.x/y/z/w, v0, c[96..99]: a projection that scales x by 2, y by 3,
     * leaves z, and puts z into w (so w = 4 for z = 4). */
    for (i = 0; i < 4; i++) {
        memset(prog[n], 0, 16);
        src(prog[n], 0, V, 0, SWZ_XYZW, 0);
        src(prog[n], 1, C, 96 + i, SWZ_XYZW, 0);
        mac(prog[n], 7 /*dp4*/, 12, 8 >> i, 0, 0);
        n++;
    }
    /* mov oD0, v3 and mov oT0, v9 */
    memset(prog[n], 0, 16);
    src(prog[n], 0, V, 3, SWZ_XYZW, 0);
    mac(prog[n], 1, 0, 0, 3 /*oD0*/, 0xF);
    n++;
    memset(prog[n], 0, 16);
    src(prog[n], 0, V, 9, SWZ_XYZW, 0);
    mac(prog[n], 1, 0, 0, 9 /*oT0*/, 0xF);
    n++;
    /* XDK epilogue */
    memset(prog[n], 0, 16);                    /* mul oPos.xyz, R12, c58 + rcc R1.x, R12.w */
    src(prog[n], 0, R, 12, SWZ_XYZW, 0);
    src(prog[n], 1, C, 58, SWZ_XYZW, 0);
    src(prog[n], 2, R, 12, SWZ_W, 0);
    mac(prog[n], 2, 0, 0, 0, 0xE);
    ilu(prog[n], 3 /*rcc*/, 0, 0x8);
    n++;
    memset(prog[n], 0, 16);                    /* mad oPos.xyz, R12, R1.x, c59 */
    src(prog[n], 0, R, 12, SWZ_XYZW, 0);
    src(prog[n], 1, R, 1, SWZ_X, 0);
    src(prog[n], 2, C, 59, SWZ_XYZW, 0);
    mac(prog[n], 4, 0, 0, 0, 0xE);
    put(prog[n], 3, 0, 1, 1);                  /* final */
    n++;

    memset(cst, 0, sizeof cst);
    cst[96][0] = 2;                             /* row x */
    cst[97][1] = 3;                             /* row y */
    cst[98][2] = 1;                             /* row z */
    cst[99][2] = 1;                             /* row w = z */
    cst[58][0] = 320; cst[58][1] = -240; cst[58][2] = 16777215.0f;
    cst[59][0] = 320; cst[59][1] = 240;  cst[59][2] = 0;

    memset(in, 0, sizeof in);
    in[0][0] = 1; in[0][1] = 2; in[0][2] = 4; in[0][3] = 1;
    in[3][0] = 0.25f; in[3][1] = 0.5f; in[3][2] = 0.75f; in[3][3] = 1;
    in[9][0] = 0.125f; in[9][1] = 0.875f;

    for (i = 0; i < n; i++)
        printf("  %2d: %08X %08X %08X  %s\n", i, prog[i][1], prog[i][2], prog[i][3],
               nv2a_vsh_disasm(prog[i], buf, sizeof buf));

    nv2a_vsh_run((const uint32_t (*)[4])prog, 0, cst, 0,
                 (const float (*)[4])in, &o);
    if (!o.ok) { printf("FAIL: no final instruction reached\n"); failures++; }
    if (o.steps != n) { printf("FAIL: steps %d want %d\n", o.steps, n); failures++; }
    /* clip = (2, 6, 4, 4); screen = clip.xyz/w * scale + offset */
    CHECK_F("oPos.x", o.o[NV2A_VSH_O_POS][0], 2.0f / 4 * 320 + 320);
    CHECK_F("oPos.y", o.o[NV2A_VSH_O_POS][1], 6.0f / 4 * -240 + 240);
    CHECK_F("oPos.z", o.o[NV2A_VSH_O_POS][2], 4.0f / 4 * 16777215.0f);
    CHECK_F("oPos.w", o.o[NV2A_VSH_O_POS][3], 4.0f);
    CHECK_F("oD0.y", o.o[NV2A_VSH_O_D0][1], 0.5f);
    CHECK_F("oD0.w", o.o[NV2A_VSH_O_D0][3], 1.0f);
    CHECK_F("oT0.x", o.o[NV2A_VSH_O_T0][0], 0.125f);
    CHECK_F("oT0.y", o.o[NV2A_VSH_O_T0][1], 0.875f);

    /* ARL + relative constant read, negate and swizzle. */
    memset(prog, 0, sizeof prog);
    n = 0;
    src(prog[n], 0, V, 1, SWZ_X, 0);           /* arl a0, v1.x */
    put(prog[n], 1, 21, 4, 13);
    n++;
    src(prog[n], 0, C, 10, 0xE4 /* wzyx */, 1); /* mov oT1, -c[a0+10].wzyx */
    put(prog[n], 3, 1, 1, 1);
    mac(prog[n], 1, 0, 0, 10, 0xF);
    put(prog[n], 3, 0, 1, 1);
    n++;
    in[1][0] = 2.0f;
    cst[12][0] = 1; cst[12][1] = 2; cst[12][2] = 3; cst[12][3] = 4;
    nv2a_vsh_run((const uint32_t (*)[4])prog, 0, cst, 0,
                 (const float (*)[4])in, &o);
    for (i = 0; i < n; i++)
        printf("  %2d: %s\n", i, nv2a_vsh_disasm(prog[i], buf, sizeof buf));
    CHECK_F("rel x", o.o[NV2A_VSH_O_T0 + 1][0], -4);
    CHECK_F("rel y", o.o[NV2A_VSH_O_T0 + 1][1], -3);
    CHECK_F("rel w", o.o[NV2A_VSH_O_T0 + 1][3], -1);

    if (failures) { printf("%d failure(s)\n", failures); return 1; }
    printf("nv2a_vsh: all passed\n");
    return 0;
}
