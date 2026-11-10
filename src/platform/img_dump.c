/*
 * img_dump.c - PNG or BMP debug images, under one per-run byte budget
 * (img_dump.h).
 */
#include "img_dump.h"
#include "recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

/* stb_image_write, vendored unmodified, private to this file: PNG only
 * (its BMP, TGA, JPEG and HDR writers and its stdio paths are not used). */
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#elif defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"
#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#elif defined(_MSC_VER)
#pragma warning(pop)
#endif

/* ── budget ──────────────────────────────────────────────────────────────── */

#if defined(_MSC_VER)
#include <intrin.h>
static uint64_t load64(volatile uint64_t *p)
{
    return (uint64_t)_InterlockedOr64((volatile long long *)p, 0);
}
static int cas64(volatile uint64_t *p, uint64_t old, uint64_t nu)
{
    return (uint64_t)_InterlockedCompareExchange64((volatile long long *)p, (long long)nu,
                                                   (long long)old) == old;
}
static int xchg32(volatile long *p, long v)
{
    return (int)_InterlockedExchange(p, v);
}
#else
static uint64_t load64(volatile uint64_t *p)
{
    return __atomic_load_n(p, __ATOMIC_ACQUIRE);
}
static int cas64(volatile uint64_t *p, uint64_t old, uint64_t nu)
{
    return __atomic_compare_exchange_n(p, &old, nu, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
}
static int xchg32(volatile long *p, long v)
{
    return (int)__atomic_exchange_n(p, v, __ATOMIC_ACQ_REL);
}
#endif

/* The Mac is the workbench: its disk holds the builds and the checkouts,
 * and frame dumps filled it twice. A run
 * there stops dumping at 2 GiB unless told otherwise; the PC and the Deck
 * have the room and no default cap. */
#ifdef __APPLE__
#define DEFAULT_BUDGET_MB 2048
#else
#define DEFAULT_BUDGET_MB 0
#endif

static volatile uint64_t s_used;
static volatile long s_refused_said;
static int64_t s_cap = -1; /* -1: not read yet; set once, races write the same value */

/* Whole MiB only: atol read "2G" as 2 MiB, a cap a thousand times smaller
 * than meant. Anything else keeps the default, with a line saying so. */
static long read_cap_mb(void)
{
    const char *v = recomp_env(RENV_DUMP_BUDGET_MB);
    char *end;
    long mb;

    if (!v)
        return DEFAULT_BUDGET_MB;
    mb = strtol(v, &end, 10);
    if (end == v || *end || mb < 0) {
        fprintf(stderr,
                "[DUMP] dump_budget_mb=%s: not a whole number of MiB; the default %d"
                " (0: no cap) stands\n",
                v, DEFAULT_BUDGET_MB);
        return DEFAULT_BUDGET_MB;
    }
    return mb;
}

uint64_t img_dump_budget_cap(void)
{
    if (s_cap < 0) {
        long mb = read_cap_mb();
        s_cap = mb > 0 ? (int64_t)mb << 20 : 0;
    }
    return (uint64_t)s_cap;
}

uint64_t img_dump_budget_used(void)
{
    return load64(&s_used);
}

int img_dump_budget_take(uint64_t bytes)
{
    uint64_t cap = img_dump_budget_cap(), old;

    for (;;) {
        old = load64(&s_used);
        if (cap && old + bytes > cap) {
            if (!xchg32(&s_refused_said, 1))
                fprintf(stderr,
                        "[DUMP] budget: %llu MiB written or queued, the run's cap is %llu MiB"
                        " (RECOMP_DEBUG=dump_budget_mb); no further dumps (those queued"
                        " are still written)\n",
                        (unsigned long long)(old >> 20), (unsigned long long)(cap >> 20));
            return 0;
        }
        if (cas64(&s_used, old, old + bytes))
            return 1;
    }
}

/* Bytes over the cap, for a file already promised: a PNG past its
 * reservation (it never is: png_bound is an upper bound). */
static void budget_force(uint64_t bytes)
{
    uint64_t old;

    do
        old = load64(&s_used);
    while (!cas64(&s_used, old, old + bytes));
}

void img_dump_budget_refund(uint64_t bytes)
{
    uint64_t old;

    do
        old = load64(&s_used);
    while (!cas64(&s_used, old, old > bytes ? old - bytes : 0));
}

void img_dump_budget_reset(void)
{
    s_cap = -1;
    s_used = 0;
    s_refused_said = 0;
}

/* ── encoders ────────────────────────────────────────────────────────────── */

static int ends_with_png(const char *path)
{
    size_t n = strlen(path);
    return n >= 4 && (path[n - 4] == '.') && (path[n - 3] == 'p' || path[n - 3] == 'P')
           && (path[n - 2] == 'n' || path[n - 2] == 'N')
           && (path[n - 1] == 'g' || path[n - 1] == 'G');
}

/* The fourth byte kept only when some pixel has it non-zero: the rule of
 * `frames compact`, so a PNG dumped here and one compacted from the BMP
 * hold the same pixels. */
static unsigned out_channels(const uint8_t *px, size_t pitch, uint32_t w, uint32_t h,
                             unsigned bpp, unsigned flags)
{
    uint32_t x, y;

    if (bpp != 4 || !(flags & IMG_DUMP_ALPHA))
        return 3;
    for (y = 0; y < h; y++) {
        const uint8_t *r = px + (size_t)y * pitch;
        for (x = 0; x < w; x++)
            if (r[x * 4u + 3u])
                return 4;
    }
    return 3;
}

/* B, G, R(, X) rows at `pitch` to packed R, G, B(, A): what stb takes. */
static uint8_t *repack_rgb(const uint8_t *px, size_t pitch, uint32_t w, uint32_t h,
                           unsigned bpp, unsigned ch)
{
    uint8_t *rgb = (uint8_t *)malloc((size_t)w * h * ch);
    uint32_t x, y;

    if (!rgb)
        return NULL;
    for (y = 0; y < h; y++) {
        const uint8_t *s = px + (size_t)y * pitch;
        uint8_t *d = rgb + (size_t)y * w * ch;
        for (x = 0; x < w; x++, s += bpp, d += ch) {
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            if (ch == 4)
                d[3] = s[3];
        }
    }
    return rgb;
}

/* 24-bit bottom-up, or (ch 4) 32-bit top-down: what the dumpers wrote
 * before PNG, byte for byte. */
static uint8_t *encode_bmp(const uint8_t *px, size_t pitch, uint32_t w, uint32_t h,
                           unsigned bpp, unsigned ch, int *len)
{
    uint32_t row = w * ch, pad = (4u - (row & 3u)) & 3u, y, x;
    uint64_t total = 54u + (uint64_t)(row + pad) * h;
    int32_t ih = ch == 4 ? -(int32_t)h : (int32_t)h;
    uint32_t sz = (uint32_t)total;
    uint8_t *out, *d;

    if (total > 0x7FFFFFFFu || !(out = (uint8_t *)calloc(1, (size_t)total)))
        return NULL;
    out[0] = 'B';
    out[1] = 'M';
    memcpy(out + 2, &sz, 4);
    out[10] = 54;
    out[14] = 40;
    memcpy(out + 18, &w, 4);
    memcpy(out + 22, &ih, 4);
    out[26] = 1;
    out[28] = (uint8_t)(ch * 8u);
    d = out + 54;
    for (y = 0; y < h; y++, d += pad) {
        const uint8_t *s = px + (size_t)(ch == 4 ? y : h - 1 - y) * pitch;
        for (x = 0; x < w; x++, s += bpp, d += ch) {
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            if (ch == 4)
                d[3] = s[3];
        }
    }
    *len = (int)total;
    return out;
}

static int replace_file(const char *tmp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) != 0;
#else
    return rename(tmp, path) == 0;
#endif
}

