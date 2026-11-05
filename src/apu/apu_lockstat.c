/*
 * RECOMP_TRACE=apu_lock (apu_lockstat.h): d->lock hold and wait statistics.
 *
 *   [APU-LOCK] t=<s> holder=<name> holds=<n> total=<ms> max=<us> h=<log2 us buckets>
 *   [APU-LOCK] t=<s> taker=<name> takes=<n> contended=<n> wait=<ms> max=<us> h=<...>
 *   [APU-LOCK] t=<s> se_frames=<n> se_us=<mean> se_max=<us> pace_waits=<n>
 *
 * Buckets are [0,1) [1,2) [2,4) ... us, the last open-ended; counts are for
 * the 5 s window. Only one thread holds the lock at a time, so one start
 * time serves every holder.
 */

#include "apu_state.h"
#include "apu_lockstat.h"
#include "recomp_env.h"
#include <stdio.h>
#include <string.h>

#define LS_BUCKETS 16            /* up to [16.4 ms, inf) */
#define LS_WINDOW_NS 5000000000ull

typedef struct {
    uint64_t n, contended, tot_ns, max_ns;
    uint64_t h[LS_BUCKETS];
} LsSum;

static const char *const s_name[APU_LOCK_HOLDERS] = { "frame", "other" };
static const char *const s_stage_name[APU_STAGES] = { "vp", "dsp", "mon", "xa2" };

int apu_lockstat_on_ = -1;
static LsSum s_hold[APU_LOCK_HOLDERS], s_wait[APU_LOCK_HOLDERS], s_stage[APU_STAGES];
static uint64_t s_hold_start;
static uint64_t s_se_n, s_se_ns, s_se_max, s_pace_waits;
static uint64_t s_t0, s_last;
static char s_text[4096];
static volatile int s_text_ready;

static LARGE_INTEGER s_freq;

/* Every apu_lockstat_now_ns caller checks apu_lockstat_on() first, so the
 * frequency is read here, before the first timestamp. Another thread can
 * still see apu_lockstat_on_ before s_freq on a weakly ordered host (arm64),
 * so apu_lockstat_now_ns reads it itself when it finds zero. */
int apu_lockstat_lookup(void)
{
    QueryPerformanceFrequency(&s_freq);
    apu_lockstat_on_ = recomp_env_on(RENV_APU_LOCK_TRACE);
    return apu_lockstat_on_;
}

uint64_t apu_lockstat_now_ns(void)
{
    LARGE_INTEGER f = s_freq, c;
    if (!f.QuadPart)
        QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (uint64_t)(c.QuadPart / f.QuadPart) * 1000000000ull +
           (uint64_t)(c.QuadPart % f.QuadPart) * 1000000000ull / (uint64_t)f.QuadPart;
}

static void ls_add(LsSum *s, uint64_t ns)
{
    uint64_t us = ns / 1000;
    int b = 0;
    while (us && b < LS_BUCKETS - 1) {
        us >>= 1;
        b++;
    }
    s->n++;
    s->tot_ns += ns;
    if (ns > s->max_ns)
        s->max_ns = ns;
    s->h[b]++;
}

void apu_lockstat_lock(MCPXAPUState *d, int who)
{
    if (!apu_lockstat_on()) {
        qemu_mutex_lock(&d->lock);
        return;
    }
    if (TryEnterCriticalSection(&d->lock.cs)) {
        ls_add(&s_wait[who], 0);
    } else {
        uint64_t t0 = apu_lockstat_now_ns();
        qemu_mutex_lock(&d->lock);
        ls_add(&s_wait[who], apu_lockstat_now_ns() - t0);
        s_wait[who].contended++;
    }
    s_hold_start = apu_lockstat_now_ns();
}

void apu_lockstat_hold_end(int who)
{
    if (apu_lockstat_on() && s_hold_start) {
        ls_add(&s_hold[who], apu_lockstat_now_ns() - s_hold_start);
        s_hold_start = 0;
    }
}

void apu_lockstat_hold_begin(int who)
{
    (void)who;
    if (apu_lockstat_on())
        s_hold_start = apu_lockstat_now_ns();
}

