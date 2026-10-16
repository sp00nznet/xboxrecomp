/*
 * host_time.h - a monotonic nanosecond clock and a sleep that honours it.
 *
 * The kernel's vblank schedule and its timer thread need sub-millisecond
 * time.
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

#ifdef __cplusplus
}
#endif

#endif /* XBOX_HOST_TIME_H */
