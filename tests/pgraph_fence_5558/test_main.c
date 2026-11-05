/**
 * pgraph_fence_5558: PGRAPH 0x400B10's fence field sits where the title's
 * D3D puts it. D3D inserts a fence as subchannel 5 method 0x0310
 * followed by BACK_END_WRITE_SEMAPHORE_RELEASE, and its wait-for-idle spins
 * until the register's copy of the fence matches the fence word:
 *
 *   XDK 5849 (BLiNX 2)  0x0310 word ((pb << 3 | f & 0x1F) << 2) | (x & 3)
 *                       wait ((reg ^ f << 2) & 0x7C) == 0
 *   XDK 5558 (BLiNX 1)  0x0310 word (pb << 5) | (f & 0x1F)
 *                       wait ((reg ^ f) & 0x1F) == 0
 *
 * With the field fixed at bits 2..6, BLiNX 1 spun at 0x00139140 on its
 * loading screen. The executor now reads the shift off each adjacent pair.
 *
 * No fence mirror is registered, so the register follows the executor's
 * releases (tests/pgraph_fence covers that choice). In this order, because
 * the learned shift is global:
 *
 *   1 default    a release with no 0x0310 before it: bits 2..6, the 5849
 *                wait passes, and no shift is learned
 *   2 stop       a 5558 word, a walk stop, the salvaged release: not a pair
 *   3 zero       a fence whose low five bits are 0 fits several shifts: skipped
 *   4 5849       a 5849 pair: shift 2, the 5849 wait passes
 *   5 apart      a 5558 word with a method between it and the release: not
 *                a pair, still 2
 *   6 subch 0    the same word as Kelvin's 0x0310 (SET_DITHER_ENABLE): not
 *                a pair, still 2
 *   7 stray      one 5558 pair once 2 is learned: still 2, 5849 wait passes
 *   8 5558       two more 5558 pairs in a row: shift 0, the 5558 wait
 *                passes, and the old field's bits 5..6 are cleared
 *   9 follow     the next 5558 fence: the wait passes again
 *  10 ambiguous  a word that fits at shifts 0 and 2 alike: the shift stays 0
 *  11 log        the "at bits 0..4" line appeared exactly once
 *
 * Negative control: with the tick's shift fixed at 2, checks 8 and 9 fail
 * (their waits spin); without the walk-stop forget, check 2 learns 0;
 * changing the shift on one pair fails check 7; clearing only the new
 * field fails check 8's bits 5..6.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
int nv2a_pb_exec_fence_shift(void);
void nv2a_pb_exec_fence_ref_forget(void);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define NV2A         0xFD000000u
#define PRAMIN       (NV2A + 0x700000u)
#define SET_CTX_SEMA 0x01A4u
#define SET_SEMA_OFF 0x1D6Cu
#define SEMA_RELEASE 0x1D70u
#define FENCE_REF    0x0310u
#define PGRAPH_FENCE 0x400B10u
#define PB           0x80012340u   /* where the fence sits in the pushbuffer */

static int s_fail;
static FILE *s_out;   /* the real stderr, while fd 2 is captured */

static volatile uint32_t *g32(uint32_t va)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

static uint32_t reg(void)
{
    return *(volatile uint32_t *)(xbox_Nv2aAperture() + PGRAPH_FENCE);
}

