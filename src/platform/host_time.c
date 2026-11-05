/*
 * host_time.c - see host_time.h.
 *
 * Windows (and Wine): QueryPerformanceCounter, and a high-resolution
 * waitable timer per thread (Windows 10 1803+; Wine has it too), which waits
 * to the 100 ns unit where Sleep rounds to the system tick. Without one,
 * Sleep in whole milliseconds, rounded up. The short nap (xbox_HostNapNs)
 * takes the same timer for its cold naps on native Windows only and spins
 * its hot ones: see host_time.h for why, and why Wine keeps
 * NtDelayExecution.
 * POSIX: CLOCK_MONOTONIC and nanosleep.
 */
#include "host_time.h"

#include <stdlib.h>

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

#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
/* The calling thread's high-resolution waitable timer, created on first use;
 * NULL where the host has none (a Windows before 1803 refuses the flag with
 * ERROR_INVALID_PARAMETER), and then NULL for good on this thread. */
static HANDLE thread_timer(void)
{
    static HOST_THREAD_LOCAL HANDLE timer;
    static HOST_THREAD_LOCAL int timer_failed;

    if (!timer && !timer_failed) {
        timer = CreateWaitableTimerExW(NULL, NULL,
                                       CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_ALL_ACCESS);
        timer_failed = !timer;
    }
    return timer;
}

/* Wait `ns` on the thread's timer; 0 when there is none or it failed. */
static int timer_wait(uint64_t ns)
{
    HANDLE timer = thread_timer();
    LARGE_INTEGER due;

    if (!timer)
        return 0;
    due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
    return SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE) &&
           WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0;
}
#endif

void xbox_HostSleepNs(uint64_t ns)
{
    if (!ns)
        return;
#ifdef _WIN32
    if (!timer_wait(ns))
        Sleep((DWORD)((ns + 999999u) / 1000000u));
#else
    {
        struct timespec ts = { (time_t)(ns / 1000000000u),
                               (long)(ns % 1000000000u) };
        nanosleep(&ts, NULL);
    }
#endif
}

#ifdef _WIN32
/* ntdll's exports the naps need, looked up once: wine_get_version says the
 * process runs under Wine, and NtDelayExecution is found at run time so the
 * mingw build needs no ntdll import. */
typedef LONG (WINAPI *nt_delay_fn)(BOOLEAN, PLARGE_INTEGER);
static INIT_ONCE s_ntdll_once = INIT_ONCE_STATIC_INIT;
static int s_under_wine;
static nt_delay_fn s_nt_delay;

static BOOL CALLBACK ntdll_init(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    HMODULE nt = GetModuleHandleA("ntdll.dll");

    (void)once; (void)param; (void)ctx;
    if (nt) {
        s_under_wine = GetProcAddress(nt, "wine_get_version") != NULL;
        s_nt_delay = (nt_delay_fn)(void *)GetProcAddress(nt, "NtDelayExecution");
    }
    return TRUE;
}
#endif

int xbox_HostUnderWine(void)
{
#ifdef _WIN32
    InitOnceExecuteOnce(&s_ntdll_once, ntdll_init, NULL, NULL);
    return s_under_wine;
#else
    return 0;
#endif
}

/* The mode the naps take: AUTO until set or first used. Resolving AUTO twice
 * at once is harmless (both threads resolve it the same way), but the word
 * is read and written atomically so it is not a data race. */
#ifdef _WIN32
static volatile LONG s_nap_mode = XBOX_NAP_AUTO;
static volatile LONG s_nap_no_timer;
#define NAP_LOAD(v)     InterlockedCompareExchange(&(v), 0, 0)
#define NAP_STORE(v, x) InterlockedExchange(&(v), (LONG)(x))
#else
static int s_nap_mode = XBOX_NAP_AUTO;
static int s_nap_no_timer;
#define NAP_LOAD(v)     __atomic_load_n(&(v), __ATOMIC_ACQUIRE)
#define NAP_STORE(v, x) __atomic_store_n(&(v), (int)(x), __ATOMIC_RELEASE)
#endif

