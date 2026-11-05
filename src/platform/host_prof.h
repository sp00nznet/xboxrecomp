/*
 * host_prof.h - a sampling profiler inside the process (RECOMP_TRACE=prof).
 *
 * A benchmark wants the same hot-path report on every host it runs on: the
 * Mac (Metal), Proton (D3D11 under Wine) and native Windows (D3D11). The
 * hosts' own profilers differ (sample/xctrace, perf, ETW) and native Windows
 * has none that a script can read back without extra installs, so the
 * runtime samples itself. A thread wakes HZ times a second, stops each
 * thread of the process that ran since its last look, reads its program
 * counter and lets it go. Samples are counted per (thread, pc) and written
 * out at exit as module offsets; the bench symbolises them offline with the
 * exe's symbol table (llvm-nm), so the recompiled guest functions, the
 * backends and the drivers come out by name or module.
 *
 * Windows (native and Wine): SuspendThread / GetThreadContext, a thread
 * counted when its cycle time moved. macOS: thread_suspend /
 * thread_get_state, a thread counted when the kernel reports it running.
 * Other hosts: one line saying it is not supported, nothing sampled.
 * Printing only: nothing the game does changes, but the stops cost a little
 * time, so the bench measures its overhead before trusting a run with it.
 */
#ifndef HOST_PROF_H
#define HOST_PROF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Starts the sampler thread: hz samples a second (clamped to 10..2000),
 * after delay_ms. out is the report's path (copied); NULL writes
 * "recomp-prof.txt" in the working directory. Registers the exit hook and
 * an atexit that write the report. Returns 0 when the host has no sampler
 * or it was started already. Any thread. */
int host_prof_start(unsigned hz, unsigned delay_ms, const char *out);

/* The RECOMP_TRACE keys: prof=HZ (1 or empty: 250), prof_delay=seconds,
 * prof_out=path. Does nothing when prof is unset or 0. */
void host_prof_start_from_env(void);

/* Writes the report now (stops sampling first). Safe to call twice: the
 * second call does nothing. */
void host_prof_dump(void);

/* For the unit test: samples taken so far, and the pc sampled most often
 * on the thread whose OS name is name (0 when none). */
uint64_t host_prof_samples(void);
uintptr_t host_prof_top_pc(const char *name);

#ifdef __cplusplus
}
#endif

#endif /* HOST_PROF_H */