/* path.tmp and the rename. Frees data. The caller has charged the budget
 * and refunds it when this fails. */
static int put_file(const char *path, uint8_t *data, int len)
{
    char tmp[1024];
    int ok;
    FILE *f;

    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    f = fopen(tmp, "wb");
    ok = f && fwrite(data, 1, (size_t)len, f) == (size_t)len;
    if (f && fclose(f))
        ok = 0;
    /* stb's buffer (STBIW_MALLOC: malloc), or encode_bmp's. */
    free(data);
    if (ok && !replace_file(tmp, path))
        ok = 0;
    if (!ok && f)
        remove(tmp);
    return ok;
}

/* The most a w x h x ch PNG from stb can take: its deflate codes every
 * byte (the pixels plus a filter byte a row) in 9 bits at worst, its fixed
 * Huffman table's longest literal, and a match never costs more a byte
 * than that; 1 KiB covers the chunks, the zlib header and the Adler sum.
 * A queued PNG is charged this before it is queued, so a 1 from
 * img_dump_write means the file will exist, and the rest is refunded once
 * the deflate knows its size. */
static uint64_t png_bound(uint32_t w, uint32_t h, unsigned ch)
{
    uint64_t in = ((uint64_t)w * ch + 1u) * h;
    return in + in / 8u + 1024u;
}

