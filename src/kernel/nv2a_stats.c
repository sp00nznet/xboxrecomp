/*
 * nv2a_stats.c - the runtime counters behind the stats strip, the Stats
 * page and the title's stats (nv2a_stats.h). Enhancements builds only.
 */
#include "nv2a_stats.h"
#include "nv2a_tex_pack.h"

#include <string.h>

int nv2a_stats_on;
uint64_t nv2a_stats_count_[NV2A_STAT_COUNT];

static unsigned s_users;                    /* NV2A_STATS_* bits, atomic */

/* The frame-time ring: one writer (the flip site), any reader. An entry
 * is the frame's length in microseconds and its end in milliseconds (low
 * 32 bits); a reader may see one entry being replaced, which only moves
 * the strip's numbers by one frame. */
#define RING 128
static uint32_t s_ring_us[RING], s_ring_end_ms[RING];
static uint64_t s_ring_n;                   /* entries written, atomic */
static uint64_t s_last_flip_ns;             /* the writer's */

void nv2a_stats_use(unsigned who, int on)
{
    unsigned u = on ? __atomic_or_fetch(&s_users, who, __ATOMIC_RELAXED)
                    : __atomic_and_fetch(&s_users, ~who, __ATOMIC_RELAXED);
    /* A frame time measured across a stretch with the flag clear would be
     * the whole stretch: start the ring's clock again. */
    if (u && !__atomic_load_n(&nv2a_stats_on, __ATOMIC_RELAXED))
        __atomic_store_n(&s_last_flip_ns, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&nv2a_stats_on, u != 0, __ATOMIC_RELAXED);
}

void nv2a_stats_flip_(uint32_t draws, uint64_t now_ns)
{
    uint64_t last, n;
    __atomic_add_fetch(&nv2a_stats_count_[NV2A_STAT_FLIPS], 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&nv2a_stats_count_[NV2A_STAT_DRAWS], draws, __ATOMIC_RELAXED);
    last = __atomic_exchange_n(&s_last_flip_ns, now_ns, __ATOMIC_RELAXED);
    if (!last || now_ns <= last)
        return;
    n = __atomic_load_n(&s_ring_n, __ATOMIC_RELAXED);
    {
        uint64_t us = (now_ns - last) / 1000u;
        __atomic_store_n(&s_ring_us[n % RING], us > 0xffffffffu ? 0xffffffffu : (uint32_t)us,
                         __ATOMIC_RELAXED);
        __atomic_store_n(&s_ring_end_ms[n % RING], (uint32_t)(now_ns / 1000000u),
                         __ATOMIC_RELAXED);
    }
    __atomic_store_n(&s_ring_n, n + 1, __ATOMIC_RELEASE);
}

void nv2a_stats_read(struct nv2a_stats_snap *s, uint64_t now_ns)
{
    uint64_t n = __atomic_load_n(&s_ring_n, __ATOMIC_ACQUIRE);
    uint32_t now_ms = (uint32_t)(now_ns / 1000000u);
    uint64_t sum = 0;
    uint32_t max = 0;
    unsigned i, k;

    memset(s, 0, sizeof *s);
    for (k = 0; k < NV2A_STAT_COUNT; k++)
        s->count[k] = __atomic_load_n(&nv2a_stats_count_[k], __ATOMIC_RELAXED);
    s->pack_hits = nv2a_tex_pack_hits();
    /* The total, not the ring: the ring is the walker's, unlocked. */
    s->pack_misses = nv2a_tex_pack_miss_total();

    /* The newest entries that ended within the last second. */
    for (i = 0; i < RING && i < n; i++) {
        uint64_t j = (n - 1 - i) % RING;
        uint32_t end = __atomic_load_n(&s_ring_end_ms[j], __ATOMIC_RELAXED);
        uint32_t us = __atomic_load_n(&s_ring_us[j], __ATOMIC_RELAXED);
        if ((uint32_t)(now_ms - end) > 1000u)
            break;
        sum += us;
        if (us > max)
            max = us;
        s->frames++;
    }
    if (s->frames) {
        s->ms_avg = (float)((double)sum / s->frames / 1000.0);
        s->ms_max = (float)(max / 1000.0);
    }
}

unsigned nv2a_stats_frames(float *ms, unsigned n)
{
    uint64_t total = __atomic_load_n(&s_ring_n, __ATOMIC_ACQUIRE);
    unsigned k = n < RING ? n : RING;

    if (total < k)
        k = (unsigned)total;
    for (unsigned i = 0; i < k; i++) {
        uint64_t j = (total - k + i) % RING;
        ms[i] = (float)(__atomic_load_n(&s_ring_us[j], __ATOMIC_RELAXED) / 1000.0);
    }
    return k;
}

void nv2a_stats_reset_for_test(void)
{
    memset(nv2a_stats_count_, 0, sizeof nv2a_stats_count_);
    memset(s_ring_us, 0, sizeof s_ring_us);
    memset(s_ring_end_ms, 0, sizeof s_ring_end_ms);
    s_ring_n = 0;
    s_last_flip_ns = 0;
    s_users = 0;
    nv2a_stats_on = 0;
}
