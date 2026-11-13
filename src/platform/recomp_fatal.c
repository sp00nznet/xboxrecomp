/*
 * recomp_fatal.c - the [FATAL] line for deaths the crash handlers miss
 * (recomp_fatal.h).
 */
#include "recomp_fatal.h"
#include "recomp_stdio.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#if defined(__APPLE__)
#include <sys/ucontext.h>
#endif
#endif

/* ---- the line ------------------------------------------------------------ */

/* Appends s at *at, never past end - 1. No snprintf: the POSIX reporter runs
 * inside a signal handler, and the Windows one may run on a thread whose
 * stack just overflowed. */
static void put(char **at, char *end, const char *s)
{
    while (*s && *at < end - 1)
        *(*at)++ = *s++;
}

static void put_hex(char **at, char *end, uint64_t v, int min_digits)
{
    char tmp[17];
    int n = 0;

    do {
        tmp[n++] = "0123456789ABCDEF"[v & 15];
        v >>= 4;
    } while (v && n < 16);
    while (n < min_digits && n < 16)
        tmp[n++] = '0';
    put(at, end, "0x");
    while (n > 0 && *at < end - 1)
        *(*at)++ = tmp[--n];
}

size_t recomp_fatal_format(char *buf, size_t n, const char *what,
                           const char *detail, const void *pc)
{
    char *at = buf, *end = buf + n;

    if (n == 0)
        return 0;
    put(&at, end, "[FATAL] ");
    put(&at, end, what ? what : "unknown");
    if (detail && *detail) {
        put(&at, end, " (");
        put(&at, end, detail);
        put(&at, end, ")");
    }
    if (pc) {
        put(&at, end, " at ");
        put_hex(&at, end, (uint64_t)(uintptr_t)pc, 1);
    }
    put(&at, end, "; the process ends\n");
    *at = 0;
    return (size_t)(at - buf);
}

const char *recomp_fatal_exception_name(uint32_t code)
{
    /* The codes by value, so the table builds without <windows.h>. */
    switch (code) {
    case 0xC0000005u: return "access violation";
    case 0xC0000006u: return "in-page error";
    case 0xC000001Du: return "illegal instruction";
    case 0xC0000025u: return "noncontinuable exception";
    case 0xC000008Cu: return "array bounds exceeded";
    case 0xC000008Du: return "float denormal operand";
    case 0xC000008Eu: return "float divide by zero";
    case 0xC000008Fu: return "float inexact result";
    case 0xC0000090u: return "float invalid operation";
    case 0xC0000091u: return "float overflow";
    case 0xC0000092u: return "float stack check";
    case 0xC0000093u: return "float underflow";
    case 0xC0000094u: return "integer divide by zero";
    case 0xC0000095u: return "integer overflow";
    case 0xC0000096u: return "privileged instruction";
    case 0xC00000FDu: return "stack overflow";
    case 0xC0000374u: return "heap corruption";
    case 0xC0000409u: return "stack buffer overrun or fail fast";
    case 0x80000003u: return "breakpoint";
    case 0x80000004u: return "single step";
    case 0x80000002u: return "datatype misalignment";
    default:          return NULL;
    }
}

const char *recomp_fatal_signal_name(int sig)
{
    switch (sig) {
    case SIGABRT: return "SIGABRT";
    case SIGILL:  return "SIGILL";
    case SIGFPE:  return "SIGFPE";
#ifdef SIGTRAP
    case SIGTRAP: return "SIGTRAP";
#endif
    default:      return NULL;
    }
}

#define ON_DEATH 4
static recomp_fatal_fn s_on_death[ON_DEATH];
static volatile int s_died;

int recomp_fatal_on_death(recomp_fatal_fn fn)
{
    for (int i = 0; i < ON_DEATH; i++) {
        recomp_fatal_fn cur = NULL;
        if (__atomic_compare_exchange_n(&s_on_death[i], &cur, fn, 0,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)
            || cur == fn)
            return 1;
    }
    return 0;
}

/* Once per process: a second death while these run goes straight on. */
void recomp_fatal_run_on_death(void)
{
    if (__atomic_exchange_n(&s_died, 1, __ATOMIC_SEQ_CST))
        return;
    for (int i = 0; i < ON_DEATH; i++) {
        recomp_fatal_fn fn = __atomic_load_n(&s_on_death[i], __ATOMIC_ACQUIRE);
        if (fn != NULL)
            fn();
    }
}

/* One report per process: a second death while the first is written (or a
 * second thread dying) goes straight to the default end. */
static volatile int s_reported;
static volatile int s_installed;

#if defined(_WIN32)

/* ---- Windows ------------------------------------------------------------- */