/* A charge of `charged` bytes for a file that came out `len`. */
static void settle(uint64_t charged, uint64_t len)
{
    if (len <= charged)
        img_dump_budget_refund(charged - len);
    else
        budget_force(len - charged);
}

/* The PNG the reservation `charged` was taken for: deflate, settle, write.
 * Frees rgb. 1 when on disk. */
static int write_png(const char *path, uint8_t *rgb, uint32_t w, uint32_t h, unsigned ch,
                     uint64_t charged)
{
    int len = 0;
    uint8_t *data =
        stbi_write_png_to_mem(rgb, (int)(w * ch), (int)w, (int)h, (int)ch, &len);

    free(rgb);
    if (!data) {
        img_dump_budget_refund(charged);
        return 0;
    }
    settle(charged, (uint64_t)len);
    if (put_file(path, data, len))
        return 1;
    img_dump_budget_refund((uint64_t)len);
    return 0;
}

/* ── the PNG writers ─────────────────────────────────────────────────────── */

/* stb's deflate takes 7-35 ms for a 640x480 frame, more than a 60 Hz flip:
 * on the flip's thread it halved the walker's rate in the golden windows
 * (attract-title 29 fps against 58 on the PC, the flip floors missed). So
 * the caller only repacks the pixels (about 0.2 ms) and the deflate runs on
 * writer threads. The queue is bounded in bytes: past it the caller waits,
 * so a long every-flip dump slows the run rather than dropping frames.
 *
 * What is queued is written before the process ends by any path that runs
 * code: atexit, the window's exit hooks and its own flush before
 * ExitProcess or TerminateProcess (which skip atexit), the console handler
 * on Windows, and SIGINT/SIGTERM on POSIX, how the benches stop a golden
 * run. The drain is bounded (DRAIN_MS); a hard kill (taskkill /F) loses
 * what was still queued, and the CLI's pulls say which frame.
 *
 * When no writer thread starts, every PNG is written on the caller's
 * thread, as a BMP is. The tests build with IMG_DUMP_WRITERS 0 for that. */
#ifndef IMG_DUMP_WRITERS
#define IMG_DUMP_WRITERS 3
#endif
#define QUEUE_BYTES (64u << 20)
#define DRAIN_MS 5000

typedef struct job {
    struct job *next;
    uint8_t *rgb;
    size_t bytes;
    uint64_t charged;
    uint32_t w, h;
    unsigned ch;
    char path[1024];
} job;

static job *s_head, *s_tail;
static size_t s_queued;              /* rgb bytes in the queue */
static volatile long s_pending;      /* jobs queued or being written */
static int s_started;                /* writers tried, once (under the lock) */
static int s_writers;                /* how many started (under the lock) */
static int s_exits_taken;            /* the drains registered, once (under the lock) */

