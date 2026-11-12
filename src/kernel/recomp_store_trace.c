/**
 * Store tracing: the runtime half of --trace-stores (tools.recomp).
 *
 * templates/runtime/recomp_store_trace.h is the other half: the lifted code
 * tests each store against the ranges here and calls recomp_store_log for a
 * hit. This file includes that header (its directory is on this one file's
 * include path), so the two cannot drift.
 *
 * A hit is a 32-byte row in the writing thread's ring. Every ring is written
 * out when any ring fills, when FLUSH_FLIPS flips have passed since the last
 * write (whichever thread logs first after that writes all of them, so a
 * thread that logged once and went quiet loses nothing), and when the run
 * ends by any path that runs code: atexit, the windows' exit hooks (before
 * ExitProcess and TerminateProcess, which skip atexit), the console handler
 * on Windows, SIGINT and SIGTERM on POSIX (how the benches stop a run), and
 * the fatal reporter, the watchdog and a game's own SIGSEGV/SIGBUS handler
 * (recomp_fatal_on_death; the game calls recomp_fatal_run_on_death). Only a
 * hard kill (SIGKILL, taskkill /F) or a crash nothing reports loses rows:
 * those logged since the last write. A stop signal landing on a thread in
 * the middle of appending skips that thread's ring (its lock is held below
 * the handler); the periodic flush blocks SIGINT and SIGTERM while it holds
 * the global lock, so the signal goes to another thread or waits for it.
 *
 * Each ring has a lock that its owner takes to append and a flusher takes to
 * write it, so a row is written once and never half-filled. The file is a
 * raw descriptor, written with write(2) under the global lock, so the
 * signal-time flush is async-signal-safe; the end-of-run flushes only try
 * the locks, for a bounded time, since the thread a signal interrupted may
 * hold one. After the final flush the trace is off and the descriptor closed,
 * so a late row is dropped rather than written to a closed file.
 *
 * The file is a 16-byte header and then rows; tools/recomp/store_trace.py
 * reads it. Rows from different threads interleave by flush, not by time:
 * `seq` is the global order.
 *
 * Keys (RECOMP_TRACE=key=value,...):
 *   stores=LO+LEN|LO-HI,...  ranges (unset: the --trace-stores defaults;
 *                            0 or off: none). Up to 32.
 *   stores_out=PATH          the file (store_trace.bin in the working dir)
 *   stores_max=N             rows before the trace stops (4194304, 128 MB;
 *                            0: no limit)
 *
 * Limits: the rings are per thread, 256 KB each, never freed, and the thread
 * index in a row is 8 bits, so it wraps after 256 tracing threads (a debug
 * tool; the titles traced so far use a handful).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <signal.h>
#include <fcntl.h>

#include "recomp_env.h"
#include "xbox_memory_layout.h"
#include "recomp_store_trace.h"
#include "recomp_exit_hook.h"
#include "recomp_fatal.h"

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#define OPEN_OUT(p) _open((p), _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, \
                          _S_IREAD | _S_IWRITE)
#define WRITE_FD(fd, b, n) _write((fd), (b), (unsigned)(n))
#define CLOSE_FD(fd) _close(fd)
#else
#include <unistd.h>
#include <time.h>
#include <sched.h>
#include <pthread.h>
#define OPEN_OUT(p) open((p), O_WRONLY | O_CREAT | O_TRUNC, 0644)
#define WRITE_FD(fd, b, n) write((fd), (b), (n))
#define CLOSE_FD(fd) close(fd)
#endif

int32_t g_store_trace_state;
uint32_t g_store_trace_nranges;
uint64_t g_store_trace_lo[RECOMP_STORE_TRACE_MAX_RANGES];
uint64_t g_store_trace_hi[RECOMP_STORE_TRACE_MAX_RANGES];

extern ptrdiff_t g_xbox_mem_offset;
uint32_t nv2a_pb_flips(void);

/* The row. Fixed layout, little-endian, so the reader needs no build info:
 * guest va of the store, the address written, the function it is in, the
 * flip it happened in, the global sequence number, the size written, the
 * kind (recomp_store_trace.h), a small thread index, and the first
 * min(size, 8) bytes the store left behind. A string row (rep stos/movs) is
 * clipped to the part inside each traced range it touches, and a clipped part
 * over 0xFFFF bytes is split into 0xFFFF-byte rows, so the 16-bit size never
 * saturates and a lookup of any address in it finds its row. */
typedef struct {
    uint32_t va, addr, func, frame, seq;
    uint16_t size;
    uint8_t kind, thread;
    uint64_t value;
} store_row;

