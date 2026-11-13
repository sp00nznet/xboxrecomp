/*
 * recomp_stdio.h - how stdout and stderr are buffered, and the flush once a
 * flip that goes with it.
 *
 * The games used to set both streams to _IONBF so every line reached the log
 * at once. Under Wine that is costly: the UCRT writes an unbuffered stream
 * one format piece at a time (about 30 NtWriteFile a log line), and on an
 * append-mode file (RECOMP_STDIO_LOG) each write also takes a position query
 * and a seek. Every one is a wineserver round trip. The four lines a flip of
 * RECOMP_FLIP_LOG cost the walker 1.5-1.85 ms a flip on Bazzite.
 *
 * So on Windows, when a stream is not a console, it is fully buffered and
 * written once a flip (recomp_stdio_flush, from the walker's flip), once a
 * second by a backstop thread when no flip comes (the boot, a hang), and
 * before every exit and crash report the runtime writes. A console stays
 * unbuffered, as before.
 *
 * POSIX keeps _IONBF: the games' SIGSEGV report and the [FATAL] line write
 * fd 2 with write(2) from a signal handler, which cannot flush stdio safely,
 * so a buffer there would put the last lines after the report or lose them;
 * and a native write(2) costs about a microsecond.
 */
#ifndef RECOMP_STDIO_H
#define RECOMP_STDIO_H

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sets the buffering of stdout and stderr: call it where the games set them
 * to _IONBF, after any RECOMP_STDIO_LOG redirect and before anything else is
 * printed. Returns 1 when a stream was buffered. Once per process. */
int recomp_stdio_buffer(void);

/* Writes out what the buffered streams hold. A no-op (no lock taken) while
 * neither is buffered. The walker calls it once a flip; any thread. */
void recomp_stdio_flush(void);

/* Has the backstop thread flush the buffered streams, and waits up to
 * timeout_ms for it. For a thread that cannot flush itself (a stack
 * overflow) or must not wait on the streams' locks unbounded (the watchdog,
 * whose wedged thread may hold one). Returns 1 when the flush finished or
 * nothing was buffered, 0 on timeout. */
int recomp_stdio_flush_from_backstop(unsigned timeout_ms);

/* 1 on the thread that is inside the flush right now: a death there must
 * not flush again, the stream is mid-write. */
int recomp_stdio_flushing_here(void);

/* Whether a stream on this host gets a buffer: only on Windows, and only
 * when it is not a console. Split out so the policy is testable on every
 * host. */
int recomp_stdio_policy(int windows, int tty);

/* Gives f a full buffer of its own (buffer != 0) or none, and marks it for
 * recomp_stdio_flush. recomp_stdio_buffer's step for one stream; the tests
 * use it on a temporary file. */
void recomp_stdio_apply(FILE *f, int buffer);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_STDIO_H */
