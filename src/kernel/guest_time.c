/*
 * guest_time.c - see guest_time.h.
 *
 * The real-mode bodies are the ones kernel_hal.c and kernel_bridge.c had,
 * moved here unchanged, so a run that does not ask for virtual time reads
 * the same values from the same host calls as before.
 */
#include "kernel.h"
#include "guest_time.h"
#include "recomp_env.h"
#include <stdio.h>

/* ── Virtual time ──
 *
 * The guest's clock is s_vblanks periods of 10^9 / 60 ns after boot. Every
 * read below derives from that one count, so all the clocks agree with each
 * other and stand still between two vblanks.
 *
 * The performance counter runs at 10 MHz (the frequency a title reads back
 * is part of what must not differ between hosts), the wall clock starts at
 * 2004-01-01 00:00 UTC (a title that seeds a generator from the date gets
 * the same seed on every run), and the TSC keeps the console's 733 MHz. */
#define VT_PERF_HZ      10000000LL
#define VT_EPOCH_100NS  127173888000000000ULL   /* 2004-01-01 since 1601 */

static volatile LONG s_mode = -1;               /* -1 unread, 0 real, 1 virtual */
static volatile LONGLONG s_vblanks;

/* Only the 64-bit compare-exchange is in the POSIX host's set. */
static LONGLONG vt_read(void)
{
    return InterlockedCompareExchange64(&s_vblanks, 0, 0);
}

static void vt_store(LONGLONG v)
{
    LONGLONG o;

    do {
        o = vt_read();
    } while (InterlockedCompareExchange64(&s_vblanks, v, o) != o);
}

int xbox_GuestTimeVirtual(void)
{
    LONG m = s_mode;

    if (m < 0) {
        m = recomp_env_on(RENV_DETERMINISTIC) ? 1 : 0;
        if (InterlockedCompareExchange(&s_mode, m, -1) == -1 && m) {
            fprintf(stderr, "  [TIME] virtual guest clock: one 60 Hz period"
                            " per emulated vblank (deterministic)\n");
            fflush(stderr);
        }
        m = s_mode;
    }
    return (int)m;
}

void xbox_GuestTimeTestMode(int mode)
{
    InterlockedExchange(&s_mode, mode);
    vt_store(0);
}

void xbox_GuestTimeVblank(void)
{
    /* One writer, the timer thread; the store is atomic for the readers. */
    if (xbox_GuestTimeVirtual())
        vt_store(vt_read() + 1);
}

void xbox_GuestTimeSetStepper(void (*start)(void))
{
    if (start && xbox_GuestTimeVirtual())
        start();
}

uint64_t xbox_GuestTimeVblanks(void)
{
    return xbox_GuestTimeVirtual()
        ? (uint64_t)vt_read() : 0;
}

/* Virtual nanoseconds since boot. v * 10^9 overflows 64 bits at
 * v = 1.8 * 10^10 vblanks, about 9.7 years at 60 Hz: far past any run, so
 * no split is needed. */
static uint64_t vt_ns(void)
{
    /* A title that busy-waits on a clock with no kernel call in the loop
     * holds its thread until the next vblank here, and one doing it on the
     * timer thread (in a DPC) would wait for a step that thread itself has
     * to make. Say so once per thread, so a stall in this mode has a name. */
    static _Thread_local uint64_t last_v, reads;
    static _Thread_local int warned;
    uint64_t v = xbox_GuestTimeVblanks();

    if (v != last_v) {
        last_v = v;
        reads = 0;
    } else if (++reads == 1000000 && !warned) {
        warned = 1;
        fprintf(stderr, "  [TIME] a thread read the virtual clock 10^6 times"
                        " in vblank %llu: a busy-wait on a frozen clock\n",
                (unsigned long long)v);
        fflush(stderr);
    }
    return v * 1000000000ULL / 60ULL;
}

uint64_t xbox_GuestTimeMs(void)
{
    return xbox_GuestTimeVirtual() ? vt_ns() / 1000000ULL
                                   : (uint64_t)GetTickCount64();
}

uint64_t xbox_GuestTimeDue(int64_t nt_timeout)
{
    uint64_t now = xbox_GuestInterruptTime();

    if (nt_timeout < 0)
        return now + (uint64_t)(-nt_timeout);
    {
        /* Absolute: how far the system time is from it, from now. */
        uint64_t sys = xbox_GuestSystemTime();
        return (uint64_t)nt_timeout > sys ? now + ((uint64_t)nt_timeout - sys)
                                          : now;
    }
}

int xbox_GuestTimeReached(uint64_t due)
{
    return xbox_GuestInterruptTime() >= due;
}