static void win_write(const char *line)
{
    /* WriteFile, not the CRT: after a stack overflow the faulting thread has
     * a few KB left, and a UCRT fputs (its lock, its buffers) can fault again
     * and lose the line. stdout and stderr are reopened in append mode on the
     * one log file (RECOMP_STDIO_LOG); the all-ones offset makes the kernel
     * write at the end of the file, as the CRT's append would, so the line
     * never lands over another. The CRT's text mode would add the \r. */
    char buf[260];
    size_t n = strlen(line);
    HANDLE h = INVALID_HANDLE_VALUE;
    OVERLAPPED ov, *pov = NULL;
    DWORD done;
    int fd = _fileno(stderr);

    if (n > sizeof buf - 2)
        n = sizeof buf - 2;
    memcpy(buf, line, n);
    if (n > 0 && buf[n - 1] == '\n') {
        buf[n - 1] = '\r';
        buf[n++] = '\n';
    }
    if (fd >= 0)
        h = (HANDLE)_get_osfhandle(fd);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        h = GetStdHandle(STD_ERROR_HANDLE);
    if (h == INVALID_HANDLE_VALUE || h == NULL)
        return;
    if (GetFileType(h) == FILE_TYPE_DISK) {
        memset(&ov, 0, sizeof ov);
        ov.Offset = ov.OffsetHigh = 0xFFFFFFFFu;
        pov = &ov;
    }
    WriteFile(h, buf, (DWORD)n, &done, pov);
}

static LPTOP_LEVEL_EXCEPTION_FILTER s_prev_filter;

/* The last stop for an exception nothing handled, on any thread. The
 * previous filter goes first and its verdict stands: a code it resumes is no
 * death. With llvm-mingw (20260922) that is the C++ runtime's
 * cpp_unhandled_exception_filter, which acts only on a C++ exception and
 * passes the rest on.
 *
 * An access violation reaches here after the game's VEH has written its
 * [CRASH] report, so the log then has both lines: [CRASH] with the guest
 * registers, then this [FATAL]. __fastfail (int 0x29, the CRT's
 * stack-cookie and invalid-parameter ends) and heap corruption (0xC0000374)
 * end the process in the kernel without calling any filter, on Windows and
 * under Wine alike, so those still die silent. */
static LONG WINAPI fatal_filter(PEXCEPTION_POINTERS ep)
{
    LONG verdict = EXCEPTION_CONTINUE_SEARCH;
    char what[64], line[256];
    char *at = what, *end = what + sizeof what;
    uint32_t code = ep && ep->ExceptionRecord
                        ? (uint32_t)ep->ExceptionRecord->ExceptionCode : 0;

    if (s_prev_filter)
        verdict = s_prev_filter(ep);
    if (verdict == EXCEPTION_CONTINUE_EXECUTION)
        return verdict;
    if (InterlockedExchange((volatile LONG *)&s_reported, 1))
        return verdict;
    put(&at, end, "unhandled exception ");
    put_hex(&at, end, code, 8);
    put(&at, end, " on thread ");
    {
        /* Decimal, as the [CS] lines name threads. */
        char num[12];
        int k = 0;
        DWORD tid = GetCurrentThreadId();
        do { num[k++] = (char)('0' + tid % 10); tid /= 10; } while (tid && k < 11);
        while (k > 0 && at < end - 1)
            *at++ = num[--k];
    }
    *at = 0;
    /* Where the streams are buffered (recomp_stdio.h), the lines still held
     * go out first, so the log keeps its order and the last lines before the
     * death. After a stack overflow the few KB of stack left would not carry
     * the CRT's write, so the backstop thread writes them, bounded in case
     * this thread died holding a stream's lock. Not at all when this thread
     * died inside the flush: the stream is mid-write. */
    if (recomp_stdio_flushing_here()) {
        /* nothing: see above */
    } else if (code == 0xC00000FDu) {
        recomp_stdio_flush_from_backstop(250);
    } else {
        fflush(stdout);
        fflush(stderr);
    }
    recomp_fatal_format(line, sizeof line, what,
                        recomp_fatal_exception_name(code),
                        ep && ep->ExceptionRecord
                            ? ep->ExceptionRecord->ExceptionAddress : NULL);
    win_write(line);
    recomp_fatal_run_on_death();
    return verdict;
}

/* UCRT abort() raises SIGABRT before it ends the process; returning from
 * here lets it go on to end it as before. */
static void fatal_abort(int sig)
{
    char line[128];

    (void)sig;
    if (InterlockedExchange((volatile LONG *)&s_reported, 1))
        return;
    fflush(stdout);   /* the held lines first, as above */
    fflush(stderr);
    recomp_fatal_format(line, sizeof line, "SIGABRT", "abort()", NULL);
    win_write(line);
    recomp_fatal_run_on_death();
}

