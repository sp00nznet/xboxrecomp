/*
 * host_time.c - see host_time.h.
 *
 * Windows (and Wine): QueryPerformanceCounter, and a high-resolution
 * waitable timer per thread (Windows 10 1803+; Wine has it too), which waits
 * to the 100 ns unit where Sleep rounds to the system tick. Without one,
 * Sleep in whole milliseconds, rounded up.
 * POSIX: CLOCK_MONOTONIC and nanosleep.
 */
#include "host_time.h"

#ifdef _WIN32
#include <windows.h>
/* Not xbox_winnt.h's XBOX_THREAD_LOCAL: that header is the POSIX build's
 * Win32 vocabulary and is not included on Windows here. */
#if defined(_MSC_VER)
#define HOST_THREAD_LOCAL __declspec(thread)
#else
#define HOST_THREAD_LOCAL _Thread_local
#endif
#else
#include <time.h>
#include <pthread.h>
#ifdef __APPLE__
#include <stdio.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#endif
#endif

uint64_t xbox_HostNowNs(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER t;

    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t);
    /* Split so the product never overflows: QPC runs at 10 MHz under Wine
     * and on most hosts, and t * 1e9 would wrap after 29 minutes. */
    return (uint64_t)(t.QuadPart / freq.QuadPart) * 1000000000ull
           + (uint64_t)(t.QuadPart % freq.QuadPart) * 1000000000ull
             / (uint64_t)freq.QuadPart;
#else
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

void xbox_HostSleepNs(uint64_t ns)
{
    if (!ns)
        return;
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
    {
        static HOST_THREAD_LOCAL HANDLE timer;
        static HOST_THREAD_LOCAL int timer_failed;

        if (!timer && !timer_failed) {
            timer = CreateWaitableTimerExW(NULL, NULL,
                                           CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                           TIMER_ALL_ACCESS);
            timer_failed = !timer;
        }
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
            if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
                WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0)
                return;
        }
        Sleep((DWORD)((ns + 999999u) / 1000000u));
    }
#else
    {
        struct timespec ts = { (time_t)(ns / 1000000000u),
                               (long)(ns % 1000000000u) };
        nanosleep(&ts, NULL);
    }
#endif
}

/* The timer thread's wakeable sleep. The deadline is the instant the
 * nanosleep would have ended at (the same clock, the same slop on both
 * hosts), so the vblank schedule keeps its edges; what changes is that
 * KeInsertQueueDpc can end it early. On Windows the high-resolution waitable
 * timer stays, with an auto-reset event beside it: an event signalled before
 * the wait stays signalled, which is the latch. POSIX latches it in a flag
 * under the condition variable's mutex. A wait that returns early for any
 * other reason (a spurious wake-up) costs the caller one pass, which it
 * handles anyway. */
#ifdef _WIN32
static HANDLE s_timer_wake;             /* auto-reset */
static INIT_ONCE s_timer_wake_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK timer_wake_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    (void)once; (void)param; (void)ctx;
    s_timer_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    return TRUE;
}

void xbox_HostTimerSleepNs(uint64_t ns)
{
    static HOST_THREAD_LOCAL HANDLE timer;
    static HOST_THREAD_LOCAL int timer_failed;
    HANDLE hs[2];

    InitOnceExecuteOnce(&s_timer_wake_once, timer_wake_init, NULL, NULL);
    if (!ns || !s_timer_wake) {
        xbox_HostSleepNs(ns);
        return;
    }
    if (!timer && !timer_failed) {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        timer_failed = !timer;
    }
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);
        if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) {
            hs[0] = s_timer_wake;
            hs[1] = timer;
            WaitForMultipleObjects(2, hs, FALSE, INFINITE);
            return;
        }
    }
    WaitForSingleObject(s_timer_wake, (DWORD)((ns + 999999u) / 1000000u));
}

void xbox_HostTimerWake(void)
{
    InitOnceExecuteOnce(&s_timer_wake_once, timer_wake_init, NULL, NULL);
    if (s_timer_wake)
        SetEvent(s_timer_wake);
}
#else
static pthread_mutex_t s_timer_wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_timer_wake_cv;
static pthread_once_t s_timer_wake_once = PTHREAD_ONCE_INIT;
static int s_timer_wake_pending;

static void timer_wake_init(void)
{
#ifdef __APPLE__
    /* No pthread_condattr_setclock: the relative wait below is on the
     * monotonic clock already. */
    pthread_cond_init(&s_timer_wake_cv, NULL);
#else
    pthread_condattr_t a;
    pthread_condattr_init(&a);
    pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
    pthread_cond_init(&s_timer_wake_cv, &a);
    pthread_condattr_destroy(&a);
#endif
}

void xbox_HostTimerSleepNs(uint64_t ns)
{
    pthread_once(&s_timer_wake_once, timer_wake_init);
    pthread_mutex_lock(&s_timer_wake_lock);
    if (ns && !s_timer_wake_pending) {
#ifdef __APPLE__
        struct timespec rel = { (time_t)(ns / 1000000000u),
                                (long)(ns % 1000000000u) };
        pthread_cond_timedwait_relative_np(&s_timer_wake_cv,
                                           &s_timer_wake_lock, &rel);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(ns / 1000000000u);
        ts.tv_nsec += (long)(ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&s_timer_wake_cv, &s_timer_wake_lock, &ts);
#endif
    }
    s_timer_wake_pending = 0;
    pthread_mutex_unlock(&s_timer_wake_lock);
}

void xbox_HostTimerWake(void)
{
    pthread_once(&s_timer_wake_once, timer_wake_init);
    pthread_mutex_lock(&s_timer_wake_lock);
    s_timer_wake_pending = 1;
    pthread_cond_signal(&s_timer_wake_cv);
    pthread_mutex_unlock(&s_timer_wake_lock);
}
#endif

void xbox_HostTimerThreadInit(void)
{
#ifdef __APPLE__
    /* A 60 Hz period. The computation budget is generous because the timer
     * thread runs guest ISRs and DPCs between sleeps, and it is preemptible.
     * XNU demotes a real-time thread only after seconds of continuous work
     * past its budget, so a guest routine that spins on this thread can hold
     * a core for that long before the scheduler steps in. Refused (a
     * sandbox, a future macOS), the thread stays a normal one: the vblank
     * grid keeps its rate and only the wake-up jitter is the old one. */
    mach_timebase_info_data_t tb;
    thread_time_constraint_policy_data_t p;
    double per_ms;

    if (mach_timebase_info(&tb) != KERN_SUCCESS || !tb.numer)
        return;
    per_ms = 1e6 * (double)tb.denom / (double)tb.numer;
    p.period = (uint32_t)(per_ms * 1000.0 / 60.0);
    p.computation = (uint32_t)(per_ms * 2.0);
    p.constraint = (uint32_t)(per_ms * 8.0);
    p.preemptible = 1;
    {
        kern_return_t kr = thread_policy_set(
            pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
            (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
        if (kr != KERN_SUCCESS)
            fprintf(stderr, "[KERNEL] timer thread: time-constraint policy refused"
                    " (%d); vblank wake-ups may run late by a few ms\n", (int)kr);
    }
#endif
}
