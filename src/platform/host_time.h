/*
 * host_time.h - a monotonic nanosecond clock and a sleep that honours it.
 *
 * The kernel's vblank schedule, the frame-pacing trace and a title's own
 * pacers (a BlockUntilVerticalBlank loop) all need sub-millisecond time.
 * Sleep() is whole milliseconds and, on native Windows without
 * timeBeginPeriod, rounds to the 15.6 ms system tick; a schedule run on it
 * fires vblanks milliseconds off their edges.
 *
 * xbox_HostNowNs reads the same performance counter on Windows that the
 * kernel's millisecond clock reads, so ms and ns deadlines compare on one
 * clock. xbox_HostSleepNs may return early or late (signals, Wine's timer
 * server): callers re-read the clock against their absolute deadline rather
 * than trusting it.
 */
#ifndef XBOX_HOST_TIME_H
#define XBOX_HOST_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Nanoseconds on a monotonic clock with an arbitrary epoch. */
uint64_t xbox_HostNowNs(void);

/* Sleep about `ns` nanoseconds; 0 returns at once. */
void xbox_HostSleepNs(uint64_t ns);

/* For a thread that keeps a clock with xbox_HostSleepNs (the kernel's timer
 * thread): ask the host to wake it on time. macOS wakes an ordinary thread's
 * nanosleep 3.7 ms late on average (8 ms at worst) even on an idle machine,
 * at any QoS class, which put the 600-vblank windows at 13.4-20.0 ms; a
 * time-constraint (real-time) thread wakes within ~20 us. Nothing elsewhere.
 * Call on the thread itself. */
void xbox_HostTimerThreadInit(void);

/* The timer thread's sleep: the same deadline as xbox_HostSleepNs, but
 * xbox_HostTimerWake from any thread ends it early. A wake that lands
 * between two sleeps is kept for the next one, so a DPC queued then is not
 * waited out. One sleeper (the timer thread); any number of wakers. The
 * caller re-reads its clock afterwards, as with xbox_HostSleepNs. */
void xbox_HostTimerSleepNs(uint64_t ns);
void xbox_HostTimerWake(void);

/* A wakeable sleep for one sleeper and any number of wakers, with a
 * nanosecond timeout: the NV2A walker waits on one for the guest's KickOff
 * (nv2a_kick_wake.h) instead of polling every 50 us. A wake that lands
 * while nobody sleeps is kept for the next sleep, so a waker that raced the
 * sleeper's last check is not lost; a sleep may also end early for no
 * reason, so the caller re-checks what it waits for. Wine: an SRW lock and
 * a condition variable (in-process futexes), whose timeout is whole
 * milliseconds, rounded up. Native Windows: an event and the
 * high-resolution waitable timer, at most one clock interval (0.5-1 ms)
 * late; NULL where that timer is missing. POSIX: a mutex and a condition
 * variable on the monotonic clock. Never freed. */
typedef struct XboxHostWaker XboxHostWaker;
XboxHostWaker *xbox_HostWakerCreate(void);
/* One form by name, whatever the host: tests/kick_wake runs the native
 * Windows form under Wine, where xbox_HostWakerCreate picks the condition
 * variable. NULL where the form does not exist (the event form on POSIX, or
 * without the high-resolution timer). */
typedef enum {
    XBOX_WAKER_AUTO,            /* xbox_HostWakerCreate's choice */
    XBOX_WAKER_CONDVAR,         /* a lock and a condition variable */
    XBOX_WAKER_EVENT            /* Windows: an auto-reset event and the
                                 * high-resolution waitable timer */
} XboxHostWakerForm;
XboxHostWaker *xbox_HostWakerCreateForm(XboxHostWakerForm form);
/* 1 when a wake ended it (or one was kept), 0 on the timeout. ns 0 only
 * consumes a kept wake. */
int  xbox_HostWakerSleepNs(XboxHostWaker *w, uint64_t ns);
void xbox_HostWakerWake(XboxHostWaker *w);

/* The Windows build under Wine (ntdll exports wine_get_version); 0 on a
 * native Windows host and on POSIX. Looked up once. */
int xbox_HostUnderWine(void);

typedef enum {
    XBOX_NAP_AUTO,      /* the rule at xbox_HostNapNs, resolved once */
    XBOX_NAP_TIMER,     /* the thread's high-resolution waitable timer
                         * (POSIX: nanosleep) */
    XBOX_NAP_DELAY,     /* NtDelayExecution (POSIX: nanosleep) */
    XBOX_NAP_SPIN,      /* busy-wait on the clock with a pause instruction */
    XBOX_NAP_HYBRID     /* SPIN for a hot nap, TIMER for a cold one */
} XboxNapMode;

/* Set once before the napping threads start (an A/B switch). */
void xbox_HostNapModeSet(XboxNapMode mode);
/* The mode in effect, never AUTO. TIMER becomes DELAY for the process when a
 * thread cannot create the high-resolution timer (Windows before 1803). */
XboxNapMode xbox_HostNapMode(void);
/* 0 once a thread could not create the high-resolution timer: TIMER naps,
 * and HYBRID's cold ones, are NtDelayExecution from then on. 1 elsewhere. */
int xbox_HostNapHasTimer(void);

/* A poll loop's short sleep (the NV2A ack loop's ~50 us between polls).
 * `hot` says the caller expects what it polls for within a couple of
 * milliseconds (it saw activity just now): under HYBRID a hot nap spins and
 * a cold one takes the timer; the other modes ignore the flag.
 *
 * The hosts differ. Every timed wait on native Windows expires on the system
 * clock interrupt: NtDelayExecution and Sleep round up to the clock
 * interval, 1 ms under timeBeginPeriod(1), and even the high-resolution
 * waitable timer, which is exempt from coalescing, can be one interval late,
 * and the finest interval is 0.5 ms (a 50 us request measured p50 511 us,
 * NtDelayExecution 1005 us, a spin 50 us). So native Windows spins hot and
 * takes the timer cold. Wine's non-alertable NtDelayExecution is a select
 * with a timeval inside ntdll and is honoured (~110 us), while its waitable
 * timer is a wineserver object, two server round trips a nap: Wine keeps
 * NtDelayExecution. POSIX nanosleep is honoured (~80 us). AUTO is HYBRID on
 * native Windows and DELAY under Wine and on POSIX.
 *
 * 0 returns at once. The caller re-reads its clock, as with
 * xbox_HostSleepNs. */
void xbox_HostNapNs(uint64_t ns, int hot);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_HOST_TIME_H */
