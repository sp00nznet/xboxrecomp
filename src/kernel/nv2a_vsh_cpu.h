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

/* The instructions of the program in `prog` from slot `start` to its first
 * FINAL that oPos depends on, in order, into `pos` (the FINAL one last, so
 * the copy ends where the program does). nv2a_vsh_run on `pos` from slot 0
 * leaves oPos and ok as the whole program would, bit for bit: an
 * instruction is kept when it writes a component (of R0..R12, oPos being
 * R12, or a0) that a kept one later reads before another write covers it,
 * and every unit of a kept instruction runs as before. Only oPos is
 * meaningful in its output, and only with const_write 0, as the 2D test
 * runs it. Returns the slots kept, 0 for a program with no FINAL from
 * start (its run never sets ok). */
uint32_t nv2a_vsh_slice_pos(const uint32_t prog[NV2A_VSH_SLOTS][4], uint32_t start,
                            uint32_t pos[NV2A_VSH_SLOTS][4]);

/* nv2a_vsh_slice_pos, kept: the program to run from *start for oPos alone
 * (the 2D test, render.ao's depth samples). Its slice, *start set to 0,
 * or prog itself, *start kept, for a program with no FINAL. The slices are
 * kept by the program's words from *start to FINAL, compared whole, so a
 * program rewritten in place gets a new one. Walker thread only. Hits and
 * misses count every lookup, for the [PB-COST] line; clear empties the
 * cache (tests). The slice returned lives in the cache: it is valid only
 * until the next nv2a_vsh_pos_prog call, which may evict it. The cache is
 * 128 entries of two program copies, about 557 KB of BSS. */
const uint32_t (*nv2a_vsh_pos_prog(const uint32_t prog[NV2A_VSH_SLOTS][4],
                                   uint32_t *start))[4];
extern uint64_t nv2a_vsh_slice_hits, nv2a_vsh_slice_misses;
void nv2a_vsh_slice_clear(void);

/* Every run and instruction since start, for the [PB-COST] line: the CPU
 * interpreter on the draw path (hor+, snap_2d, render.ao) is a per-batch
 * cost. Plain counters, kept only while nv2a_vsh_count_on (set by
 * nv2a_pb_cost_init); a run off the walker's thread may be missed. */
extern int nv2a_vsh_count_on;
extern uint32_t nv2a_vsh_run_count;
extern uint64_t nv2a_vsh_step_count;

/* One instruction as text, for traces and tests. Returns `buf`. */
char *nv2a_vsh_disasm(const uint32_t insn[4], char *buf, int size);

#endif
