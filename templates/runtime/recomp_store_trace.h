/**
 * Store tracing for code lifted with --trace-stores (tools.recomp).
 *
 * Who wrote this guest address, and when, is the question a state map of a
 * title needs, and the host cannot answer it: a write watchpoint sees host
 * pages, 16 KB on arm64 macOS, so a watch on one field fires on every store
 * to its neighbours. Lifted with --trace-stores, every guest store that can
 * reach memory outside the stack is bracketed:
 *
 *     _st_a = (uint32_t)(ebx + 0x10);          address, before the store
 *     MEM32(ebx + 0x10) = eax;
 *     RECOMP_STORE(0x00176183u, 0x00176150u, 4u);   guest va, function, size
 *
 * and a store that overlaps one of the traced ranges becomes a row in a
 * per-thread ring, flushed to a file (src/kernel/recomp_store_trace.c).
 *
 * Ranges: RECOMP_TRACE=stores=LO+LEN|LO-HI,... at run time; unset, the
 * ranges given to --trace-stores at generation (RECOMP_STORE_TRACE_DEFAULTS
 * below); stores=0 or no range at all leaves every hook a load and a branch.
 *
 * The generator writes this file into gen/ only when the option is on, with
 * RECOMP_STORE_TRACE_DEFAULTS defined first; a stock lift never includes it.
 */
#ifndef RECOMP_STORE_TRACE_H
#define RECOMP_STORE_TRACE_H

#include <stdint.h>

#ifndef RECOMP_STORE_TRACE_DEFAULTS
#define RECOMP_STORE_TRACE_DEFAULTS ""
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Row kinds. STORE and STRING are what the lifter emits; the rest are kept
 * for owned code (task 2.5 of owned-logic-spike) logging the same rows. */
enum {
    RECOMP_STORE_KIND_STORE = 0,    /* one store of `size` bytes */
    RECOMP_STORE_KIND_STRING = 1,   /* rep stos/movs: `size` bytes from addr */
    RECOMP_STORE_KIND_ENTER = 2,
    RECOMP_STORE_KIND_EXIT = 3,
    RECOMP_STORE_KIND_OWNED = 4,
};

#define RECOMP_STORE_TRACE_MAX_RANGES 32

/* 0: not read yet, 1: on, -1: off. Ranges are [lo, hi), 64-bit so a range
 * ending at 4 GB and a store wrapping past it compare correctly. */
extern int32_t g_store_trace_state;
extern uint32_t g_store_trace_nranges;
extern uint64_t g_store_trace_lo[RECOMP_STORE_TRACE_MAX_RANGES];
extern uint64_t g_store_trace_hi[RECOMP_STORE_TRACE_MAX_RANGES];

/* Read the ranges (RECOMP_TRACE stores=, else `defaults`). Idempotent and
 * thread-safe; the first hook calls it. */
void recomp_store_trace_init(const char *defaults);
/* One row: the store at guest va (in function func) wrote size bytes at
 * addr. The value logged is read back from guest memory. */
void recomp_store_log(uint32_t va, uint32_t addr, uint32_t size,
                      uint32_t func, uint32_t kind);
/* Write every thread's pending rows. Called at exit; safe to call earlier
 * from a quiet point. */
void recomp_store_trace_flush(void);

#if defined(_MSC_VER)
#define RECOMP_STORE_STATE() \
    ((int32_t)__iso_volatile_load32((const volatile __int32 *)&g_store_trace_state))
#else
#define RECOMP_STORE_STATE() \
    __atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE)
#endif

static inline int recomp_store_hit(uint32_t addr, uint32_t size)
{
    int32_t st = RECOMP_STORE_STATE();
    uint64_t lo, hi;
    uint32_t i;
    if (st == 0) {
        recomp_store_trace_init(RECOMP_STORE_TRACE_DEFAULTS);
        st = RECOMP_STORE_STATE();
    }
    if (st < 0)
        return 0;
    lo = addr;
    hi = lo + size;
    for (i = 0; i < g_store_trace_nranges; i++)
        if (lo < g_store_trace_hi[i] && hi > g_store_trace_lo[i])
            return 1;
    return 0;
}

#define RECOMP_STORE(va, func, size) \
    do { if (recomp_store_hit(_st_a, (size))) \
             recomp_store_log((va), _st_a, (size), (func), \
                              RECOMP_STORE_KIND_STORE); } while (0)
#define RECOMP_STORE_N(va, func) \
    do { if (_st_n && recomp_store_hit(_st_a, _st_n)) \
             recomp_store_log((va), _st_a, _st_n, (func), \
                              RECOMP_STORE_KIND_STRING); } while (0)

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_STORE_TRACE_H */