#define RING_ROWS 8192
/* Flips between writes: two seconds at 30 Hz, one at 60. */
#define FLUSH_FLIPS 60
/* How long an end-of-run flush tries a lock before it gives that ring up. */
#define FINAL_TRY_MS 500
/* How long the periodic flush waits for a ring's owner. On Windows the owner
 * can be suspended (SuspendThread) in the middle of an append; its ring then
 * waits for the next interval instead of hanging the flusher. */
#define PERIODIC_TRY_MS 50

typedef struct store_ring {
    store_row rows[RING_ROWS];
    volatile long busy;          /* the owner appending, or a flusher writing */
    uint32_t n;                  /* under busy */
    uint8_t index;
    struct store_ring *next;     /* set once, before the ring is published */
} store_ring;

static RECOMP_TLS store_ring *t_ring;
static store_ring *volatile s_rings;
static int s_fd = -1;                    /* under s_lock */
static uint32_t s_seq;
static uint64_t s_rows, s_max_rows;      /* under s_lock */
static uint32_t s_last_flush_frame;
static uint8_t s_threads;
static volatile long s_lock;

/* The flip clock; the ctest swaps it for its own. */
static uint32_t (*s_frames)(void) = nv2a_pb_flips;
void recomp_store_trace_test_frames(uint32_t (*fn)(void));
void recomp_store_trace_test_frames(uint32_t (*fn)(void))
{
    s_frames = fn ? fn : nv2a_pb_flips;
}

static void nap_ms(unsigned ms)
{
#if defined(_WIN32)
    Sleep(ms);
#else
    struct timespec ts = {0, (long)ms * 1000000L};
    while (nanosleep(&ts, &ts) != 0)
        ;
#endif
}

static void relax(void)
{
#if defined(_WIN32)
    SwitchToThread();
#else
    sched_yield();
#endif
}

static int try_lock(volatile long *l)
{
    return !__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE);
}

static void lock(volatile long *l)
{
    while (!try_lock(l))
        relax();
}

/* Only atomics and a sleep: usable inside a signal handler. */
static int lock_bounded(volatile long *l, unsigned ms)
{
    unsigned waited = 0;
    while (!try_lock(l)) {
        if (waited >= ms)
            return 0;
        nap_ms(1);
        waited++;
    }
    return 1;
}

static void unlock(volatile long *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

static void set_state(int32_t v)
{
    __atomic_store_n(&g_store_trace_state, v, __ATOMIC_RELEASE);
}

/* "lo+len" or "lo-hi" (hi exclusive), numbers in any strtoul base, split by
 * ',' or ';'. Returns the count parsed; a malformed entry, text after the
 * last one and ranges past the table are reported. */
static uint32_t parse_ranges(const char *spec)
{
    uint32_t n = 0;
    const char *p = spec;
    while (p && *p) {
        char *end;
        unsigned long long lo = strtoull(p, &end, 0), hi;
        if (end == p) {
            fprintf(stderr, "[store-trace] bad range at '%s'; the rest is ignored\n", p);
            break;
        }
        if (*end == '+') {
            const char *q = end + 1;
            hi = lo + strtoull(q, &end, 0);
            if (end == q) hi = lo;
        } else if (*end == '-') {
            const char *q = end + 1;
            hi = strtoull(q, &end, 0);
            if (end == q) hi = lo;
        } else {
            hi = lo + 1;
        }
        if (hi > lo && lo <= 0xFFFFFFFFull) {
            if (n == RECOMP_STORE_TRACE_MAX_RANGES) {
                fprintf(stderr, "[store-trace] more than %d ranges; the rest are ignored\n",
                        RECOMP_STORE_TRACE_MAX_RANGES);
                break;
            }
            g_store_trace_lo[n] = lo;
            g_store_trace_hi[n] = hi;
            n++;
        } else {
            fprintf(stderr, "[store-trace] empty range at '%s'; skipped\n", p);
        }
        while (*end == ' ') end++;
        if (!*end)
            break;
        if (*end != ',' && *end != ';') {
            fprintf(stderr, "[store-trace] unexpected '%s' after a range; the rest "
                    "is ignored (separate ranges with ',')\n", end);
            break;
        }
        p = end + 1;
    }
    return n;
}

static void write_all(const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    while (len && s_fd >= 0) {
        long w = (long)WRITE_FD(s_fd, p, len);
        if (w <= 0)
            break;
        p += w;
        len -= (size_t)w;
    }
}

/* Under s_lock and the ring's lock. */
static void write_ring(store_ring *r)
{
    if (!r->n)
        return;
    if (s_fd >= 0) {
        write_all(r->rows, (size_t)r->n * sizeof(store_row));
        s_rows += r->n;
    }
    r->n = 0;
}

/* Under s_lock. */
static void check_budget(void)
{
    if (s_max_rows && s_rows >= s_max_rows
        && __atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE) == 1) {
        set_state(-1);
        fprintf(stderr, "[store-trace] budget of %llu rows reached; off\n",
                (unsigned long long)s_max_rows);
    }
}

