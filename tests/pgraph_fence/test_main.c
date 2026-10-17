/**
 * pgraph_fence: PGRAPH 0x400B10 follows the executor's semaphore releases
 * when no fence mirror is registered, and a registered mirror otherwise.
 * XDK D3D's wait-for-idle spins until bits 2..6 of the register
 * equal the low five bits of its fence word. A fresh scaffold registers no
 * mirror, so the register stayed 0 and Spartan: Total Warrior's boot spun in
 * that wait (runs/spartan r9) although the executor had written the fence.
 *
 * In this order, because a mirror registration cannot be undone:
 *
 *   1 idle      no mirror, no release: the register (pre-set to 0x101, bits
 *               outside 2..6) is untouched by a tick
 *   2 release   a release of 5: bits 2..6 are (5 << 2) & 0x7C, bits 0 and 8
 *               still set
 *   3 follow    a release of 7: the bits follow
 *   4 waiting   a mirror registered whose device pointer is still 0, then a
 *               release of 9: the register keeps 7. Count is 1, nothing is
 *               readable, and the executor does not override the mirror
 *   5 mirror    the device written, its completed word 3: the register shows
 *               3, the mirror's value, not the executor's 9
 *   6 log       the "NV2A PGRAPH fence" line appeared exactly once
 *
 * Negative control: with the no-mirror branch of fence_completed removed,
 * checks 2, 3, 4 and 6 fail and 1 and 5 pass (4 expects the 7 that only the
 * fallback put there; what it guards is that the 9 does not replace it).
 *
 * The layout starts the ack thread, which runs the fence-mirror tick on
 * every pass and this register's tick on its first pass and every 2 s after.
 * It ticks from the same state as the test, so it writes the same value.
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

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define NV2A        0xFD000000u
#define PRAMIN      (NV2A + 0x700000u)
#define SET_CTX_SEMA 0x01A4u
#define SET_SEMA_OFF 0x1D6Cu
#define SEMA_RELEASE 0x1D70u
#define PGRAPH_FENCE 0x400B10u

/* The fence-mirror chain: [P] = device, [device + 0x30] = completed word. */
#define P 0x00200000u
#define D 0x00201000u
#define W 0x00202000u

static int s_fail;
static FILE *s_out;   /* the real stderr, while fd 2 is captured */

static volatile uint32_t *g32(uint32_t va)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

static volatile uint32_t *reg(void)
{
    return (volatile uint32_t *)(xbox_Nv2aAperture() + PGRAPH_FENCE);
}

static void expect(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(s_out, "[pgraph_fence] %-48s 0x%08X want 0x%08X %s\n", what,
            got, want, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

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

static void release(uint32_t value)
{
    M(SET_CTX_SEMA, 8);
    M(SET_SEMA_OFF, 0);
    M(SEMA_RELEASE, value);
}

/* Bits 2..6 for a fence value, over the 0x101 the register was pre-set to. */
static uint32_t shows(uint32_t v)
{
    return 0x101u | ((v << 2) & 0x7Cu);
}

int main(void)
{
    static uint8_t xbe[0x1000];
    char cap[] = "/tmp/pgraph_fence.XXXXXX", line[512];
    int fd, said = 0;
    FILE *f;

    setvbuf(stderr, NULL, _IONBF, 0);
    s_out = fdopen(dup(2), "w");
    if (!s_out)
        return 1;
    setvbuf(s_out, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe) || !xbox_Nv2aAperture()) {
        fprintf(s_out, "[pgraph_fence] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    *g32(NV2A + 0x002210u) = 0x03000000u;  /* PFIFO_RAMHT: PRAMIN 0, 4 KB */
    /* Spartan's fence object: handle 8, physical 0x2000. */
    object(8, 0x40, 0x11B0, 0x0002B03Du, 0x20, 0x00002003u);

    /* Everything the runtime says from here on goes to a file, so check 6
     * can count the line; the verdicts go to s_out. */
    fd = mkstemp(cap);
    if (fd < 0 || dup2(fd, 2) < 0) {
        fprintf(s_out, "[pgraph_fence] FAIL: capture stderr\n");
        return 1;
    }

    *reg() = 0x101u;
    xbox_Nv2aPgraphFenceTick();
    expect("1 idle: no mirror, no release, untouched", *reg(), 0x101u);

    release(5);
    xbox_Nv2aPgraphFenceTick();
    expect("2 release: bits 2..6 from the release of 5", *reg(), shows(5));

    release(7);
    xbox_Nv2aPgraphFenceTick();
    expect("3 follow: the release of 7", *reg(), shows(7));

    *g32(P) = 0;
    xbox_Nv2aMirrorFence(P, 0x2C, 0x30);
    release(9);
    xbox_Nv2aPgraphFenceTick();
    expect("4 waiting: unreadable mirror, still 7", *reg(), shows(7));

    *g32(D + 0x2C) = 3;
    *g32(D + 0x30) = W;
    *g32(W) = 3;
    /* The chain is complete before the ack thread can follow it. */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    *g32(P) = D;
    xbox_Nv2aPgraphFenceTick();
    expect("5 mirror: the mirror's 3, not the executor's 9", *reg(), shows(3));

    f = fdopen(fd, "r");
    if (!f) {
        fprintf(s_out, "[pgraph_fence] FAIL: read capture\n");
        return 1;
    }
    rewind(f);
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, "NV2A PGRAPH fence: no fence mirror registered"))
            said++;
        fputs(line, s_out);   /* the runtime's own lines, for the log */
    }
    unlink(cap);
    expect("6 log: the PGRAPH fence line, once", (uint32_t)said, 1);

    fprintf(s_out, "[pgraph_fence] %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
