/*
 * nv2a_stats.h - the runtime counters the stats strip, the menu's Stats
 * page and RECOMP_TRACE=title read.
 *
 * Built only with -DXBOXRECOMP_ENHANCE=ON; without it every site below is
 * an empty macro. Each site tests one relaxed flag, nv2a_stats_on, set
 * while something shows the counters (the strip, the Stats page, the
 * title's stats), and does nothing more while it is clear. So a run that
 * shows none of them, every golden among them, pays one load per site.
 * RECOMP_TRACE=title sets NV2A_STATS_TITLE, so the title's FPS counts the
 * same flips as the strip.
 *
 * Writers: the flip site in nv2a_pb_exec.c (flips, draws, frame time),
 * each presenter (presents), the GPU backends' texture uploads. Readers:
 * any thread, through nv2a_stats_read. Counts are relaxed atomics, so a
 * reader's snapshot is a few counts loose at most, which a 2 Hz strip
 * does not show.
 */
#ifndef NV2A_STATS_H
#define NV2A_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum nv2a_stat {
    NV2A_STAT_FLIPS, NV2A_STAT_PRESENTS, NV2A_STAT_DRAWS, NV2A_STAT_TEX_UPLOADS,
    NV2A_STAT_COUNT
};

/* Who shows the counters: the flag is set while any bit is. */
enum { NV2A_STATS_STRIP = 1, NV2A_STATS_PAGE = 2, NV2A_STATS_TITLE = 4 };

#ifdef XBOXRECOMP_ENHANCE

extern int nv2a_stats_on;                    /* relaxed */
extern uint64_t nv2a_stats_count_[NV2A_STAT_COUNT];

static inline int nv2a_stats_enabled(void)
{
    return __atomic_load_n(&nv2a_stats_on, __ATOMIC_RELAXED);
}

#define nv2a_stats_add(k, n) do { \
        if (nv2a_stats_enabled()) \
            __atomic_add_fetch(&nv2a_stats_count_[(k)], (uint64_t)(n), __ATOMIC_RELAXED); \
    } while (0)

/* One counter, relaxed (the title's flips). */
static inline uint64_t nv2a_stats_count(enum nv2a_stat k)
{
    return __atomic_load_n(&nv2a_stats_count_[k], __ATOMIC_RELAXED);
}

/* A user of the counters starts or stops showing them. */
void nv2a_stats_use(unsigned who, int on);

/* At the flip: the flip, the frame's draws, and the time since the last
 * flip into the frame-time ring (128 entries). now_ns is monotonic. A
 * macro, like nv2a_stats_add, so the clock read in its arguments is not
 * made while the flag is clear. */
void nv2a_stats_flip_(uint32_t draws, uint64_t now_ns);
#define nv2a_stats_flip(d, t) do { \
        if (nv2a_stats_enabled()) \
            nv2a_stats_flip_((d), (t)); \
    } while (0)

struct nv2a_stats_snap {
    uint64_t count[NV2A_STAT_COUNT];
    uint64_t pack_hits, pack_misses;
    /* Frame times of the flips in the last second of the ring. */
    unsigned frames;
    float ms_avg, ms_max;
};
void nv2a_stats_read(struct nv2a_stats_snap *s, uint64_t now_ns);
/* The newest frame times of the ring, in ms, oldest first (the Stats
 * page's graph); returns how many, at most n and 128. */
unsigned nv2a_stats_frames(float *ms, unsigned n);

/* ctest only. */
void nv2a_stats_reset_for_test(void);

#else

#define nv2a_stats_add(k, n) ((void)0)
#define nv2a_stats_flip(d, t) ((void)0)
#define nv2a_stats_use(w, on) ((void)0)

#endif

#ifdef __cplusplus
}
#endif

#endif /* NV2A_STATS_H */
