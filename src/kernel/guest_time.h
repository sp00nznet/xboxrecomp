/*
 * guest_time.h - every clock the guest can read, from one place.
 *
 * A title reads time through several doors: the kernel's
 * KeQueryPerformanceCounter/Frequency, KeQuerySystemTime and
 * KeQueryInterruptTime calls, the KeTickCount, KeInterruptTime and
 * KeSystemTime data exports, and a bare rdtsc that the recompiler turns
 * into xbox_ReadTimeStampCounter. Timed waits (a wait's timeout, a delay, a
 * kernel timer) convert their due times on the same clocks. Each of these
 * used to read the host on its own; this file is now the only place that
 * does, so one switch can replace all of them.
 *
 * Two modes:
 *
 *   real     (the default) each read is the host's, exactly as it was
 *            before this file existed: the performance counter at the
 *            host's frequency, the wall clock anchored once, GetTickCount.
 *   virtual  (RECOMP_DEBUG=deterministic) the guest's time is a count of
 *            emulated vblanks. It stands still between two vblanks and
 *            moves by exactly one period (10^9 / 60 ns) at each, whatever
 *            the host's speed, so a title that reads the clock twice in
 *            one frame sees the same delta on every run and every host.
 *            It steps per vblank and never per game flip, so a title that
 *            runs at 30 fps, 60 fps or a varying rate needs no setting.
 *
 * Virtual mode is a debugging aid: a title that busy-waits on the clock
 * without a kernel call holds its thread until the next vblank. The
 * vblanks themselves still come from the host's 60 Hz schedule, which is
 * why virtual time alone does not make a run repeatable; it removes the
 * clock from the list of things that differ between two runs.
 */
#ifndef XBOX_GUEST_TIME_H
#define XBOX_GUEST_TIME_H

#include <stdint.h>

/* Lets a game's own code (a hand-written pacer, say) test at build time
 * that this toolkit has the virtual clock, and stay buildable on one that
 * does not. */
#define XBOX_GUEST_TIME_VIRTUAL 1

#ifdef __cplusplus
extern "C" {
#endif

/* KeQueryPerformanceCounter / KeQueryPerformanceFrequency. */
int64_t  xbox_GuestPerfCounter(void);
int64_t  xbox_GuestPerfFrequency(void);

/* KeQuerySystemTime: 100 ns since 1601, at the performance counter's
 * resolution (anchored once to the wall clock). */
uint64_t xbox_GuestSystemTime(void);

/* The host wall clock at its own resolution, 100 ns since 1601: the
 * KeSystemTime data export, NtSetSystemTime's previous time, and the
 * conversion of an absolute due time (a wait's timeout, a delay, a timer).
 * In virtual mode the same value as xbox_GuestSystemTime. */
uint64_t xbox_GuestSystemTimeCoarse(void);

/* KeQueryInterruptTime and the KeInterruptTime export: 100 ns since boot. */
uint64_t xbox_GuestInterruptTime(void);

/* KeTickCount: milliseconds since boot, 32 bits. */
uint32_t xbox_GuestTickCount(void);

/* The rdtsc helper the generated code calls (declared again in the
 * generated recomp_types.h): ticks at the console's 733 MHz. */
uint64_t xbox_ReadTimeStampCounter(void);

/* ── Virtual time ──
 *
 * 1 when RECOMP_DEBUG=deterministic asked for it; read once, never changes
 * during a run. */
int      xbox_GuestTimeVirtual(void);

/* One emulated vblank: the virtual clock moves by one period. Called by the
 * timer thread's vblank schedule (also when vblanks are not raised, so the
 * clock still runs); a no-op in real mode. */
void     xbox_GuestTimeVblank(void);

/* Who steps the clock: the kernel's timer thread, registered by bridge
 * init. In virtual mode it is started here at once, so the clock runs from
 * boot, before and without any kernel timer. In real mode nothing happens:
 * the first KeSetTimer starts that thread, as before. */
void     xbox_GuestTimeSetStepper(void (*start)(void));

/* How many vblanks the virtual clock has counted (0 in real mode). */
uint64_t xbox_GuestTimeVblanks(void);
/* This thread's reads of the virtual clock in the current vblank. */
uint64_t xbox_GuestTimeFrozenReads(void);

/* Guest milliseconds since boot, 64 bits: the clock the kernel timers keep
 * their due times on in virtual mode. */
uint64_t xbox_GuestTimeMs(void);

/* An NT timeout (negative: relative 100 ns; positive: absolute system time)
 * as a due time on the interrupt-time clock (100 ns since boot), for a wait
 * that runs on virtual time: it ends once xbox_GuestTimeReached says so. */
uint64_t xbox_GuestTimeDue(int64_t nt_timeout);
int      xbox_GuestTimeReached(uint64_t due);

/* Test hooks: force the mode (1 virtual, 0 real, -1 back to the env) and
 * reset the virtual count to 0. Not for the runtime. */
void     xbox_GuestTimeTestMode(int mode);

#ifdef __cplusplus
}
#endif

#endif /* XBOX_GUEST_TIME_H */
