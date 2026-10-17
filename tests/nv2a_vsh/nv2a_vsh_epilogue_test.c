/* The Xbox D3D screen-space epilogue, as every title's programs end:
 *
 *   mov o[0], v0                      position (stand-in for the DP4s)
 *   mul o[0].xyz, R12, c[58]  + rcc R1.x, R12.w
 *   mad o[0].xyz, R12, R1.x, c[59]
 *
 * The rcc is paired with a MAC op that writes no temp, and must still land
 * in R1; sent to the instruction's temp field instead, R1.x stayed 0 and
 * every vertex came out at c[59].
 *
 * Upstream's tests/nv2a_vsh/nv2a_vsh_test.c (sp00nznet/xboxrecomp), ported
 * from nv2a_vsh_interp's stateful API (set_instruction / set_constant /
 * run) to nv2a_vsh_cpu's stateless one (program, constants and inputs
 * passed to nv2a_vsh_run). The program and the expected result are the
 * same. */
#include "nv2a_vsh_cpu.h"
#include <math.h>
#include <stdio.h>

enum { T = 1, V = 2, C = 3 };             /* source mux */
#define XYZW 0x1B                         /* swizzle x,y,z,w */
#define WWWW 0xFF
#define XXXX 0x00

static void src(uint32_t w[4], int which, int mux, int reg, int swz)
{
    if (which == 0) { w[1] |= swz; w[2] |= (mux << 26) | (reg << 28); }
    else if (which == 1) { w[2] |= (swz << 17) | (mux << 11) | (reg << 13); }
    else { w[2] |= (swz << 2) | (reg >> 2); w[3] |= (mux << 28) | ((reg & 3) << 30); }
}

int main(void)
{
    static uint32_t prog[NV2A_VSH_SLOTS][4];
    static float c[NV2A_VSH_CONSTANTS][4];
    float in[NV2A_VSH_INPUTS][4] = {{0}};
    uint32_t *i0 = prog[0], *i1 = prog[1], *i2 = prog[2];
    const float *pos;
    Nv2aVshOut o;

    /* mov o[0].xyzw, v0 */
    i0[1] = (1u << 21);                         /* MAC mov, input v0 */
    src(i0, 0, V, 0, XYZW);
    i0[3] |= (0xFu << 12) | (1u << 11);         /* o[0] mask xyzw */

    /* mul o[0].xyz, R12, c[58] ; rcc R1.x, R12.w (temp field says R7) */
    i1[1] = (3u << 25) | (2u << 21) | (58u << 13);
    src(i1, 0, T, 12, XYZW); src(i1, 1, C, 0, XYZW); src(i1, 2, T, 12, WWWW);
    i1[3] |= (7u << 20) | (8u << 16) | (0xEu << 12) | (1u << 11);

    /* mad o[0].xyz, R12, R1.x, c[59] ; final */
    i2[1] = (4u << 21) | (59u << 13);
    src(i2, 0, T, 12, XYZW); src(i2, 1, T, 1, XXXX); src(i2, 2, C, 0, XYZW);
    i2[3] |= (0xEu << 12) | (1u << 11) | 1u;

    c[58][0] = 320; c[58][1] = -240; c[58][2] = 1000; c[58][3] = 0;
    c[59][0] = 320; c[59][1] = 240;  c[59][2] = 0;    c[59][3] = 0;

    in[0][0] = 1.0f; in[0][1] = 0.5f; in[0][2] = 0.25f; in[0][3] = 2.0f;
    nv2a_vsh_run((const uint32_t (*)[4])prog, 0, c, 0,
                 (const float (*)[4])in, &o);
    if (!o.ok) {
        puts("FAIL: program did not run");
        return 1;
    }
    pos = o.o[NV2A_VSH_O_POS];
    /* x = 1*320/2 + 320, y = 0.5*-240/2 + 240, z = 0.25*1000/2, w kept */
    if (fabsf(pos[0] - 480) > 1e-3f || fabsf(pos[1] - 180) > 1e-3f
        || fabsf(pos[2] - 125) > 1e-3f || pos[3] != 2.0f) {
        printf("FAIL: pos %g %g %g %g, want 480 180 125 2\n",
               pos[0], pos[1], pos[2], pos[3]);
        return 1;
    }
    puts("ok");
    return 0;
}