/* Writes every ring (only `one`, the caller's own, when set). Called by a
 * logging thread, which holds no lock. SIGINT and SIGTERM are blocked while
 * it holds s_lock: delivered here, their handler would wait on a lock its own
 * thread holds, give up, and lose every ring. Blocked, they go to another
 * thread or land as soon as the lock is free. */
static void flush_rings(store_ring *one)
{
    store_ring *r;
#if !defined(_WIN32)
    sigset_t stop, old;
    sigemptyset(&stop);
    sigaddset(&stop, SIGINT);
    sigaddset(&stop, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &stop, &old);
#endif
    lock(&s_lock);
    for (r = one ? one : s_rings; r; r = one ? NULL : r->next) {
        if (one)
            lock(&r->busy);
        else if (!lock_bounded(&r->busy, PERIODIC_TRY_MS))
            continue;
        write_ring(r);
        unlock(&r->busy);
    }
    check_budget();
    unlock(&s_lock);
#if !defined(_WIN32)
    pthread_sigmask(SIG_SETMASK, &old, NULL);
#endif
}

/* The end of the run: off, every ring written as far as its lock can be had
 * in time, the file closed. Best effort and async-signal-safe (no stdio, no
 * malloc). close: the process goes on (an exit hook) or is about to end. */
static void finish(int close_file)
{
    store_ring *r;
    if (__atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE) == 0)
        return;
    set_state(-1);
    if (!lock_bounded(&s_lock, FINAL_TRY_MS))
        return;
    for (r = s_rings; r; r = r->next) {
        if (!lock_bounded(&r->busy, FINAL_TRY_MS))
            continue;
        write_ring(r);
        unlock(&r->busy);
    }
    if (close_file && s_fd >= 0) {
        CLOSE_FD(s_fd);
        s_fd = -1;
    }
    unlock(&s_lock);
}

static void at_exit(void) { finish(1); }
static void at_exit_hook(const char *why) { (void)why; finish(1); }
static void at_death(void) { finish(0); }

#if defined(_WIN32)
static BOOL WINAPI at_ctrl(DWORD type)
{
    (void)type;
    finish(0);
    return FALSE; /* the next handler, or the default exit, still runs */
}
#else
static struct sigaction s_old_int, s_old_term;

static void at_signal(int sig)
{
    finish(0);
    /* Then what was there before, so the stop means what it did: the same
     * exit status. */
    sigaction(sig, sig == SIGINT ? &s_old_int : &s_old_term, NULL);
    raise(sig);
}

