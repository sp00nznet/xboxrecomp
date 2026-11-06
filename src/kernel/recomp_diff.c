#include "recomp_diff.h"

#include <stdio.h>

#if defined(_MSC_VER)
extern long __cdecl _InterlockedIncrement(long volatile *);
#endif

struct record { uint32_t f[10]; };  /* sequence eip eax ebx ecx edx esi edi esp eflags */

struct trace_record { uint32_t f[7]; };

static struct trace_record g_trace[RECOMP_DIFF_RING];
static volatile uint32_t g_trace_written;
static volatile uint32_t g_filter_begin, g_filter_end;

static struct record g_ring[RECOMP_DIFF_RING];
static volatile uint32_t g_written;  /* total records ever written */

/* The cursor is claimed atomically so threads never share a slot. Records are
 * not published atomically: a dump taken while other threads are still
 * checkpointing can show a half-written newest record, so dump when quiet. */
static uint32_t claim(void)
{
#if defined(_MSC_VER)
    return (uint32_t)_InterlockedIncrement((long volatile *)&g_written);
#else
    return __atomic_add_fetch(&g_written, 1u, __ATOMIC_RELAXED);
#endif
}

void recomp_diff_checkpoint(uint32_t eip, uint32_t eax, uint32_t ebx,
                            uint32_t ecx, uint32_t edx, uint32_t esi,
                            uint32_t edi, uint32_t esp, uint32_t eflags)
{
    uint32_t seq = claim();
    struct record *r = &g_ring[(seq - 1u) & (RECOMP_DIFF_RING - 1u)];
    r->f[0] = seq; r->f[1] = eip; r->f[2] = eax; r->f[3] = ebx; r->f[4] = ecx;
    r->f[5] = edx; r->f[6] = esi; r->f[7] = edi; r->f[8] = esp; r->f[9] = eflags;
}

int recomp_diff_checkpoint_dump(const char *path)
{
    uint32_t total = g_written, first, i, count;
    FILE *out;

    count = total < RECOMP_DIFF_RING ? total : RECOMP_DIFF_RING;
    first = total - count;
    out = fopen(path, "wb");
    if (!out) return -1;
    for (i = 0; i < count; i++) {
        const struct record *r = &g_ring[(first + i) & (RECOMP_DIFF_RING - 1u)];
        uint8_t le[40];
        int k;
        for (k = 0; k < 10; k++) {   /* explicit little-endian, host-independent */
            le[k * 4 + 0] = (uint8_t)(r->f[k]);
            le[k * 4 + 1] = (uint8_t)(r->f[k] >> 8);
            le[k * 4 + 2] = (uint8_t)(r->f[k] >> 16);
            le[k * 4 + 3] = (uint8_t)(r->f[k] >> 24);
        }
        if (fwrite(le, sizeof le, 1, out) != 1) { fclose(out); return -1; }
    }
    return fclose(out) == 0 ? (int)count : -1;
}

void recomp_diff_checkpoint_reset(void)
{
    g_written = 0;
}

void recomp_diff_trace_set_memory_filter(uint32_t begin, uint32_t end)
{
    g_filter_begin = begin;
    g_filter_end = end;
}

void recomp_diff_trace_event(uint32_t type, uint32_t eip,
                             uint32_t arg0, uint32_t arg1, uint32_t arg2)
{
    uint32_t seq;
    struct trace_record *r;

    if ((type == RECOMP_DIFF_TRACE_READ || type == RECOMP_DIFF_TRACE_WRITE) &&
        g_filter_begin < g_filter_end &&
        (arg0 < g_filter_begin || arg0 >= g_filter_end))
        return;
#if defined(_MSC_VER)
    seq = (uint32_t)_InterlockedIncrement((long volatile *)&g_trace_written);
#else
    seq = __atomic_add_fetch(&g_trace_written, 1u, __ATOMIC_RELAXED);
#endif
    r = &g_trace[(seq - 1u) & (RECOMP_DIFF_RING - 1u)];
    r->f[0] = seq; r->f[1] = 0; r->f[2] = eip; r->f[3] = type;
    r->f[4] = arg0; r->f[5] = arg1; r->f[6] = arg2;
}

int recomp_diff_trace_dump(const char *path)
{
    uint32_t total = g_trace_written, count, first, i;
    FILE *out;

    count = total < RECOMP_DIFF_RING ? total : RECOMP_DIFF_RING;
    first = total - count;
    out = fopen(path, "wb");
    if (!out) return -1;
    for (i = 0; i < count; i++) {
        const struct trace_record *r = &g_trace[(first + i) & (RECOMP_DIFF_RING - 1u)];
        uint8_t le[28];
        int k;
        for (k = 0; k < 7; k++) {
            le[k * 4 + 0] = (uint8_t)(r->f[k]);
            le[k * 4 + 1] = (uint8_t)(r->f[k] >> 8);
            le[k * 4 + 2] = (uint8_t)(r->f[k] >> 16);
            le[k * 4 + 3] = (uint8_t)(r->f[k] >> 24);
        }
        if (fwrite(le, sizeof le, 1, out) != 1) { fclose(out); return -1; }
    }
    return fclose(out) == 0 ? (int)count : -1;
}

void recomp_diff_trace_reset(void)
{
    g_trace_written = 0;
}
