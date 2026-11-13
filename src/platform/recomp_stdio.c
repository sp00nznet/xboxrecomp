/*
 * recomp_stdio.c - stdout and stderr buffering and the flush once a flip
 * (recomp_stdio.h).
 */
#include "recomp_stdio.h"

#if defined(_WIN32)
#include <io.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Big enough for a flip's lines many times over (about 1 KB with the flip
 * log on), so the CRT only writes on its own when a burst of tracing fills
 * it between two flips. */
#define BUF_BYTES (64u << 10)

/* stdout and stderr. */
#define MARKED 2
static FILE *s_marked[MARKED];
static char s_buf[MARKED][BUF_BYTES];
static int s_nmarked;
/* Read without a lock by recomp_stdio_flush on every flip: set once, at boot,
 * before the threads that flush. */
static volatile int s_any;

int recomp_stdio_policy(int windows, int tty)
{
    return windows && !tty;
}

void recomp_stdio_apply(FILE *f, int buffer)
{
    if (!buffer || s_nmarked >= MARKED) {
        setvbuf(f, NULL, _IONBF, 0);
        return;
    }
    setvbuf(f, s_buf[s_nmarked], _IOFBF, BUF_BYTES);
    s_marked[s_nmarked++] = f;
    __atomic_store_n(&s_any, 1, __ATOMIC_RELEASE);
}

#if defined(_WIN32)

/* GetTickCount64 of the last flush, low 32 bits, for the backstop. */
static volatile LONG s_last_ms;
/* The thread inside flush_marked, 0 when none: a death on that thread
 * leaves the streams alone (recomp_stdio_flushing_here). */
static volatile LONG s_flusher;
/* recomp_stdio_flush_from_backstop: the request and the reply. Auto-reset,
 * made before the backstop thread starts. */
static HANDLE s_kick, s_done;

static void flush_marked(void)
{
    LONG me = (LONG)GetCurrentThreadId();

    InterlockedExchange(&s_flusher, me);
    for (int i = 0; i < s_nmarked; i++)
        fflush(s_marked[i]);
    InterlockedCompareExchange(&s_flusher, 0, me);
    InterlockedExchange(&s_last_ms, (LONG)(DWORD)GetTickCount64());
}

/* Lines printed with no flip to follow them (the boot, a hang, a run with no
 * walker) reach the log within about a second. It flushes only after a quiet
 * second, so with flips coming it never takes the streams' locks. A kick
 * (recomp_stdio_flush_from_backstop) flushes at once and answers. */
static DWORD WINAPI backstop_main(LPVOID unused)
{
    (void)unused;
    for (;;) {
        if (WaitForSingleObject(s_kick, 250) == WAIT_OBJECT_0) {
            flush_marked();
            SetEvent(s_done);
        } else if ((DWORD)GetTickCount64() - (DWORD)s_last_ms >= 1000) {
            flush_marked();
        }
    }
    return 0;
}

int recomp_stdio_flush_from_backstop(unsigned timeout_ms)
{
    if (!__atomic_load_n(&s_any, __ATOMIC_ACQUIRE) || !s_kick || !s_done)
        return 1;
    /* A reply left over from an earlier call that timed out is not this
     * call's. */
    ResetEvent(s_done);
    SetEvent(s_kick);
    return WaitForSingleObject(s_done, timeout_ms) == WAIT_OBJECT_0;
}

int recomp_stdio_flushing_here(void)
{
    return s_flusher != 0 && s_flusher == (LONG)GetCurrentThreadId();
}

/* Ctrl+C, a closed console and the stop the Proton benches send: what is
 * buffered goes out first. FALSE passes the event on to the other handlers
 * and the default end, as before. */
static BOOL WINAPI flush_ctrl(DWORD type)
{
    (void)type;
    flush_marked();
    return FALSE;
}

static int stream_is_tty(FILE *f)
{
    int fd = _fileno(f);

    /* A GUI-subsystem exe with no RECOMP_STDIO_LOG has no descriptor (-2):
     * nothing reaches a file, so nothing to buffer. _isatty is also true of
     * NUL, which needs no buffer either. */
    return fd < 0 || _isatty(fd);
}

int recomp_stdio_buffer(void)
{
    static int done;
    int out, err;

    if (done)
        return s_any;
    done = 1;
    out = recomp_stdio_policy(1, stream_is_tty(stdout));
    err = recomp_stdio_policy(1, stream_is_tty(stderr));
    recomp_stdio_apply(stdout, out);
    recomp_stdio_apply(stderr, err);
    if (s_any) {
        HANDLE t;

        InterlockedExchange(&s_last_ms, (LONG)(DWORD)GetTickCount64());
        SetConsoleCtrlHandler(flush_ctrl, TRUE);
        s_kick = CreateEventA(NULL, FALSE, FALSE, NULL);
        s_done = CreateEventA(NULL, FALSE, FALSE, NULL);
        t = CreateThread(NULL, 0, backstop_main, NULL, 0, NULL);
        if (t)
            CloseHandle(t);
    }
    return s_any;
}

#else /* !_WIN32 */

static void flush_marked(void)
{
    for (int i = 0; i < s_nmarked; i++)
        fflush(s_marked[i]);
}

int recomp_stdio_buffer(void)
{
    static int done;

    if (done)
        return s_any;
    done = 1;
    /* The policy says no on POSIX (see the header): both stay unbuffered. */
    recomp_stdio_apply(stdout, recomp_stdio_policy(0, isatty(fileno(stdout))));
    recomp_stdio_apply(stderr, recomp_stdio_policy(0, isatty(fileno(stderr))));
    return s_any;
}

/* Nothing is ever buffered here, so nothing is held. */
int recomp_stdio_flush_from_backstop(unsigned timeout_ms)
{
    (void)timeout_ms;
    return 1;
}

int recomp_stdio_flushing_here(void)
{
    return 0;
}

#endif /* _WIN32 */

void recomp_stdio_flush(void)
{
    if (__atomic_load_n(&s_any, __ATOMIC_ACQUIRE))
        flush_marked();
}