static void expect(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(s_out, "[pgraph_fence_5558] %-46s 0x%08X want 0x%08X %s\n", what,
            got, want, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

/* D3D's two waits, as they test the register against the fence word. */
static uint32_t wait_5849_passes(uint32_t f) { return ((reg() ^ (f << 2)) & 0x7Cu) == 0; }
static uint32_t wait_5558_passes(uint32_t f) { return ((reg() ^ f) & 0x1Fu) == 0; }

static uint32_t word_5849(uint32_t f) { return ((PB << 3 | (f & 0x1Fu)) << 2) | 1u; }
static uint32_t word_5558(uint32_t f) { return (PB << 5) | (f & 0x1Fu); }

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

static void object(uint32_t handle, uint32_t ramht_off, uint32_t inst,
                   uint32_t w0, uint32_t limit, uint32_t w2)
{
    *g32(PRAMIN + ramht_off) = handle;
    *g32(PRAMIN + ramht_off + 4) = 0x80000000u | (inst >> 4);
    *g32(PRAMIN + inst) = w0;
    *g32(PRAMIN + inst + 4) = limit;
    *g32(PRAMIN + inst + 8) = w2;
}

static void release(uint32_t f)
{
    nv2a_pb_exec_method(0, SEMA_RELEASE, f);
    xbox_Nv2aPgraphFenceTick();
}

/* InsertFence's two methods back to back, then the ack loop's tick. */
static void fence(uint32_t word, uint32_t f)
{
    nv2a_pb_exec_method(5, FENCE_REF, word);
    release(f);
}

int main(void)
{
    static uint8_t xbe[0x1000];
    char cap[] = "/tmp/pgraph_fence_5558.XXXXXX", line[512];
    int fd, said = 0;
    FILE *f;

    setvbuf(stderr, NULL, _IONBF, 0);
    s_out = fdopen(dup(2), "w");
    if (!s_out)
        return 1;
    setvbuf(s_out, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe) || !xbox_Nv2aAperture()) {
        fprintf(s_out, "[pgraph_fence_5558] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    *g32(NV2A + 0x002210u) = 0x03000000u;  /* PFIFO_RAMHT: PRAMIN 0, 4 KB */
    /* BLiNX 1's fence object: handle 8, physical 0x1000. */
    object(8, 0x40, 0x11B0, 0x0002B03Du, 0x20, 0x00001003u);
    nv2a_pb_exec_method(0, SET_CTX_SEMA, 8);
    nv2a_pb_exec_method(0, SET_SEMA_OFF, 0);

    fd = mkstemp(cap);
    if (fd < 0 || dup2(fd, 2) < 0) {
        fprintf(s_out, "[pgraph_fence_5558] FAIL: capture stderr\n");
        return 1;
    }

    release(5);
    expect("1 default: 5849 wait passes on 5", wait_5849_passes(5), 1);
    expect("1 default: no shift learned", (uint32_t)nv2a_pb_exec_fence_shift(),
           (uint32_t)-1);

    /* What nv2a_pb_scan.c does when a walk stops right after the word. */
    nv2a_pb_exec_method(5, FENCE_REF, word_5558(9));
    nv2a_pb_exec_fence_ref_forget();
    release(9);
    expect("2 stop: no shift learned", (uint32_t)nv2a_pb_exec_fence_shift(),
           (uint32_t)-1);

    fence(word_5558(0x20), 0x20);
    expect("3 zero: no shift learned", (uint32_t)nv2a_pb_exec_fence_shift(),
           (uint32_t)-1);

    fence(word_5849(7), 7);
    expect("4 5849: shift 2", (uint32_t)nv2a_pb_exec_fence_shift(), 2);
    expect("4 5849: 5849 wait passes on 7", wait_5849_passes(7), 1);

    nv2a_pb_exec_method(5, FENCE_REF, word_5558(9));
    nv2a_pb_exec_method(0, SET_SEMA_OFF, 0);
    release(9);
    expect("5 apart: still 2", (uint32_t)nv2a_pb_exec_fence_shift(), 2);

    nv2a_pb_exec_method(0, FENCE_REF, word_5558(11));
    release(11);
    expect("6 subch 0: still 2", (uint32_t)nv2a_pb_exec_fence_shift(), 2);

    fence(word_5558(13), 13);
    expect("7 stray: still 2", (uint32_t)nv2a_pb_exec_fence_shift(), 2);
    expect("7 stray: 5849 wait passes on 13", wait_5849_passes(13), 1);

    fence(word_5558(15), 15);
    fence(word_5558(17), 17);
    expect("8 5558: shift 0", (uint32_t)nv2a_pb_exec_fence_shift(), 0);
    expect("8 5558: 5558 wait passes on 17", wait_5558_passes(17), 1);
    expect("8 5558: old field's bits 5..6 cleared", reg() & 0x60u, 0);

    fence(word_5558(19), 19);
    expect("9 follow: 5558 wait passes on 19", wait_5558_passes(19), 1);

    /* 0x55 holds 0x15 at bit 0 and at bit 2: no evidence either way. */
    fence((PB << 7) | 0x55u, 0x35);
    expect("10 ambiguous: still 0", (uint32_t)nv2a_pb_exec_fence_shift(), 0);
    expect("10 ambiguous: 5558 wait passes on 0x35", wait_5558_passes(0x35), 1);

    f = fdopen(fd, "r");
    if (!f) {
        fprintf(s_out, "[pgraph_fence_5558] FAIL: read capture\n");
        return 1;
    }
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "fence field in PGRAPH 0x400B10 at bits 0..4"))
            said++;
        fputs(line, s_out);   /* the runtime's own lines, for the log */
    }
    unlink(cap);
    expect("11 log: the at bits 0..4 line, once", (uint32_t)said, 1);

    fprintf(s_out, "[pgraph_fence_5558] %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