void recomp_fatal_install(void)
{
    void (*prev)(int);

    if (InterlockedExchange((volatile LONG *)&s_installed, 1))
        return;
    s_prev_filter = SetUnhandledExceptionFilter(fatal_filter);
    prev = signal(SIGABRT, fatal_abort);
    if (prev != SIG_DFL && prev != SIG_ERR)
        signal(SIGABRT, prev);   /* someone else's on purpose: keep it */
}

#else /* !_WIN32 */

/* ---- POSIX --------------------------------------------------------------- */

/* The interrupted pc from the signal frame, where si_addr does not hold it. */
static const void *frame_pc(void *ucv)
{
#if defined(__APPLE__) && defined(__aarch64__)
    ucontext_t *uc = ucv;
    return uc && uc->uc_mcontext ? (const void *)uc->uc_mcontext->__ss.__pc
                                 : NULL;
#elif defined(__APPLE__) && defined(__x86_64__)
    ucontext_t *uc = ucv;
    return uc && uc->uc_mcontext ? (const void *)uc->uc_mcontext->__ss.__rip
                                 : NULL;
#else
    (void)ucv;
    return NULL;
#endif
}

static void fatal_signal(int sig, siginfo_t *si, void *ucv)
{
    char line[256];
    const char *detail = NULL;
    const void *pc = NULL;
    struct sigaction dfl;

    if (!__atomic_exchange_n(&s_reported, 1, __ATOMIC_SEQ_CST)) {
        int sent = si && (si->si_code <= 0 || si->si_code == SI_USER
                          || si->si_code == SI_QUEUE);

        if (sig == SIGABRT)
            detail = "abort()";
#if defined(__APPLE__)
        /* Darwin does not tell a trap from a raise here: a brk or ud2 and a
         * pthread_kill (its raise) can both arrive with si_code 0, and a
         * raised SIGILL gets 2, the code of a real one. The frame's pc
         * still says where it stopped. */
        else if (sig == SIGTRAP || sig == SIGILL) {
            detail = sent ? "trap or raise"
                   : sig == SIGILL ? "illegal instruction" : "trap";
            pc = frame_pc(ucv);
        }
#endif
        /* Sent, not faulted: Linux marks kill and tgkill with codes <= 0,
         * Darwin a pthread_kill with 0 and kill with SI_USER. A fault's code
         * is a positive ILL_, FPE_ or TRAP_ value. */
        else if (sent)
            detail = "sent by kill or raise";
        else {
            detail = sig == SIGILL ? "illegal instruction"
                   : sig == SIGFPE ? "arithmetic fault" : "trap";
            /* For these the kernel puts the faulting instruction here. */
            if (sig != SIGTRAP && si)
                pc = si->si_addr;
        }
        recomp_fatal_format(line, sizeof line, recomp_fatal_signal_name(sig),
                            detail, pc);
        /* write(2): async-signal-safe, and stderr is the log's descriptor. */
        {
            size_t len = strlen(line), off = 0;
            while (off < len) {
                ssize_t w = write(STDERR_FILENO, line + off, len - off);
                if (w <= 0)
                    break;
                off += (size_t)w;
            }
        }
        recomp_fatal_run_on_death();
    }
    /* End as the process would have: the default action, by the same
     * signal, so the exit status and any core dump are unchanged. The signal
     * is blocked here, so the raise lands as this handler returns; a fault
     * would also come back by re-executing the instruction. */
    memset(&dfl, 0, sizeof dfl);
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    sigaction(sig, &dfl, NULL);
    raise(sig);
}

void recomp_fatal_install(void)
{
    static const int sigs[] = {
        SIGABRT, SIGILL, SIGFPE,
#ifdef SIGTRAP
        SIGTRAP,
#endif
    };
    struct sigaction sa, old;
    size_t i;

    if (__atomic_exchange_n(&s_installed, 1, __ATOMIC_SEQ_CST))
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = fatal_signal;
    /* SA_ONSTACK: when the thread has an alternate stack (the games set one
     * for their SIGSEGV handler), the report runs there too. */
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof sigs / sizeof sigs[0]; i++) {
        /* Only over the default: a debugger's SIGTRAP or a game's own
         * handler keeps its signal. */
        if (sigaction(sigs[i], NULL, &old) == 0
            && !(old.sa_flags & SA_SIGINFO) && old.sa_handler == SIG_DFL)
            sigaction(sigs[i], &sa, NULL);
    }
}

#endif /* _WIN32 */
