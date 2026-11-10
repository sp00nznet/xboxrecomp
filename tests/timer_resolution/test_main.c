/*
 * xbox_HostTimerResolutionInit (host_time.c).
 *
 * Times the waits the runtime makes with a timeout: a 1 ms sleep, and a
 * condition-variable wait of 1 ms and of 6 ms (the APU frame thread's pacing
 * wait for one 5.3 ms EP period, apu_core.c throttle), first as the process
 * starts, then after the init. On native Windows the first set is 15.6 ms
 * each (the default clock interval); it is printed, not checked, since the
 * figure depends on the host and on Windows before 10 2004 another program's
 * request leaks in. After the init, on Windows, each wait must end within a
 * few ms of what it asked for; POSIX asks nothing and only prints them. The
 * bound is p50 only: a loaded host makes the tail noise.
 */
#include "host_time.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
static CRITICAL_SECTION cs;
static CONDITION_VARIABLE cv;
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
#endif

#define WAITS 40

static int failures;

static double now_ms(void)
{
    return (double)xbox_HostNowNs() / 1e6;
}

static int cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static void sleep_ms(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

/* A timed wait on a condition variable nobody signals, as the APU's
 * throttle waits on d->cond (apu_shim.h: SleepConditionVariableCS). */
static void cond_wait_ms(unsigned ms)
{
#ifdef _WIN32
    EnterCriticalSection(&cs);
    SleepConditionVariableCS(&cv, &cs, ms);
    LeaveCriticalSection(&cs);
#else
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_nsec += (long)ms * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&mu);
    while (pthread_cond_timedwait(&cv, &mu, &ts) != ETIMEDOUT)
        ;
    pthread_mutex_unlock(&mu);
#endif
}

/* p50 of WAITS waits of `ms`. */
static double time_waits(int cond, unsigned ms, const char *phase)
{
    static double t[WAITS];
    int i;

    for (i = 0; i < WAITS; i++) {
        double t0 = now_ms();
        if (cond)
            cond_wait_ms(ms);
        else
            sleep_ms(ms);
        t[i] = now_ms() - t0;
    }
    qsort(t, WAITS, sizeof t[0], cmp);
    printf("  %-6s %-20s %2d waits of %u ms: p50 %.2f ms, max %.2f ms\n",
           phase, cond ? "condition variable" : "sleep", WAITS, ms,
           t[WAITS / 2], t[WAITS - 1]);
    return t[WAITS / 2];
}

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* A timing bound: checked on Windows, printed elsewhere. */
static void timed_check(int ok, const char *what)
{
#ifdef _WIN32
    check(ok, what);
#else
    (void)ok;
    printf("  %-62s (not checked)\n", what);
#endif
}

int main(void)
{
    char what[96];
    double p;
    int ok1, ok2;

#ifdef _WIN32
    InitializeCriticalSection(&cs);
    InitializeConditionVariable(&cv);
    printf("  host: %s\n", xbox_HostUnderWine() ? "Wine" : "native Windows");
#else
    printf("  host: POSIX (the init asks nothing)\n");
#endif

    time_waits(0, 1, "before");
    time_waits(1, 6, "before");

    ok1 = xbox_HostTimerResolutionInit();
    ok2 = xbox_HostTimerResolutionInit();     /* a second call does nothing */
    check(ok1 == 1, "the 1 ms period is granted");
    check(ok2 == ok1, "a second call returns what the first did");

    /* The bounds hold the native Windows waits the period is for. Elsewhere
     * nothing is asked and the timings are only printed: macOS wakes a 6 ms
     * condition wait at 8-9 ms p50, too near any bound worth checking. */
    p = time_waits(0, 1, "after");
    snprintf(what, sizeof what, "1 ms sleep after the init: p50 %.2f ms < 5", p);
    timed_check(p < 5.0, what);
    p = time_waits(1, 1, "after");
    snprintf(what, sizeof what, "1 ms timed wait after the init: p50 %.2f ms < 5", p);
    timed_check(p < 5.0, what);
    p = time_waits(1, 6, "after");
    snprintf(what, sizeof what, "6 ms timed wait after the init: p50 %.2f ms < 10", p);
    timed_check(p < 10.0, what);

    printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
