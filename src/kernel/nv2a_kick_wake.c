/*
 * nv2a_kick_wake.c - see nv2a_kick_wake.h.
 */
#include "nv2a_kick_wake.h"

#define STAT_ADD(field, n) __atomic_fetch_add(&(field), (n), __ATOMIC_RELAXED)

static void kick_relax(void)
{
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ __volatile__("yield");
#endif
}

int xbox_KickWakePending(const XboxKickWake *k)
{
    uint32_t put = __atomic_load_n(k->put, __ATOMIC_RELAXED);
    if (put != __atomic_load_n(k->done_put, __ATOMIC_RELAXED))
        return 1;
    return (__atomic_load_n(k->kick, __ATOMIC_RELAXED) & k->kick_bit) != 0;
}

/* `armed` while the walker sleeps: ARM_WORK when work ends the sleep, ARM_NUDGE
 * while a flip is held (the flush bit stays set and must not wake it). */
enum { ARM_WORK = 1, ARM_NUDGE = 2 };
#define MS_1 1000000ull

/* Take the armed flag back. 1 when it was still ours (nobody woke us), 0
 * when a poke or nudge took it first, whose wake is then on its way. */
static int disarm(XboxKickWake *k, int was)
{
    return __atomic_compare_exchange_n(k->armed, &was, 0, 0, __ATOMIC_ACQ_REL,
                                       __ATOMIC_ACQUIRE);
}

/* After a sleep that the flag shows someone ended (a poke or a nudge took
 * it): when the sleep itself did not consume their wake (the timeout raced
 * it), the wake is on its way, a few instructions behind the flag. Wait for
 * it rather than leave it latched to end the next sleep; the wait is
 * bounded (the caller's timeout, or 1 ms), so a waker preempted between the
 * flag and the wake costs at most that. */
static void take_wake(XboxKickWake *k, int woke, uint64_t ns)
{
    if (!woke)
        xbox_HostWakerSleepNs(k->waker, ns ? ns : MS_1);
}

int xbox_KickWakeSleep(XboxKickWake *k, uint64_t ns)
{
    int woke;

    STAT_ADD(k->sleeps, 1);
    /* Armed, then the check: a guest that stored its word before seeing
     * `armed` is seen here, one that stores after sees `armed` and pokes. */
    __atomic_store_n(k->armed, ARM_WORK, __ATOMIC_SEQ_CST);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (xbox_KickWakePending(k)) {
        STAT_ADD(k->early, 1);
        if (!disarm(k, ARM_WORK))
            take_wake(k, 0, ns);
        return 1;
    }
    woke = xbox_HostWakerSleepNs(k->waker, ns);
    if (disarm(k, ARM_WORK)) {
        /* Nobody took the flag: a timeout, or a spurious wake-up. */
        STAT_ADD(k->timeouts, 1);
        return 0;
    }
    take_wake(k, woke, ns);
    STAT_ADD(k->poked, 1);
    return 1;
}

int xbox_KickWakeSleepNudge(XboxKickWake *k, uint64_t ns, uint32_t seen)
{
    int woke;

    STAT_ADD(k->held_sleeps, 1);
    __atomic_store_n(k->armed, ARM_NUDGE, __ATOMIC_SEQ_CST);
    /* The timer thread counts and then nudges: a count that moved before
     * the arm was seen nudged nobody, so look again after it. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (k->vblank_count && k->vblank_count() != seen) {
        if (!disarm(k, ARM_NUDGE))
            take_wake(k, 0, ns);
        return 1;
    }
    woke = xbox_HostWakerSleepNs(k->waker, ns);
    if (disarm(k, ARM_NUDGE))
        return 0;
    take_wake(k, woke, ns);
    return 1;
}

void xbox_KickWakePoke(XboxKickWake *k)
{
    if (__atomic_load_n(k->armed, __ATOMIC_RELAXED) != ARM_WORK)
        return;
    /* The guest's store is before this; the walker's arm before its check. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!xbox_KickWakePending(k))
        return;
    if (disarm(k, ARM_WORK)) {
        STAT_ADD(k->pokes, 1);
        xbox_HostWakerWake(k->waker);
    }
}

void xbox_KickWakeNudge(XboxKickWake *k)
{
    int was;

    /* The caller's count is stored before this (the other half of
     * SleepNudge's recheck); once a vblank, so the fence costs nothing. */
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    was = __atomic_load_n(k->armed, __ATOMIC_ACQUIRE);
    if (!was)
        return;
    if (disarm(k, was)) {
        STAT_ADD(k->nudges, 1);
        xbox_HostWakerWake(k->waker);
    }
}

int xbox_KickWakeWatchPut(XboxKickWake *k, uint32_t put_before, uint64_t ns)
{
    uint64_t t0 = xbox_HostNowNs(), now = t0;
    unsigned n = 0;

    for (;;) {
        if (__atomic_load_n(k->put, __ATOMIC_RELAXED) != put_before) {
            k->watch_missed = 0;
            STAT_ADD(k->watch_hits, 1);
            STAT_ADD(k->watch_ns, (long long)(now - t0));
            return 1;
        }
        if (now - t0 >= ns)
            break;
        kick_relax();
        /* The clock every few passes: under Wine QPC is a syscall-free
         * read, but there is no reason to take it per pause. */
        if ((++n & 15) == 0)
            now = xbox_HostNowNs();
    }
    STAT_ADD(k->watch_misses, 1);
    k->watch_missed = 1;
    k->watch_missed_put = put_before;
    k->watch_missed_ns = now;
    return 0;
}

int xbox_KickWakePolling(XboxKickWake *k, uint32_t put)
{
    if (!k->watch_missed)
        return 0;
    if (put != k->watch_missed_put
        || xbox_HostNowNs() - k->watch_missed_ns >= XBOX_KICK_WAKE_CARRY_NS)
        k->watch_missed = 0;
    return k->watch_missed;
}

int xbox_KickWakeMiss(XboxKickWake *k)
{
    STAT_ADD(k->misses, 1);
    if (k->off)
        return 0;
    if (++k->misses_in_row < XBOX_KICK_WAKE_MISS_LIMIT)
        return 0;
    k->off = 1;
    return 1;
}

void xbox_KickWakeHit(XboxKickWake *k)
{
    k->misses_in_row = 0;
}
