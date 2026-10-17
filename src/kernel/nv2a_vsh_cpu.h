/**
 * NV2A vertex program interpreter, on the CPU.
 *
 * The pushbuffer executor (nv2a_pb_exec.c) rasterises in software, so the
 * vertex stage has to run in software too: a title drawing 3D geometry hands
 * the GPU object-space positions and a program that turns them into screen
 * space, and without running the program there is nothing to rasterise.
 *
 * This is deliberately free of D3D, Win32 and the executor's state, so it can
 * be unit-tested on its own (tests/nv2a_vsh). The microcode decode follows
 * xemu's hw/xbox/nv2a/pgraph/vsh.c field table, which is the reference the
 * D3D11 translator in src/d3d/d3d8_vsh.c also cites.
 *
 * Program memory is 136 slots of 128 bits; constants are 192 float4. XDK D3D
 * appends a viewport epilogue to every program it builds
 *
 *     mul oPos.xyz, R12, c[58]        ; viewport scale  (XDK c-38)
 *   + rcc R1.x, R12.w
 *     mad oPos.xyz, R12, R1.x, c[59]  ; viewport offset (XDK c-37)
 *
 * so what comes out of oPos is already divided by w and in surface pixels,
 * with the clip-space w left in oPos.w. That is exactly what the rasteriser
 * wants, and why no separate viewport transform appears here.
 */
#ifndef XBOXRECOMP_NV2A_VSH_CPU_H
#define XBOXRECOMP_NV2A_VSH_CPU_H

#include <stdint.h>

#define NV2A_VSH_SLOTS     136
#define NV2A_VSH_CONSTANTS 192
#define NV2A_VSH_INPUTS    16

/* Output registers, by the index the microcode's output address uses. */
enum {
    NV2A_VSH_O_POS  = 0,
    NV2A_VSH_O_D0   = 3,
    NV2A_VSH_O_D1   = 4,
    NV2A_VSH_O_FOG  = 5,
    NV2A_VSH_O_PTS  = 6,
    NV2A_VSH_O_B0   = 7,
    NV2A_VSH_O_B1   = 8,
    NV2A_VSH_O_T0   = 9,      /* T1..T3 follow */
    NV2A_VSH_O_COUNT = 13
};

typedef struct {
    float o[NV2A_VSH_O_COUNT][4];
    int   steps;               /* instructions executed */
    int   ok;                  /* reached a FINAL instruction */
} Nv2aVshOut;

/* Run the program in `prog` (4 dwords per slot) from slot `start` on one
 * vertex. `c` is the constant file, read and possibly written (programs may
 * write constants when the title enables it; the write is honoured only when
 * `const_write` is set, as on hardware with CXT_WRITE_EN). */
void nv2a_vsh_run(const uint32_t prog[NV2A_VSH_SLOTS][4], uint32_t start,
                  float c[NV2A_VSH_CONSTANTS][4], int const_write,
                  const float v[NV2A_VSH_INPUTS][4], Nv2aVshOut *out);

/* One instruction as text, for traces and tests. Returns `buf`. */
char *nv2a_vsh_disasm(const uint32_t insn[4], char *buf, int size);

#endif
