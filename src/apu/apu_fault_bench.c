/*
 * apu_fault_bench.c -- RECOMP_APU_FAULT_BENCH, apart from apu_mmio_hook.c.
 *
 * Its own object because it needs the kernel's xbox_GetMemoryOffset: a link
 * that pulls in the hook (every mcpx_apu_init_standalone does, for the trace
 * reporter) does not then need the kernel too.
 */
#include "apu.h"
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>

#if defined(_WIN32)
#include <windows.h>

extern volatile LONG g_apu_hook_bench_active;   /* apu_mmio_hook.c */
extern MCPXAPUState *g_apu_state;               /* apu_mmio_hook.c */

/* RECOMP_APU_FAULT_BENCH=<n>: the full cost of one trapped access.
 *
 * Time n reads of NV1BA0_PIO_FREE (0xFE820010, read-only, the register
 * DirectSound polls most) through the same page fault, VEH and decoder a
 * guest access takes, from the calling thread. Call it after the VEH is
 * installed and g_apu_state is set. The accesses are left out of the
 * RECOMP_APU_TRACE tallies. Prints one line and returns the microseconds per
 * access (0 if it did not run). */
double apu_hook_fault_bench(unsigned n)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    volatile uint32_t *p;
    LARGE_INTEGER f, a, b;
    uint32_t sink = 0;
    double us;

    if (!g_apu_state || !n)
        return 0;
    p = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + 0xFE820010u);
    QueryPerformanceFrequency(&f);
    InterlockedExchange(&g_apu_hook_bench_active, 1);
    QueryPerformanceCounter(&a);
    /* A plain MOV load: left to itself the compiler folds the read into
     * `add r32, [mem]`, a form the decoder does not handle. */
    for (unsigned i = 0; i < n; i++) {
        uint32_t v;
        __asm__ volatile("movl (%1), %0" : "=r"(v) : "r"(p) : "memory");
        sink += v;
    }
    QueryPerformanceCounter(&b);
    InterlockedExchange(&g_apu_hook_bench_active, 0);
    us = (double)(b.QuadPart - a.QuadPart) * 1e6 / (double)f.QuadPart / n;
    fprintf(stderr, "[APU] fault bench: %u trapped reads of PIO_FREE, %.2f us"
            " each (fault + VEH + decode + model; value 0x%X)\n",
            n, us, sink / n);
    return us;
}

#endif /* _WIN32 */

#if !defined(_WIN32) && defined(__aarch64__)
#include <time.h>
extern volatile int g_apu_hook_bench_active;    /* apu_mmio_hook.c */
extern MCPXAPUState *g_apu_state;

/* The POSIX arm64 twin: n trapped `ldr w` reads of PIO_FREE through SIGBUS,
 * the A64 decoder and the model. */
double apu_hook_fault_bench(unsigned n)
{
    extern ptrdiff_t xbox_GetMemoryOffset(void);
    volatile uint32_t *p;
    struct timespec a, b;
    uint32_t sink = 0;
    double us;

    if (!g_apu_state || !n)
        return 0;
    p = (volatile uint32_t *)((uintptr_t)xbox_GetMemoryOffset() + 0xFE820010u);
    g_apu_hook_bench_active = 1;
    clock_gettime(CLOCK_MONOTONIC, &a);
    for (unsigned i = 0; i < n; i++) {
        uint32_t v;
        __asm__ volatile("ldr %w0, [%1]" : "=r"(v) : "r"(p) : "memory");
        sink += v;
    }
    clock_gettime(CLOCK_MONOTONIC, &b);
    g_apu_hook_bench_active = 0;
    us = ((b.tv_sec - a.tv_sec) * 1e9 + (b.tv_nsec - a.tv_nsec)) / 1e3 / n;
    fprintf(stderr, "[APU] fault bench: %u trapped reads of PIO_FREE, %.2f us"
            " each (fault + signal + decode + model; value 0x%X)\n",
            n, us, sink / n);
    return us;
}
#elif !defined(_WIN32)
double apu_hook_fault_bench(unsigned n) { (void)n; return 0; }
#endif
