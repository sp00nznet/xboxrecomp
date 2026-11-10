/*
 * The [FATAL] line (recomp_fatal.c): the exact text, a short buffer cut
 * cleanly, the exception and signal names, and (POSIX) that each death it
 * takes writes the line once and still ends the child by the same signal
 * (a real trap instruction among them), while a handler installed before it
 * keeps its signal.
 */
#include "recomp_fatal.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/wait.h>
#include <unistd.h>
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static void test_format(void)
{
    char line[256], tiny[12];
    size_t n;

    n = recomp_fatal_format(line, sizeof line, "SIGILL", "illegal instruction",
                            (const void *)(uintptr_t)0x1A0489u);
    printf("  %s", line);
    check(!strcmp(line, "[FATAL] SIGILL (illegal instruction) at 0x1A0489;"
                        " the process ends\n"),
          "format: the exact line");
    check(n == strlen(line), "format: returns the length");
    recomp_fatal_format(line, sizeof line, "SIGABRT", NULL, NULL);
    check(!strcmp(line, "[FATAL] SIGABRT; the process ends\n"),
          "format: no detail, no pc");
    n = recomp_fatal_format(tiny, sizeof tiny, "SIGABRT", "abort()", NULL);
    check(n == sizeof tiny - 1 && tiny[sizeof tiny - 1] == 0
              && !strncmp(tiny, "[FATAL] SIG", sizeof tiny - 1),
          "format: a short buffer is cut and terminated");
    check(recomp_fatal_format(tiny, 0, "x", NULL, NULL) == 0,
          "format: a zero-size buffer writes nothing");
}

static void test_names(void)
{
    check(!strcmp(recomp_fatal_exception_name(0xC00000FDu), "stack overflow"),
          "names: 0xC00000FD is a stack overflow");
    check(!strcmp(recomp_fatal_exception_name(0xC0000094u),
                  "integer divide by zero"),
          "names: 0xC0000094 is an integer divide by zero");
    check(recomp_fatal_exception_name(0x12345678u) == NULL,
          "names: an unknown code has no name");
    check(!strcmp(recomp_fatal_signal_name(SIGABRT), "SIGABRT"),
          "names: SIGABRT");
    check(recomp_fatal_signal_name(SIGINT) == NULL,
          "names: SIGINT is not taken");
}

#ifndef _WIN32

enum { DIE_ABORT, DIE_ILL, DIE_FPE, DIE_TRAP, KEEP_OWN };

/* __builtin_trap is ud2 on x86 (SIGILL) and brk on arm64 (SIGTRAP). */
#if defined(__aarch64__) || defined(__arm64__)
#define TRAP_SIG SIGTRAP
#else
#define TRAP_SIG SIGILL
#endif

static volatile sig_atomic_t s_own_ran;

static void own_handler(int sig)
{
    (void)sig;
    s_own_ran = 1;
}

/* Runs `how` in a child whose stderr is a pipe; returns the child's wait
 * status and what it wrote. */
static int run_child(int how, char *out, size_t cap)
{
    int fds[2], status = 0;
    size_t got = 0;
    pid_t pid;

    if (pipe(fds) != 0)
        return -1;
    pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDERR_FILENO);
        if (how == KEEP_OWN)
            signal(SIGFPE, own_handler);
        recomp_fatal_install();
        recomp_fatal_install();   /* idempotent: still one report */
        switch (how) {
        case DIE_ABORT: abort();
        case DIE_ILL:   raise(SIGILL); break;
        case DIE_FPE:   raise(SIGFPE); break;
        case DIE_TRAP:  __builtin_trap();
        case KEEP_OWN:
            raise(SIGFPE);
            _exit(s_own_ran ? 42 : 1);
        }
        _exit(0);   /* not reached: each death above ends the child */
    }
    close(fds[1]);
    for (;;) {
        ssize_t r = read(fds[0], out + got, cap - 1 - got);
        if (r <= 0)
            break;
        got += (size_t)r;
        if (got >= cap - 1)
            break;
    }
    out[got] = 0;
    close(fds[0]);
    waitpid(pid, &status, 0);
    return status;
}

static int count(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = hay; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

static void test_deaths(void)
{
    char out[1024];
    int st;

    st = run_child(DIE_ABORT, out, sizeof out);
    printf("  abort: %s", out);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT,
          "abort: the child still ends by SIGABRT");
    check(count(out, "[FATAL] SIGABRT (abort()); the process ends\n") == 1,
          "abort: one [FATAL] line");

    st = run_child(DIE_ILL, out, sizeof out);
    printf("  ill: %s", out);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGILL,
          "SIGILL: the child still ends by SIGILL");
    /* Not "sent by kill or raise" on every host: Darwin hands a raised
     * SIGILL si_code 2, the code of a real one. */
    check(count(out, "[FATAL] SIGILL (") == 1, "SIGILL: one [FATAL] line");

    st = run_child(DIE_FPE, out, sizeof out);
    printf("  fpe: %s", out);
    check(WIFSIGNALED(st) && WTERMSIG(st) == SIGFPE,
          "SIGFPE: the child still ends by SIGFPE");
    check(count(out, "[FATAL] SIGFPE") == 1, "SIGFPE: one [FATAL] line");

    st = run_child(DIE_TRAP, out, sizeof out);
    printf("  trap: %s", out);
    check(WIFSIGNALED(st) && WTERMSIG(st) == TRAP_SIG,
          "trap: __builtin_trap still ends the child by its signal");
    check(count(out, "[FATAL] ") == 1
              && strstr(out, recomp_fatal_signal_name(TRAP_SIG)) != NULL,
          "trap: one [FATAL] line naming the signal");

    st = run_child(KEEP_OWN, out, sizeof out);
    check(WIFEXITED(st) && WEXITSTATUS(st) == 42,
          "own handler: a handler set first keeps its signal");
    check(strstr(out, "[FATAL]") == NULL, "own handler: no [FATAL] line");
}

#endif

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("recomp_fatal:\n");
    test_format();
    test_names();
#ifndef _WIN32
    test_deaths();
#endif
    printf("%s (%d failed)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