void apu_lockstat_unlock(MCPXAPUState *d, int who)
{
    apu_lockstat_hold_end(who);
    qemu_mutex_unlock(&d->lock);
}

void apu_lockstat_se_frame(uint64_t ns)
{
    s_se_n++;
    s_se_ns += ns;
    if (ns > s_se_max)
        s_se_max = ns;
}

void apu_lockstat_stage(int stage, uint64_t ns)
{
    ls_add(&s_stage[stage], ns);
}

void apu_lockstat_pace_wait(void)
{
    s_pace_waits++;
}

/* snprintf returns what it would have written, so a line that does not fit
 * would carry k past room and the next call would write past the buffer;
 * LS_ADD stops at the end instead. Returns what was written. */
#define LS_ADD(...)                                                        \
    do {                                                                   \
        k += snprintf(p + k, room - k, __VA_ARGS__);                       \
        if (k >= room)                                                     \
            return room - 1;                                               \
    } while (0)

static size_t ls_fmt(char *p, size_t room, const char *kind, const char *name,
                     const char *count, const LsSum *s, double t, int waits)
{
    size_t k = 0;

    if (room < 2)
        return 0;
    LS_ADD("[APU-LOCK] t=%.0f %s=%s %s=%llu", t, kind, name, count,
           (unsigned long long)s->n);
    if (waits)
        LS_ADD(" contended=%llu wait=%.2fms", (unsigned long long)s->contended,
               s->tot_ns / 1e6);
    else
        LS_ADD(" total=%.2fms", s->tot_ns / 1e6);
    LS_ADD(" max=%lluus h=", (unsigned long long)(s->max_ns / 1000));
    for (int b = 0; b < LS_BUCKETS; b++)
        LS_ADD(b ? ",%llu" : "%llu", (unsigned long long)s->h[b]);
    LS_ADD("\n");
    return k;
}

void apu_lockstat_report(void)
{
    uint64_t now;
    size_t k = 0;
    double t;

    if (!apu_lockstat_on() || s_text_ready)
        return;
    now = apu_lockstat_now_ns();
    if (!s_t0)
        s_t0 = s_last = now;
    if (now - s_last < LS_WINDOW_NS)
        return;
    s_last = now;
    t = (now - s_t0) / 1e9;
    for (int i = 0; i < APU_LOCK_HOLDERS; i++)
        if (s_hold[i].n && k < sizeof(s_text) - 1)
            k += ls_fmt(s_text + k, sizeof(s_text) - k, "holder", s_name[i],
                        "holds", &s_hold[i], t, 0);
    for (int i = 0; i < APU_LOCK_HOLDERS; i++)
        if (s_wait[i].n && k < sizeof(s_text) - 1)
            k += ls_fmt(s_text + k, sizeof(s_text) - k, "taker", s_name[i],
                        "takes", &s_wait[i], t, 1);
    for (int i = 0; i < APU_STAGES; i++)
        if (s_stage[i].n && k < sizeof(s_text) - 1)
            k += ls_fmt(s_text + k, sizeof(s_text) - k, "stage", s_stage_name[i],
                        "runs", &s_stage[i], t, 0);
    if (k < sizeof(s_text) - 1)
        snprintf(s_text + k, sizeof(s_text) - k,
                 "[APU-LOCK] t=%.0f se_frames=%llu se_us=%.1f se_max=%lluus pace_waits=%llu\n",
                 t, (unsigned long long)s_se_n,
                 s_se_n ? s_se_ns / 1e3 / (double)s_se_n : 0.0,
                 (unsigned long long)(s_se_max / 1000), (unsigned long long)s_pace_waits);
    memset(s_hold, 0, sizeof(s_hold));
    memset(s_wait, 0, sizeof(s_wait));
    memset(s_stage, 0, sizeof(s_stage));
    s_se_n = s_se_ns = s_se_max = s_pace_waits = 0;
    s_text_ready = 1;
}

void apu_lockstat_flush(void)
{
    if (!s_text_ready)
        return;
    fputs(s_text, stderr);
    fflush(stderr);
    s_text_ready = 0;
}
