/*
 * The poll loop's nap (xbox_HostNapNs, host_time.c).
 *
 * The NV2A ack loop naps 50 us between polls of the KickOff flush bit and
 * DMA_PUT. Every timed wait on native Windows expires on the clock
 * interrupt: NtDelayExecution there is 1 ms under timeBeginPeriod(1), the
 * high-resolution timer 0.5 ms, so the default there spins while the loop is
 * hot and takes the timer when it is cold. This times 200 naps of 50 us per
 * primitive and bounds, on p50 only (a loaded host makes p95 and max noise),
 * what holds on every host: the spin and the hybrid's hot nap, Wine's and
 * POSIX's delay (none below the 50 us asked, since no wait returns early),
 * and a nap of 0. The timer, the native Windows delay and the hybrid's cold nap are
 * printed for the record. It also checks the kick_nap parser and that the
 * cold nap (xbox_HostNapColdNs) gives the core away under the spinning
 * modes. Under _WIN32 the process asks for
 * timeBeginPeriod(1) first, as xbox_kernel_init does in the runtime
 * (xbox_HostTimerResolutionInit).
 */
#include "host_time.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <mmsystem.h>
#else
#include <time.h>
#endif

#define NAPS 200
#define US 1000ull

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static int cmp(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

typedef struct { uint64_t p50, p95, max; } Figures;

/* NAPS naps of `ns` in the mode now set. */
static Figures time_naps(uint64_t ns, int hot, const char *label)
{
    static uint64_t t[NAPS];
    Figures f;
    int i;

    for (i = 0; i < NAPS; i++) {
        uint64_t t0 = xbox_HostNowNs();
        xbox_HostNapNs(ns, hot);
        t[i] = xbox_HostNowNs() - t0;
    }
    qsort(t, NAPS, sizeof t[0], cmp);
    f.p50 = t[NAPS / 2];
    f.p95 = t[NAPS * 95 / 100];
    f.max = t[NAPS - 1];
    if (label)
        printf("  %-14s %3d naps of %llu us: p50 %llu us, p95 %llu us, max %llu us\n",
               label, NAPS, (unsigned long long)(ns / US),
               (unsigned long long)(f.p50 / US), (unsigned long long)(f.p95 / US),
               (unsigned long long)(f.max / US));
    return f;
}

/* The process's CPU time, ns. */
static uint64_t cpu_ns(void)
{
#ifdef _WIN32
    FILETIME c, e, k, u;
    ULARGE_INTEGER kk, uu;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    kk.LowPart = k.dwLowDateTime; kk.HighPart = k.dwHighDateTime;
    uu.LowPart = u.dwLowDateTime; uu.HighPart = u.dwHighDateTime;
    return (kk.QuadPart + uu.QuadPart) * 100u;
#else
    struct timespec ts;
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

/* 40 cold naps of 2 ms in the mode now set: CPU time over wall time, %. */
static unsigned cold_cpu_pct(void)
{
    uint64_t w0 = xbox_HostNowNs(), c0 = cpu_ns(), w;
    int i;

    for (i = 0; i < 40; i++)
        xbox_HostNapColdNs(2000 * US);
    w = xbox_HostNowNs() - w0;
    return w ? (unsigned)((cpu_ns() - c0) * 100u / w) : 100u;
}

static const char *mode_name(XboxNapMode m)
{
    switch (m) {
    case XBOX_NAP_TIMER:  return "timer";
    case XBOX_NAP_DELAY:  return "delay";
    case XBOX_NAP_SPIN:   return "spin";
    case XBOX_NAP_HYBRID: return "hybrid";
    case XBOX_NAP_YIELD:  return "yield";
    case XBOX_NAP_SPINDELAY: return "spindelay";
    default:              return "auto";
    }
}

int main(void)
{
    char what[112];
    Figures f;
    int wine = xbox_HostUnderWine(), native_win = 0;
    XboxNapMode expect;

#ifdef _WIN32
    timeBeginPeriod(1);
    {
        HMODULE nt = GetModuleHandleA("ntdll.dll");
        int direct = nt && GetProcAddress(nt, "wine_get_version");
        snprintf(what, sizeof what, "under Wine %d, as ntdll's wine_get_version says (%d)",
                 wine, direct);
        check(wine == direct, what);
    }
    native_win = !wine;
#else
    snprintf(what, sizeof what, "under Wine %d on POSIX", wine);
    check(wine == 0, what);
#endif

    xbox_HostNapModeSet(XBOX_NAP_AUTO);
    expect = native_win ? XBOX_NAP_HYBRID : XBOX_NAP_DELAY;
    snprintf(what, sizeof what, "auto is %s (expected %s)",
             mode_name(xbox_HostNapMode()), mode_name(expect));
    check(xbox_HostNapMode() == expect, what);

    xbox_HostNapModeSet(XBOX_NAP_SPIN);
    f = time_naps(50 * US, 0, "spin");
    /* None of the waits may return early: a nap is at least its length. */
    snprintf(what, sizeof what, "spin (the hybrid's hot nap): 50 <= p50 %llu us < 100",
             (unsigned long long)(f.p50 / US));
    check(f.p50 >= 50 * US && f.p50 < 100 * US, what);

    xbox_HostNapModeSet(XBOX_NAP_DELAY);
    f = time_naps(50 * US, 0, "delay");
    if (native_win) {
        printf("  %-62s %s\n", "delay on native Windows: printed, not bounded", "--");
    } else {
        /* The ceiling guards against millisecond rounding (the old native
         * Windows nap), not a loaded host's scheduling, which can reach
         * several hundred us. */
        snprintf(what, sizeof what, "delay: 50 <= p50 %llu us < 900",
                 (unsigned long long)(f.p50 / US));
        check(f.p50 >= 50 * US && f.p50 < 900 * US, what);
    }

    xbox_HostNapModeSet(XBOX_NAP_TIMER);
    time_naps(50 * US, 0, "timer");
    printf("  %-14s mode in effect after the timer run: %s%s\n", "",
           mode_name(xbox_HostNapMode()),
           xbox_HostNapHasTimer() ? "" : " (no high-resolution timer)");

    xbox_HostNapModeSet(XBOX_NAP_HYBRID);
    f = time_naps(50 * US, 1, "hybrid hot");
    snprintf(what, sizeof what, "hybrid hot: p50 %llu us >= 50",
             (unsigned long long)(f.p50 / US));
    check(f.p50 >= 50 * US, what);
    time_naps(50 * US, 0, "hybrid cold");

    /* The clock-wait arms: yield is bounded by the clock as the spin is;
     * spindelay's hot nap is the spin and its cold one the delay. */
    xbox_HostNapModeSet(XBOX_NAP_YIELD);
    f = time_naps(50 * US, 0, "yield");
    snprintf(what, sizeof what, "yield: 50 <= p50 %llu us < 900",
             (unsigned long long)(f.p50 / US));
    check(f.p50 >= 50 * US && f.p50 < 900 * US, what);

    xbox_HostNapModeSet(XBOX_NAP_SPINDELAY);
    f = time_naps(50 * US, 1, "spindelay hot");
    snprintf(what, sizeof what, "spindelay hot (the spin): 50 <= p50 %llu us < 100",
             (unsigned long long)(f.p50 / US));
    check(f.p50 >= 50 * US && f.p50 < 100 * US, what);
    f = time_naps(50 * US, 0, "spindelay cold");
    snprintf(what, sizeof what, "spindelay cold (the delay): p50 %llu us >= 50",
             (unsigned long long)(f.p50 / US));
    check(f.p50 >= 50 * US, what);

    /* The kick_nap values (docs/env.md). */
    {
        static const struct {
            const char *s; int ok; XboxNapMode mode; unsigned us;
        } cases[] = {
            { "hybrid", 1, XBOX_NAP_HYBRID, 500 },
            { "timer", 1, XBOX_NAP_TIMER, 500 },
            { "delay", 1, XBOX_NAP_DELAY, 500 },
            { "spin", 1, XBOX_NAP_SPIN, 500 },
            { "yield", 1, XBOX_NAP_YIELD, 500 },
            { "spindelay", 1, XBOX_NAP_SPINDELAY, 500 },
            { "spindelay:250", 1, XBOX_NAP_SPINDELAY, 250 },
            { "spindelay:1000000", 1, XBOX_NAP_SPINDELAY, 1000000 },
            { "spindelay:0", -1, XBOX_NAP_SPINDELAY, 500 },
            { "spindelay:1000001", -1, XBOX_NAP_SPINDELAY, 500 },
            { "spindelay:", -1, XBOX_NAP_SPINDELAY, 500 },
            { "spindelay:9x", -1, XBOX_NAP_SPINDELAY, 500 },
            { "spindelayx", 0, XBOX_NAP_AUTO, 500 },
            { "spinx", 0, XBOX_NAP_AUTO, 500 },
            { "", 0, XBOX_NAP_AUTO, 500 },
            { NULL, 0, XBOX_NAP_AUTO, 500 },
        };
        size_t i;
        int bad = 0;
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            XboxNapMode m = XBOX_NAP_AUTO;
            unsigned us = 0;
            int ok = xbox_HostNapModeParse(cases[i].s, &m, &us);
            if (ok != cases[i].ok || us != cases[i].us
                || (ok && m != cases[i].mode)) {
                printf("    parse \"%s\": %d %s %u\n",
                       cases[i].s ? cases[i].s : "(null)", ok, mode_name(m), us);
                bad++;
            }
        }
        snprintf(what, sizeof what, "kick_nap parser: %d of %d cases wrong", bad,
                 (int)(sizeof cases / sizeof cases[0]));
        check(!bad, what);
    }

    /* The cold nap sleeps whatever the mode: the flip log's kick observer
     * must not spin with the walker's A/B arm. A spin is ~100% of a core. */
    {
        static const XboxNapMode modes[] = { XBOX_NAP_SPIN, XBOX_NAP_YIELD,
                                             XBOX_NAP_SPINDELAY };
        size_t i;
        for (i = 0; i < sizeof modes / sizeof modes[0]; i++) {
            unsigned pct;
            xbox_HostNapModeSet(modes[i]);
            pct = cold_cpu_pct();
            snprintf(what, sizeof what, "cold nap under %s: CPU %u%% of wall < 50%%",
                     mode_name(modes[i]), pct);
            check(pct < 50, what);
        }
    }

    xbox_HostNapModeSet(XBOX_NAP_AUTO);
    {
        Figures z0 = time_naps(0, 0, NULL), z1 = time_naps(0, 1, NULL);
        snprintf(what, sizeof what, "a nap of 0 returns at once: p50 %llu / %llu us < 50",
                 (unsigned long long)(z0.p50 / US), (unsigned long long)(z1.p50 / US));
        check(z0.p50 < 50 * US && z1.p50 < 50 * US, what);
    }

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
