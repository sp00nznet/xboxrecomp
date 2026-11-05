/*
 * The stop probe (game-reset phase 0, task 0.7; RECOMP_DEBUG=reset_probe).
 *
 * A reset stops every guest thread at a point where its host stack can be
 * unwound: a loop back edge, a spin-wait pass, the guest CPU lock, a kernel
 * call. This measures how long that takes, without stopping anything: at
 * each listed flip it raises g_xbox_reset_probe (and the preempt flag, so
 * the back edges call in), notes for each guest thread when it first
 * reaches one of those points, then drops the flag. A thread already
 * blocked in a kernel wait or queued on the CPU lock counts as stopped at
 * 0. One [RESET-PROBE] line per thread and a summary per sample.
 *
 * Off (no key), every hook is one load of a static and a branch.
 */
#ifndef KERNEL_RESET_PROBE_H
#define KERNEL_RESET_PROBE_H

#include <stdint.h>

/* Where a thread reached a stop point, or how it was already stopped. */
enum {
    XBOX_RESET_STOP_BACK_EDGE = 1,  /* xbox_GuestCpuPreempt */
    XBOX_RESET_STOP_SPIN,           /* xbox_SpinWait */
    XBOX_RESET_STOP_KICK,           /* xbox_SpinWaitKick */
    XBOX_RESET_STOP_ACQUIRE,        /* asks for the guest CPU lock */
    XBOX_RESET_STOP_BRIDGE,         /* enters a kernel call */
    XBOX_RESET_STOP_BLOCKED,        /* blocks inside a short kernel call */
    XBOX_RESET_STOP_WAITING,        /* at the flip: in a blocking kernel call */
    XBOX_RESET_STOP_QUEUED,         /* at the flip: queued on the CPU lock */
    XBOX_RESET_STOP_KINDS
};

/* What a guest thread is doing, as far as the probe tracks it. */
enum {
    XBOX_RESET_RUN = 0,             /* guest code (or a host call outside a bridge) */
    XBOX_RESET_IN_BRIDGE,           /* inside a kernel call */
    XBOX_RESET_IN_BLOCK,            /* blocked inside a short kernel call */
    XBOX_RESET_IN_QUEUE             /* waiting for the guest CPU lock */
};

/* Set only while a sample is open. */
extern volatile int32_t g_xbox_reset_probe;
/* 1 once a guest thread has joined with the key set: the state hooks run. */
extern int g_xbox_reset_probe_track;

/* 1 when the key is set: the state hooks then track. Read once. */
int  xbox_ResetProbeTracking(void);
/* Read the key again, as after recomp_env_set (the ctest); threads that
 * joined before keep what they had. */
void xbox_ResetProbeReloadConfig(void);

/* A guest thread starts or ends (xbox_GuestCpuJoin / Part). */
void xbox_ResetProbeJoin(void);
void xbox_ResetProbePart(void);

/* This thread's state; where is the ordinal for IN_BRIDGE. */
void xbox_ResetProbeStateSlow(int state, uint32_t where);
static inline void xbox_ResetProbeState(int state, uint32_t where)
{
    if (g_xbox_reset_probe_track)
        xbox_ResetProbeStateSlow(state, where);
}

/* A stop point: the first one per thread per sample is the one noted. */
void xbox_ResetProbeHitSlow(int kind, uint32_t where);
static inline void xbox_ResetProbeHit(int kind, uint32_t where)
{
    if (g_xbox_reset_probe)
        xbox_ResetProbeHitSlow(kind, where);
}

/* The walker's flip (every backend); opens a sample at a listed flip.
 * Never blocks: the sample runs on the probe's own host thread. */
void xbox_ResetProbeFlip(uint32_t flip);

/* For the ctest: open a sample now, wait for its summary, and read it.
 * 0 when no sample ran. */
typedef struct {
    uint32_t flip;
    int      threads;               /* guest threads alive at the flip */
    int      stopped;               /* those that reached a stop point */
    uint64_t max_ns;                /* the slowest of them */
    int      late_100ms;            /* stopped, but past 100 ms */
} XboxResetProbeSample;
int  xbox_ResetProbeSampleNow(uint32_t flip, XboxResetProbeSample *out);

/* The parsed key: 1 when flip is listed (for the ctest of the parser). */
int  xbox_ResetProbeListed(const char *spec, uint32_t flip);

#endif
