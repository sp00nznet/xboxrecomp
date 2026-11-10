/*
 * recomp_fatal.h - one [FATAL] line in the game log for every death the
 * games' crash handlers do not report.
 *
 * Each game's main.c reports an access violation (Windows VEH) or a SIGSEGV
 * or SIGBUS (POSIX) with the guest registers. Everything else ended the
 * process with nothing in the log: abort(), an illegal instruction, an
 * integer divide by zero, a stack overflow, a breakpoint. On the Deck that
 * reads exactly like a hang, because the log simply stops. This
 * names what ended the run, on stderr, which the games point at the log
 * (RECOMP_STDIO_LOG) unbuffered, so the line is on disk before the process
 * goes.
 *
 * Not covered: on Windows (and under Wine) __fastfail, which the CRT uses
 * for a failed stack cookie or an invalid parameter, and heap corruption
 * (0xC0000374) end the process without calling any exception filter, so they
 * still leave nothing. A Windows access violation gets two lines: the game's
 * [CRASH] report, then [FATAL] once nothing handled it.
 *
 * It only reports: each death still ends the process as it did before
 * (Windows: the previous top-level filter is called first and its verdict
 * kept; POSIX: the signal is re-raised with its default action). On POSIX it
 * takes a signal only while its disposition is the default, so a handler a
 * game or SDL installed keeps it.
 */
#ifndef RECOMP_FATAL_H
#define RECOMP_FATAL_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Installs the reporters. Idempotent; recomp_env_init calls it, which every
 * game runs first in main, so the reporters are in place before any thread
 * and every game has them with no code of its own. */
void recomp_fatal_install(void);

/* The name of a Windows exception code ("stack overflow"), or NULL for one
 * this does not know. Plain data, so it is testable on every host. */
const char *recomp_fatal_exception_name(uint32_t code);

/* The name of a POSIX signal the reporter takes ("SIGABRT"), or NULL. */
const char *recomp_fatal_signal_name(int sig);

/* The report line, without allocating or taking a lock (it runs inside a
 * signal handler): "[FATAL] <what> (<detail>) at 0x<pc>; the process ends\n".
 * detail and pc may be NULL. Returns the length written, the line cut to fit
 * n - 1 bytes; buf is always terminated when n > 0. */
size_t recomp_fatal_format(char *buf, size_t n, const char *what,
                           const char *detail, const void *pc);

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_FATAL_H */
