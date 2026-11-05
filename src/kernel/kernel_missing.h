/*
 * kernel_missing.h - the missing-game-file report
 *
 * A title that opens a file the dump lacks does not crash: it goes silent,
 * skips a movie, or draws its own "disc is dirty or damaged" screen. None of
 * those says which file. This report does, once per file, and sums up at
 * exit. It reads only what the failed open already had and never changes
 * what the guest sees.
 *
 * RECOMP_TRACE=missing   unset/1: high-confidence misses and a summary
 *                        all:     also low-confidence misses, other trees,
 *                                 attribute probes; every FAILED line prints
 *                        0:       off; nothing recorded, every FAILED line
 * RECOMP_DEBUG=missing_list=<path>: the unique misses, written at summary
 */
#ifndef KERNEL_MISSING_H
#define KERNEL_MISSING_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Record a failed open (is_probe 0) or a failed NtQueryFullAttributesFile
 * (is_probe 1, with disposition FILE_OPEN and options 0). Call it on the
 * thread that made the failed call, right after it: the host path comes
 * from that thread's xbox_LastHostPath(). Prints this path's line the first
 * time it is seen. */
void xbox_missing_note(const char *guest_path, uint32_t status,
                       uint32_t access, uint32_t disposition,
                       uint32_t options, int is_probe);

/* 1 when guest_path already has a "[FILE] missing" line and the per-attempt
 * FAILED line for this attempt should not print; counts the attempt as a
 * repeat. Always 0 under missing=all and missing=0. */
int xbox_missing_is_reported(const char *guest_path);

/* 1 under RECOMP_TRACE=missing=all: the bridge then also logs the opens
 * it is quiet about by default. */
int xbox_missing_verbose(void);

/* The summary line, at most once per run whoever calls first, and the
 * missing_list file. why ("exit", "firmware", "window", "bugcheck")
 * ends the line under missing=all, so a verbose log says which exit printed it. */
void xbox_missing_summary(const char *why);

/* Lock-free reads for the watchdog and the crash handler. Any pointer may
 * be NULL. */
void xbox_missing_counts(uint32_t *high, uint32_t *low, uint32_t *attempts);

/* Tests only: forget every path and count, re-read the keys. */
void xbox_missing_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* KERNEL_MISSING_H */