#if defined(_WIN32)
static SRWLOCK s_lock = SRWLOCK_INIT;
static CONDITION_VARIABLE s_work = CONDITION_VARIABLE_INIT, s_room = CONDITION_VARIABLE_INIT;
#define LOCK() AcquireSRWLockExclusive(&s_lock)
#define UNLOCK() ReleaseSRWLockExclusive(&s_lock)
#define WAIT(c) SleepConditionVariableSRW(&(c), &s_lock, INFINITE, 0)
#define WAKE_ONE(c) WakeConditionVariable(&(c))
#define WAKE_ALL(c) WakeAllConditionVariable(&(c))
#else
#include <pthread.h>
#include <signal.h>
#include <time.h>
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_work = PTHREAD_COND_INITIALIZER, s_room = PTHREAD_COND_INITIALIZER;
#define LOCK() pthread_mutex_lock(&s_lock)
#define UNLOCK() pthread_mutex_unlock(&s_lock)
#define WAIT(c) pthread_cond_wait(&(c), &s_lock)
#define WAKE_ONE(c) pthread_cond_signal(&(c))
#define WAKE_ALL(c) pthread_cond_broadcast(&(c))
#endif

static long pending_add(long d)
{
#if defined(_MSC_VER)
    return _InterlockedExchangeAdd(&s_pending, d) + d;
#else
    return __atomic_add_fetch(&s_pending, d, __ATOMIC_ACQ_REL);
#endif
}