int64_t xbox_GuestPerfCounter(void)
{
    LARGE_INTEGER counter;

    if (xbox_GuestTimeVirtual())
        return (int64_t)(vt_ns() / (1000000000ULL / VT_PERF_HZ));
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

int64_t xbox_GuestPerfFrequency(void)
{
    LARGE_INTEGER freq;

    if (xbox_GuestTimeVirtual())
        return VT_PERF_HZ;
    QueryPerformanceFrequency(&freq);
    return freq.QuadPart;
}

/* ── KeQuerySystemTime ──
 *
 * Anchored once to the wall clock, advanced by the performance counter.
 *
 * GetSystemTimeAsFileTime alone moves in steps of about 15.6 ms, the host's
 * scheduler tick. The console's clock is far finer, and a title that
 * busy-waits on this -- reading it until enough time has passed -- spins
 * for the whole of each step instead of a few iterations.
 *
 * Measured on Shin Megami Tensei: Nine: one such wait called this
 * **11.8 million times in two seconds**, which is most of what the title
 * was doing at that moment, and it came out of the spin in a state where it
 * no longer polled the gamepad.
 *
 * The anchor keeps the absolute value right; the counter supplies the
 * resolution between ticks.
 *
 * Whole seconds and the remainder are scaled separately: scaling the whole
 * count by 10^7 overflows after about a day at 10 MHz. */
static LONGLONG s_time_anchor_100ns;
static LONGLONG s_time_anchor_counts;
static LONGLONG s_time_freq_counts;
static INIT_ONCE s_time_once = INIT_ONCE_STATIC_INIT;

static BOOL CALLBACK anchor_system_time(PINIT_ONCE once, PVOID param, PVOID *ctx)
{
    FILETIME ft;
    LARGE_INTEGER f, now;
    (void)once; (void)param; (void)ctx;
    QueryPerformanceFrequency(&f);
    GetSystemTimeAsFileTime(&ft);
    QueryPerformanceCounter(&now);
    s_time_freq_counts = f.QuadPart ? f.QuadPart : 1;
    s_time_anchor_100ns = ((LONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    s_time_anchor_counts = now.QuadPart;
    return TRUE;
}

uint64_t xbox_GuestSystemTime(void)
{
    LARGE_INTEGER now;
    LONGLONG delta;

    if (xbox_GuestTimeVirtual())
        return VT_EPOCH_100NS + vt_ns() / 100ULL;
    InitOnceExecuteOnce(&s_time_once, anchor_system_time, NULL, NULL);
    QueryPerformanceCounter(&now);
    delta = now.QuadPart - s_time_anchor_counts;
    return (uint64_t)(s_time_anchor_100ns
        + (delta / s_time_freq_counts) * 10000000LL
        + (delta % s_time_freq_counts) * 10000000LL / s_time_freq_counts);
}

uint64_t xbox_GuestSystemTimeCoarse(void)
{
    FILETIME ft;

    if (xbox_GuestTimeVirtual())
        return xbox_GuestSystemTime();
    GetSystemTimeAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}

uint64_t xbox_GuestInterruptTime(void)
{
    /* GetTickCount64 is milliseconds, so scale by 10,000. Resolution is
     * coarser than the real kernel's, but it is monotonic, which is the
     * property callers actually depend on. */
    if (xbox_GuestTimeVirtual())
        return vt_ns() / 100ULL;
    return (uint64_t)GetTickCount64() * 10000ULL;
}

uint32_t xbox_GuestTickCount(void)
{
    if (xbox_GuestTimeVirtual())
        return (uint32_t)(vt_ns() / 1000000ULL);
    return (uint32_t)GetTickCount();
}

/* ── Time stamp counter ──
 *
 * Xbox's QueryPerformanceCounter is a bare `rdtsc`, and its
 * QueryPerformanceFrequency returns the CPU clock as a constant the title
 * compiles in: Half-Life 2's is 0x2BB5C755 (733,333,333 Hz) at 0x0059C6C7.
 * So a frame timer computes seconds as counter / 733333333.
 *
 * Returning the host's own TSC would make that division wrong by the ratio
 * of the two clocks -- a 3.5 GHz host would have the guest believe nearly
 * five seconds had passed for every real one. Scaling the host's
 * performance counter to the console's rate keeps the guest's arithmetic
 * honest.
 *
 * Monotonic and shared by every thread, which is what a TSC is. The first
 * call establishes the origin so the counter starts near zero rather than
 * at whatever the host had been running for. */
#define XBOX_TSC_HZ 733333333ull

uint64_t xbox_ReadTimeStampCounter(void)
{
    static LARGE_INTEGER freq;
    static LARGE_INTEGER origin;
    LARGE_INTEGER now;

    if (xbox_GuestTimeVirtual()) {
        uint64_t ns = vt_ns();
        return (ns / 1000000000ULL) * XBOX_TSC_HZ
             + (ns % 1000000000ULL) * XBOX_TSC_HZ / 1000000000ULL;
    }
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&origin);
        if (freq.QuadPart == 0)
            freq.QuadPart = 1;
    }
    QueryPerformanceCounter(&now);

    {
        uint64_t ticks = (uint64_t)(now.QuadPart - origin.QuadPart);
        /* Split the scaling so a long run cannot overflow: whole seconds
         * first, then the remainder. */
        uint64_t secs = ticks / (uint64_t)freq.QuadPart;
        uint64_t rem  = ticks % (uint64_t)freq.QuadPart;
        return secs * XBOX_TSC_HZ
             + (rem * XBOX_TSC_HZ) / (uint64_t)freq.QuadPart;
    }
}