static XboxNapMode nap_auto(void)
{
#ifdef _WIN32
    return xbox_HostUnderWine() ? XBOX_NAP_DELAY : XBOX_NAP_HYBRID;
#else
    return XBOX_NAP_DELAY;
#endif
}

void xbox_HostNapModeSet(XboxNapMode mode)
{
    NAP_STORE(s_nap_mode, mode == XBOX_NAP_AUTO ? nap_auto() : mode);
}

XboxNapMode xbox_HostNapMode(void)
{
    XboxNapMode m = (XboxNapMode)NAP_LOAD(s_nap_mode);

    if (m == XBOX_NAP_AUTO) {
        m = nap_auto();
        NAP_STORE(s_nap_mode, m);
    }
    return m;
}

int xbox_HostNapHasTimer(void)
{
    return !NAP_LOAD(s_nap_no_timer);
}

static void cpu_relax(void)
{
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

void xbox_HostNapNs(uint64_t ns, int hot)
{
    XboxNapMode m;

    if (!ns)
        return;
    m = xbox_HostNapMode();
    if (m == XBOX_NAP_HYBRID)
        m = hot ? XBOX_NAP_SPIN : XBOX_NAP_TIMER;
    if (m == XBOX_NAP_SPIN) {
        /* Bounded by the clock, never by a count. */
        uint64_t end = xbox_HostNowNs() + ns;
        while (xbox_HostNowNs() < end)
            cpu_relax();
        return;
    }
#ifdef _WIN32
    if (m == XBOX_NAP_TIMER && !NAP_LOAD(s_nap_no_timer)) {
        if (timer_wait(ns))
            return;
        if (!thread_timer()) {
            /* No high-resolution timer: the cold naps are NtDelayExecution
             * for the process; a TIMER mode says so, HYBRID keeps spinning
             * its hot naps. */
            NAP_STORE(s_nap_no_timer, 1);
            if (NAP_LOAD(s_nap_mode) == XBOX_NAP_TIMER)
                NAP_STORE(s_nap_mode, XBOX_NAP_DELAY);
        }
    }
    InitOnceExecuteOnce(&s_ntdll_once, ntdll_init, NULL, NULL);
    if (s_nt_delay) {
        LARGE_INTEGER t;
        t.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
        s_nt_delay(FALSE, &t);
    } else {
        Sleep(1);
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

/* The walker's waker (host_time.h). The same latch as the timer thread's,
 * kept in a flag under the lock on both hosts. */
#ifdef _WIN32
/* Two forms. Under Wine an SRW lock and a condition variable, both
 * in-process futexes; a waitable timer there is a wineserver object, two
 * round trips a sleep (host_time.h, the naps). On native Windows the
 * condition variable's timeout is whole milliseconds on the system clock
 * interval, 15.6 ms when nothing has raised the timer resolution, which
 * would make the walker's 1 ms tick a 15.6 ms one: there an auto-reset
 * event latches the wake and the high-resolution waitable timer bounds the
 * wait, at most one interval (0.5-1 ms) late. A native host without that
 * timer (before Windows 10 1803) gets no waker, and the walker polls. */
struct XboxHostWaker {
    SRWLOCK lock;
    CONDITION_VARIABLE cv;
    int pending;
    HANDLE ev;                  /* native Windows: auto-reset, the latch */
    HANDLE timer;               /* native Windows: high-resolution */
};

XboxHostWaker *xbox_HostWakerCreateForm(XboxHostWakerForm form)
{
    XboxHostWaker *w;

    if (form == XBOX_WAKER_AUTO)
        form = xbox_HostUnderWine() ? XBOX_WAKER_CONDVAR : XBOX_WAKER_EVENT;
    w = (XboxHostWaker *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    InitializeSRWLock(&w->lock);
    InitializeConditionVariable(&w->cv);
    if (form == XBOX_WAKER_EVENT) {
        w->ev = CreateEventW(NULL, FALSE, FALSE, NULL);
        w->timer = CreateWaitableTimerExW(NULL, NULL,
                                          CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
        if (!w->ev || !w->timer) {
            if (w->ev)
                CloseHandle(w->ev);
            if (w->timer)
                CloseHandle(w->timer);
            free(w);
            return NULL;
        }
    }
    return w;
}

int xbox_HostWakerSleepNs(XboxHostWaker *w, uint64_t ns)
{
    int woke;

    if (w->ev) {
        HANDLE hs[2];
        LARGE_INTEGER due;

        if (!ns)
            return WaitForSingleObject(w->ev, 0) == WAIT_OBJECT_0;
        due.QuadPart = -(LONGLONG)((ns + 99u) / 100u);   /* relative, 100 ns */
        if (!SetWaitableTimer(w->timer, &due, 0, NULL, NULL, FALSE))
            return WaitForSingleObject(w->ev, 1) == WAIT_OBJECT_0;
        hs[0] = w->ev;          /* first: both signalled reports the wake */
        hs[1] = w->timer;
        return WaitForMultipleObjects(2, hs, FALSE, INFINITE) == WAIT_OBJECT_0;
    }
    AcquireSRWLockExclusive(&w->lock);
    if (ns && !w->pending) {
        /* Rounded up: a wait shorter than asked would be a spin. Wine's
         * futex wait honours the millisecond. */
        DWORD ms = (DWORD)((ns + 999999u) / 1000000u);
        SleepConditionVariableSRW(&w->cv, &w->lock, ms, 0);
    }
    woke = w->pending;
    w->pending = 0;
    ReleaseSRWLockExclusive(&w->lock);
    return woke;
}

void xbox_HostWakerWake(XboxHostWaker *w)
{
    if (w->ev) {
        SetEvent(w->ev);
        return;
    }
    AcquireSRWLockExclusive(&w->lock);
    w->pending = 1;
    ReleaseSRWLockExclusive(&w->lock);
    WakeConditionVariable(&w->cv);
}
#else
struct XboxHostWaker {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    int pending;
};

XboxHostWaker *xbox_HostWakerCreateForm(XboxHostWakerForm form)
{
    XboxHostWaker *w;

    if (form == XBOX_WAKER_EVENT)
        return NULL;
    w = (XboxHostWaker *)calloc(1, sizeof *w);
    if (!w)
        return NULL;
    pthread_mutex_init(&w->lock, NULL);
#ifdef __APPLE__
    pthread_cond_init(&w->cv, NULL);
#else
    {
        pthread_condattr_t a;
        pthread_condattr_init(&a);
        pthread_condattr_setclock(&a, CLOCK_MONOTONIC);
        pthread_cond_init(&w->cv, &a);
        pthread_condattr_destroy(&a);
    }
#endif
    return w;
}

int xbox_HostWakerSleepNs(XboxHostWaker *w, uint64_t ns)
{
    int woke;

    pthread_mutex_lock(&w->lock);
    if (ns && !w->pending) {
#ifdef __APPLE__
        struct timespec rel = { (time_t)(ns / 1000000000u),
                                (long)(ns % 1000000000u) };
        pthread_cond_timedwait_relative_np(&w->cv, &w->lock, &rel);
#else
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += (time_t)(ns / 1000000000u);
        ts.tv_nsec += (long)(ns % 1000000000u);
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec += 1;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&w->cv, &w->lock, &ts);
#endif
    }
    woke = w->pending;
    w->pending = 0;
    pthread_mutex_unlock(&w->lock);
    return woke;
}

void xbox_HostWakerWake(XboxHostWaker *w)
{
    pthread_mutex_lock(&w->lock);
    w->pending = 1;
    pthread_cond_signal(&w->cv);
    pthread_mutex_unlock(&w->lock);
}
#endif

XboxHostWaker *xbox_HostWakerCreate(void)
{
    return xbox_HostWakerCreateForm(XBOX_WAKER_AUTO);
}

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