static long pending_now(void)
{
#if defined(_MSC_VER)
    return _InterlockedOr(&s_pending, 0);
#else
    return __atomic_load_n(&s_pending, __ATOMIC_ACQUIRE);
#endif
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

/* Only atomics and a sleep: safe in a signal handler. A writer stuck on
 * the lock a signalled thread held would only make it time out. */
int img_dump_flush(unsigned ms)
{
    unsigned waited = 0;

    while (pending_now() > 0) {
        if (waited >= ms)
            return 0;
        nap_ms(2);
        waited += 2;
    }
    return 1;
}

#if defined(_WIN32)
static DWORD WINAPI writer_main(LPVOID arg)
#else
static void *writer_main(void *arg)
#endif
{
    job *j;

    (void)arg;
    for (;;) {
        LOCK();
        while (!s_head)
            WAIT(s_work);
        j = s_head;
        s_head = j->next;
        if (!s_head)
            s_tail = NULL;
        s_queued -= j->bytes;
        WAKE_ALL(s_room);
        UNLOCK();
        write_png(j->path, j->rgb, j->w, j->h, j->ch, j->charged);
        free(j);
        pending_add(-1);
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

static void drain_atexit(void)
{
    img_dump_flush(DRAIN_MS);
}

static void drain_hook(const char *why)
{
    (void)why;
    img_dump_flush(DRAIN_MS);
}

#if defined(_WIN32)
static BOOL WINAPI drain_ctrl(DWORD type)
{
    (void)type;
    img_dump_flush(DRAIN_MS);
    return FALSE; /* the next handler, or the default exit, still runs */
}
#else
static struct sigaction s_old_int, s_old_term;

static void drain_signal(int sig)
{
    img_dump_flush(DRAIN_MS);
    /* Then what was there before (mem_stats' exit line, the default), so
     * the stop means what it did. */
    sigaction(sig, sig == SIGINT ? &s_old_int : &s_old_term, NULL);
    raise(sig);
}

static void take_signal(int sig, struct sigaction *old)
{
    struct sigaction sa;

    /* Never over ourselves: the saved "old" would then be drain_signal,
     * and the re-raise would call it for ever. */
    if (sigaction(sig, NULL, old) != 0 || old->sa_handler == SIG_IGN
        || old->sa_handler == drain_signal)
        return;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = drain_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}
#endif

/* Under the lock, once, when a writer runs: every exit that runs code
 * waits for the queue. */
static void take_exits(void)
{
    if (s_exits_taken)
        return;
    s_exits_taken = 1;
#if defined(_WIN32)
    SetConsoleCtrlHandler(drain_ctrl, TRUE);
#else
    take_signal(SIGINT, &s_old_int);
    take_signal(SIGTERM, &s_old_term);
#endif
    atexit(drain_atexit);
    recomp_exit_hook_add(drain_hook);
}

/* Under the lock, once, at the first queued PNG. */
static void start_writers(void)
{
    int i;

    s_started = 1;
#if defined(_WIN32)
    for (i = 0; i < IMG_DUMP_WRITERS; i++) {
        HANDLE t = CreateThread(NULL, 0, writer_main, NULL, 0, NULL);
        if (t) {
            CloseHandle(t);
            s_writers++;
        }
    }
#else
    {
        /* The stops the benches send go to the game's threads, whose
         * handler waits for the writers; a writer taking one would wait for
         * itself. The faults stay open, so a crash in the deflate still
         * reaches recomp_fatal's [FATAL] line. */
        static const int k_async[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT,
                                      SIGPIPE, SIGUSR1, SIGUSR2};
        sigset_t set, old;

        sigemptyset(&set);
        for (i = 0; i < (int)(sizeof k_async / sizeof k_async[0]); i++)
            sigaddset(&set, k_async[i]);
        pthread_sigmask(SIG_BLOCK, &set, &old);
        for (i = 0; i < IMG_DUMP_WRITERS; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, writer_main, NULL) == 0) {
                pthread_detach(t);
                s_writers++;
            }
        }
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    }
#endif
    if (s_writers)
        take_exits();
    else
        fprintf(stderr, "[DUMP] no PNG writer thread started; PNGs are written on the"
                        " dumping thread\n");
}

/* 1 when queued; 0 when there is no writer (the caller writes it). */
static int queue_png(const char *path, uint8_t *rgb, uint32_t w, uint32_t h, unsigned ch,
                     uint64_t charged)
{
    job *j;

    LOCK();
    if (!s_started)
        start_writers();
    if (!s_writers) {
        UNLOCK();
        return 0;
    }
    UNLOCK();
    j = (job *)malloc(sizeof *j);
    if (!j)
        return 0;
    j->next = NULL;
    j->rgb = rgb;
    j->bytes = (size_t)w * h * ch;
    j->charged = charged;
    j->w = w;
    j->h = h;
    j->ch = ch;
    snprintf(j->path, sizeof j->path, "%s", path);
    LOCK();
    /* One frame always goes in, however big, so the wait ends. */
    while (s_queued && s_queued + j->bytes > QUEUE_BYTES)
        WAIT(s_room);
    pending_add(1);
    if (s_tail)
        s_tail->next = j;
    else
        s_head = j;
    s_tail = j;
    s_queued += j->bytes;
    WAKE_ONE(s_work);
    UNLOCK();
    return 1;
}

int img_dump_write(const char *path, const uint8_t *px, size_t pitch, uint32_t w,
                   uint32_t h, unsigned bpp, unsigned flags)
{
    unsigned ch;
    uint8_t *data;
    uint64_t charged;
    int len = 0;

    if (!path || !px || !w || !h || (bpp != 3 && bpp != 4) || w > 16384 || h > 16384)
        return 0;
    /* Past the cap nothing is written; skip the repack and the queue. */
    if (s_refused_said)
        return 0;
    if (ends_with_png(path)) {
        ch = out_channels(px, pitch, w, h, bpp, flags);
        charged = png_bound(w, h, ch);
        if (!img_dump_budget_take(charged))
            return 0;
        data = repack_rgb(px, pitch, w, h, bpp, ch);
        if (!data) {
            img_dump_budget_refund(charged);
            return 0;
        }
        if (queue_png(path, data, w, h, ch, charged))
            return 1;
        return write_png(path, data, w, h, ch, charged);
    }
    ch = bpp == 4 && (flags & IMG_DUMP_ALPHA) ? 4u : 3u;
    data = encode_bmp(px, pitch, w, h, bpp, ch, &len);
    if (!data)
        return 0;
    if (!img_dump_budget_take((uint64_t)len)) {
        free(data);
        return 0;
    }
    if (put_file(path, data, len))
        return 1;
    img_dump_budget_refund((uint64_t)len);
    return 0;
}