static void take_signal(int sig, struct sigaction *old)
{
    struct sigaction sa;
    /* In front of whatever is there (mem_stats' exit line, img_dump's
     * drain, the default), which runs next; never over ourselves, or the
     * re-raise would call us for ever, and never over an ignore. */
    if (sigaction(sig, NULL, old) != 0
        || (!(old->sa_flags & SA_SIGINFO)
            && (old->sa_handler == SIG_IGN || old->sa_handler == at_signal)))
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = at_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
#endif

static void take_exits(void)
{
#if defined(_WIN32)
    SetConsoleCtrlHandler(at_ctrl, TRUE);
#else
    take_signal(SIGINT, &s_old_int);
    take_signal(SIGTERM, &s_old_term);
#endif
    atexit(at_exit);
    recomp_exit_hook_add(at_exit_hook);
    recomp_fatal_on_death(at_death);
}

void recomp_store_trace_init(const char *defaults)
{
    const char *spec, *out;
    uint32_t n;
    static const char magic[8] = {'X','B','R','S','T','R','C','1'};
    uint32_t hdr[2] = { (uint32_t)sizeof(store_row), 1u };
    static int exits_taken;

    lock(&s_lock);
    if (g_store_trace_state != 0) {
        unlock(&s_lock);
        return;
    }
    spec = recomp_env(RENV_STORE_TRACE);
    if (!spec)
        spec = defaults;
    n = 0;
    if (spec && *spec && strcmp(spec, "0") != 0 && strcmp(spec, "off") != 0) {
        if (strcmp(spec, "1") == 0)
            fprintf(stderr, "[store-trace] a bare 'stores' names no range; off "
                    "(stores=LO+LEN,...)\n");
        else
            n = parse_ranges(spec);
    }
    if (!n) {
        set_state(-1);
        unlock(&s_lock);
        return;
    }
    out = recomp_env(RENV_STORE_TRACE_OUT);
    if (!out || !*out)
        out = "store_trace.bin";
    s_fd = OPEN_OUT(out);
    if (s_fd < 0) {
        fprintf(stderr, "[store-trace] cannot open %s; off\n", out);
        set_state(-1);
        unlock(&s_lock);
        return;
    }
    write_all(magic, sizeof magic);
    write_all(hdr, sizeof hdr);
    s_rows = 0;
    s_max_rows = (uint64_t)recomp_env_ulong(RENV_STORE_TRACE_MAX, 4194304ul);
    s_last_flush_frame = s_frames();
    g_store_trace_nranges = n;
    if (!exits_taken) {
        exits_taken = 1;
        take_exits();
    }
    fprintf(stderr, "[store-trace] %u range(s) from %s -> %s, budget %llu rows\n",
            (unsigned)n, spec == defaults ? "--trace-stores" : "RECOMP_TRACE",
            out, (unsigned long long)s_max_rows);
    set_state(1);
    unlock(&s_lock);
}

/* Appends one row to the caller's ring; 1 when the ring is now full. */
static int append(store_ring *r, uint32_t va, uint32_t addr, uint32_t size,
                  uint32_t func, uint32_t kind, uint32_t frame)
{
    store_row *row;
    uint32_t take;
    int full;

    lock(&r->busy);
    if (r->n == RING_ROWS) {
        /* Full and not yet written (a final flush gave up on it): drop. */
        unlock(&r->busy);
        return 1;
    }
    row = &r->rows[r->n];
    row->va = va;
    row->addr = addr;
    row->func = func;
    row->frame = frame;
    row->seq = __atomic_fetch_add(&s_seq, 1u, __ATOMIC_RELAXED);
    row->size = (uint16_t)(size > 0xFFFF ? 0xFFFF : size);
    row->kind = (uint8_t)kind;
    row->thread = r->index;
    row->value = 0;
    take = size < 8 ? size : 8;
    memcpy(&row->value, (const void *)((uintptr_t)addr + g_xbox_mem_offset), take);
    r->n++;
    full = r->n == RING_ROWS;
    unlock(&r->busy);
    return full;
}

void recomp_store_log(uint32_t va, uint32_t addr, uint32_t size,
                      uint32_t func, uint32_t kind)
{
    store_ring *r = t_ring;
    uint32_t frame, last;
    int full = 0;

    if (__atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE) != 1)
        return;
    frame = s_frames();
    if (!r) {
        r = (store_ring *)calloc(1, sizeof *r);
        if (!r)
            return;
        lock(&s_lock);
        r->index = s_threads++;
        r->next = s_rings;
        s_rings = r;
        unlock(&s_lock);
        t_ring = r;
    }
    if (kind == RECOMP_STORE_KIND_STRING) {
        /* Rows for each traced range the string touches, clipped to it and
         * cut to the 16-bit size; a ring that fills between them is written
         * out at once, so a long string drops nothing. */
        uint64_t lo = addr, hi = lo + size;
        uint32_t i;
        for (i = 0; i < g_store_trace_nranges; i++) {
            uint64_t a = lo > g_store_trace_lo[i] ? lo : g_store_trace_lo[i];
            uint64_t b = hi < g_store_trace_hi[i] ? hi : g_store_trace_hi[i];
            while (a < b) {
                uint32_t n = b - a > 0xFFFF ? 0xFFFF : (uint32_t)(b - a);
                if (full) {
                    flush_rings(r);
                    full = 0;
                    if (__atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE) != 1)
                        return;  /* the budget ran out on that write */
                }
                full = append(r, va, (uint32_t)a, n, func, kind, frame);
                a += n;
            }
        }
    } else {
        full = append(r, va, addr, size, func, kind, frame);
    }

    last = __atomic_load_n(&s_last_flush_frame, __ATOMIC_RELAXED);
    if (frame - last >= FLUSH_FLIPS
        && __atomic_compare_exchange_n(&s_last_flush_frame, &last, frame, 0,
                                       __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
        flush_rings(NULL);       /* every ring: the quiet threads' too */
    else if (full)
        flush_rings(r);
}

void recomp_store_trace_flush(void)
{
    if (__atomic_load_n(&g_store_trace_state, __ATOMIC_ACQUIRE) == 0)
        return;
    flush_rings(NULL);
}

/* The end of the run, for a caller that knows it (a test). */
void recomp_store_trace_close(void);
void recomp_store_trace_close(void)
{
    finish(1);
}
