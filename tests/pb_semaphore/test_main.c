/**
 * pb_semaphore: a semaphore release and a GET_REPORT land at their context
 * DMA object's base plus the offset, within its limit. Spartan: Total Warrior's D3D (XDK 5849)
 * aims object 8 at its fence page, physical 0x2000, with offset 0; the
 * executor wrote physical 0 and D3D's fence wait spun for ever.
 *
 * Instance memory is laid out as Spartan's was at the hang: PFIFO_RAMHT
 * 0x03000000 (RAMHT at PRAMIN 0, 4 KB), handle 8 at RAMHT +0x40 with
 * context 0x8000011B (instance 0x11B0), object { 0x0002B03D, 0x20,
 * 0x00002003 }.
 *
 *   base      handle 8: the release lands at physical 0x2000, not 0
 *   adjust    an object with adjust 0x020 in word 0 and frame 0x3000:
 *             lands at 0x3020 (a fresh SET_CONTEXT_DMA_SEMAPHORE is
 *             looked up again)
 *   limit 0x1F  handle 9 has the XDK's real 32-byte shape, limit 0x1F
 *             (inclusive: its last byte). Offset 0x1C ends at 0x20 = limit
 *             + 1 and lands; 0x1D ends a byte past and is refused. This is
 *             the case that tells an inclusive limit from an exclusive one
 *   offset    the offset adds to the base
 *   missing   a handle RAMHT does not hold: the bare offset, as before
 *   cached    object 8 rewritten in PRAMIN while bound: a release with no
 *             re-bind still lands at the old base, 0x2000
 *   rebind    object 8 rewritten in PRAMIN (frame 0x4000) and bound again:
 *             the next release lands at 0x4000 (a bind re-reads, a release
 *             does not)
 *   limit     a release at offset 0x20 of the 0x20-limit object ends past
 *             limit + 1: refused, the word untouched; offset 0x1C still
 *             lands
 *   report    GET_REPORT after SET_CONTEXT_DMA_REPORT of an object at
 *             physical 0x5000 (CPU backend): the 16-byte report's status
 *             lands at 0x5000 + offset + 12, and the bare offset's is left
 *             alone; with limit 0xFFF, offset 0xFF0 (limit - 15) is the
 *             last that fits and 0xFF1 is refused
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

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
#define SET_CTX_REPORT 0x01A8u
#define CLEAR_REPORT   0x17C8u
#define GET_REPORT     0x17D0u

static int s_fail;

static volatile uint32_t *g32(uint32_t va)
{
    return (volatile uint32_t *)((uint8_t *)xbox_GetMemoryOffset() + va);
}

static void expect(const char *what, uint32_t got, uint32_t want)
{
    int ok = got == want;
    fprintf(stderr, "[pb_semaphore] %-44s 0x%08X want 0x%08X %s\n", what,
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

static void release(uint32_t handle, uint32_t offset, uint32_t value)
{
    M(SET_CTX_SEMA, handle);
    M(SET_SEMA_OFF, offset);
    M(SEMA_RELEASE, value);
}

int main(void)
{
    static uint8_t xbe[0x1000];

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe) || !xbox_Nv2aAperture()) {
        fprintf(stderr, "[pb_semaphore] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);             /* the CPU rasteriser */
    *g32(NV2A + 0x002210u) = 0x03000000u;  /* PFIFO_RAMHT: PRAMIN 0, 4 KB */
    object(8, 0x40, 0x11B0, 0x0002B03Du, 0x20, 0x00002003u);
    object(9, 0x48, 0x11C0, 0x0202B03Du, 0x1F, 0x00003003u);

    *g32(nv2a_pb_dma_resolve(0)) = 0xAAAAAAAAu;
    release(8, 0, 5);
    expect("base: release at physical 0x2000",
           *g32(nv2a_pb_dma_resolve(0x2000)), 5);
    expect("base: physical 0 untouched",
           *g32(nv2a_pb_dma_resolve(0)), 0xAAAAAAAAu);

    release(9, 0, 7);
    expect("adjust: release at physical 0x3020",
           *g32(nv2a_pb_dma_resolve(0x3020)), 7);

    /* inclusive limit: the XDK's 32-byte object has limit 0x1F, its last
     * byte. A 4-byte write at 0x1C ends at 0x20 = limit + 1 and lands; one
     * at 0x1D ends a byte past it and is refused. */
    *g32(nv2a_pb_dma_resolve(0x3040)) = 0xCCCCCCCCu;
    release(9, 0x1C, 19);
    expect("limit 0x1F: offset 0x1C lands",
           *g32(nv2a_pb_dma_resolve(0x303C)), 19);
    release(9, 0x1D, 21);
    expect("limit 0x1F: offset 0x1D refused (0x303C kept)",
           *g32(nv2a_pb_dma_resolve(0x303C)), 19);
    expect("limit 0x1F: offset 0x1D refused (0x3040 kept)",
           *g32(nv2a_pb_dma_resolve(0x3040)), 0xCCCCCCCCu);

    release(8, 0x10, 9);
    expect("offset: release at physical 0x2010",
           *g32(nv2a_pb_dma_resolve(0x2010)), 9);

    release(0x77, 0x40, 11);
    expect("missing: bare offset 0x40",
           *g32(nv2a_pb_dma_resolve(0x40)), 11);

    /* rebind: the object changes under the same handle. Until it is bound
     * again the old base stands; the bind picks up the new one. */
    M(SET_CTX_SEMA, 8);
    *g32(PRAMIN + 0x11B0 + 8) = 0x00004003u;
    M(SET_SEMA_OFF, 0);
    M(SEMA_RELEASE, 12);
    expect("cached: release without a re-bind at physical 0x2000",
           *g32(nv2a_pb_dma_resolve(0x2000)), 12);
    release(8, 0, 13);
    expect("rebind: release at physical 0x4000",
           *g32(nv2a_pb_dma_resolve(0x4000)), 13);

    /* limit: the object at 0x4000 has limit 0x20, so a 4-byte write may
     * start at 0x1D at the latest. */
    *g32(nv2a_pb_dma_resolve(0x4020)) = 0xBBBBBBBBu;
    release(8, 0x20, 15);
    expect("limit: past limit + 1 refused",
           *g32(nv2a_pb_dma_resolve(0x4020)), 0xBBBBBBBBu);
    release(8, 0x1C, 17);
    expect("limit: offset 0x1C lands",
           *g32(nv2a_pb_dma_resolve(0x401C)), 17);

    /* report: object 10 at physical 0x5000, limit 0xFFF. */
    object(10, 0x50, 0x11D0, 0x0002B03Du, 0xFFF, 0x00005003u);
    *g32(nv2a_pb_dma_resolve(0x5040 + 12)) = 0xFFFFFFFFu;
    *g32(nv2a_pb_dma_resolve(0x0040 + 12)) = 0xFFFFFFFFu;
    M(SET_CTX_REPORT, 10);
    M(CLEAR_REPORT, 0);
    M(GET_REPORT, (1u << 24) | 0x40u);
    expect("report: status written at 0x5040 + 12",
           *g32(nv2a_pb_dma_resolve(0x5040 + 12)), 0);
    expect("report: bare offset 0x40 + 12 untouched",
           *g32(nv2a_pb_dma_resolve(0x0040 + 12)), 0xFFFFFFFFu);

    /* report limit: a report is 16 bytes, so limit - 15 (0xFF0) is the last
     * offset that fits and limit - 14 is refused. */
    *g32(nv2a_pb_dma_resolve(0x5FF0 + 12)) = 0xFFFFFFFFu;
    M(GET_REPORT, (1u << 24) | 0xFF0u);
    expect("report limit: offset 0xFF0 lands",
           *g32(nv2a_pb_dma_resolve(0x5FF0 + 12)), 0);
    *g32(nv2a_pb_dma_resolve(0x5FF0 + 12)) = 0xDDDDDDDDu;
    *g32(nv2a_pb_dma_resolve(0x6000)) = 0xDDDDDDDDu;
    M(GET_REPORT, (1u << 24) | 0xFF1u);
    expect("report limit: offset 0xFF1 refused (0x5FFC kept)",
           *g32(nv2a_pb_dma_resolve(0x5FF0 + 12)), 0xDDDDDDDDu);
    expect("report limit: offset 0xFF1 refused (0x6000 kept)",
           *g32(nv2a_pb_dma_resolve(0x6000)), 0xDDDDDDDDu);

    fprintf(stderr, "[pb_semaphore] %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
