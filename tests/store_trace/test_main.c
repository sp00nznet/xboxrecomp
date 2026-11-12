/*
 * The store trace end to end, minus the lifter: the statements below are the
 * ones tools.recomp emits under --trace-stores (`_st_a = addr;` before a
 * store, RECOMP_STORE after it), run over a fake guest memory, and the file
 * the runtime writes is read back row by row.
 *
 * Checks the range test at the edges, the value read back after the store,
 * rep-string ranges (DF clear and set, and one longer than 64 KB, clipped to
 * the traced ranges and cut into 0xFFFF-byte rows inside a range over
 * 64 KB), the rows of a second thread, a full ring, the flip
 * flush that writes a quiet thread's ring, the stores_max budget, a flush
 * racing four logging threads (no row lost or written twice), the end of
 * the run by exit, by the windows' exit hook and by SIGTERM (POSIX), and
 * that stores=0 turns every hook off.
 */
#include "recomp_store_trace.h"
#include "recomp_env.h"
#include "platform/recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#if !defined(_WIN32)
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

/* recomp_store_trace.c's seams for this test. */
void recomp_store_trace_test_frames(uint32_t (*fn)(void));
void recomp_store_trace_close(void);

extern ptrdiff_t g_xbox_mem_offset;

#define MEM_VA 0x00010000u
static uint8_t guest[0x40000];   /* 0x10000..0x50000 */
static int failures;
static volatile uint32_t s_frame;
static char s_dir[512];

static uint32_t test_frames(void) { return s_frame; }

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void w32(uint32_t va, uint32_t v)
{
    memcpy(&guest[va - MEM_VA], &v, 4);
}

/* The emitted shape of `mov dword [addr], v` at guest va `va` in `func`. */
static void store32(uint32_t va, uint32_t func, uint32_t addr, uint32_t v)
{
    uint32_t _st_a, _st_n = 0;
    (void)_st_n;
    _st_a = (uint32_t)(addr);
    w32(addr, v);
    RECOMP_STORE(va, func, 4u);
}

#if defined(_WIN32)
#define setenv_(k, v) _putenv_s((k), (v))
#else
#define setenv_(k, v) setenv((k), (v), 1)
#endif

typedef struct {
    uint32_t va, addr, func, frame, seq;
    uint16_t size;
    uint8_t kind, thread;
    uint64_t value;
} row_t;

static void path_of(char *buf, size_t n, const char *name)
{
    snprintf(buf, n, "%s%s", s_dir, name);
}

/* Ends the current trace and starts one into `name` with `ranges` and any
 * extra keys. */
static void restart(const char *name, const char *ranges, const char *extra)
{
    char env[1024], path[768];
    recomp_store_trace_close();
    path_of(path, sizeof path, name);
    snprintf(env, sizeof env, "stores=%s,stores_out=%s%s%s", ranges, path,
             extra ? "," : "", extra ? extra : "");
    setenv_("RECOMP_TRACE", env);
    recomp_env_reload();
    g_store_trace_state = 0;
    recomp_store_trace_init("");
}

/* The rows of a trace file (malloc'd), or NULL; *n the count. */
static row_t *read_rows(const char *name, size_t *n)
{
    char path[768], magic[8];
    uint32_t hdr[2];
    row_t *rows;
    long len;
    FILE *f;

    path_of(path, sizeof path, name);
    *n = 0;
    f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fread(magic, 1, 8, f) != 8 || fread(hdr, 4, 2, f) != 2
        || memcmp(magic, "XBRSTRC1", 8) != 0 || hdr[0] != sizeof(row_t)) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    len = ftell(f) - 16;
    fseek(f, 16, SEEK_SET);
    rows = (row_t *)malloc(len > 0 ? (size_t)len : 1);
    *n = fread(rows, sizeof(row_t), (size_t)len / sizeof(row_t), f);
    fclose(f);
    return rows;
}

static int cmp_seq(const void *a, const void *b)
{
    uint32_t x = ((const row_t *)a)->seq, y = ((const row_t *)b)->seq;
    return x < y ? -1 : x > y;
}

/* 1 when no seq appears twice. */
static int unique_seqs(row_t *rows, size_t n)
{
    size_t i;
    qsort(rows, n, sizeof *rows, cmp_seq);
    for (i = 1; i < n; i++)
        if (rows[i].seq == rows[i - 1].seq)
            return 0;
    return 1;
}

#if !defined(_WIN32)
static void *other_thread(void *arg)
{
    (void)arg;
    store32(0x00200000u, 0x00200000u, 0x00011004u, 0x77u);
    return NULL;
}

#define RACE_THREADS 4
#define RACE_ROWS 20000
static volatile int s_race_go;

static void *race_thread(void *arg)
{
    uint32_t k = (uint32_t)(uintptr_t)arg, i;
    while (!s_race_go) { }
    for (i = 0; i < RACE_ROWS; i++)
        store32(0x00300000u + k, 0x00300000u, 0x00011000u, i);
    return NULL;
}

/* A thread that logs once and then never again, as a loader does. */
static void *quiet_thread(void *arg)
{
    (void)arg;
    store32(0x00400000u, 0x00400000u, 0x00011000u, 0x51u);
    return NULL;
}

static volatile int s_parked;
static void *parked_thread(void *arg)
{
    (void)arg;
    store32(0x00500000u, 0x00500000u, 0x00011000u, 0x52u);
    s_parked = 1;
    for (;;)
        pause();
    return NULL;
}

/* A child that logs from a parked thread and the main one, then ends the
 * run by `how`; the rows must be in the file. */
static int child_run(const char *name, int how)
{
    pid_t pid = fork();
    if (pid == 0) {
        pthread_t t;
        restart(name, "0x11000+8", NULL);
        pthread_create(&t, NULL, parked_thread, NULL);
        while (!s_parked) { }
        store32(0x00500004u, 0x00500000u, 0x00011004u, 0x53u);
        if (how == 0)
            exit(0);                          /* atexit */
        if (how == 1) {
            recomp_exit_hook_run("window");   /* before ExitProcess */
            _exit(0);
        }
        raise(SIGTERM);                       /* the benches' stop */
        _exit(9);
    }
    {
        int st = 0;
        waitpid(pid, &st, 0);
        return st;
    }
}
#endif

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "store_trace_test.bin";
    const char *slash = strrchr(out, '/');
    char env[512];
    uint32_t _st_a, _st_n, edi, ecx, g_df;
    row_t *rows;
    size_t n;
    int i;

    if (slash)
        snprintf(s_dir, sizeof s_dir, "%.*s", (int)(slash - out + 1), out);
    g_xbox_mem_offset = (ptrdiff_t)((uintptr_t)guest - MEM_VA);
    recomp_store_trace_test_frames(test_frames);
    snprintf(env, sizeof env,
             "stores=0x11000+8,0x12000-0x12010,stores_out=%s", out);
    setenv_("RECOMP_TRACE", env);
    recomp_env_reload();

    printf("ranges and rows:\n");
    store32(0x00100000u, 0x000FFFF0u, 0x00011000u, 0x11223344u);   /* hit */
    store32(0x00100004u, 0x000FFFF0u, 0x00011008u, 1u);            /* just past */
    store32(0x00100008u, 0x000FFFF0u, 0x00010FFEu, 0xAABBCCDDu);   /* straddles in */
    store32(0x0010000Cu, 0x000FFFF0u, 0x00012000u, 5u);            /* second range */
    store32(0x00100010u, 0x000FFFF0u, 0x00013000u, 6u);            /* miss */
    check(g_store_trace_state == 1, "the first hook reads the ranges");
    check(g_store_trace_nranges == 2, "a comma-separated value is two ranges");

    /* rep stosd, DF clear, 4 dwords from 0x11FF8: 0x11FF8..0x12008, logged
     * as its part in the second range, 0x12000..0x12008. */
    g_df = 0; edi = 0x00011FF8u; ecx = 4;
    _st_n = ecx * 4u; _st_a = g_df ? edi + 4u - _st_n : edi;
    memset(&guest[0x11FF8 - MEM_VA], 0x5A, 16);
    RECOMP_STORE_N(0x00100014u, 0x000FFFF0u);
    /* A zero count logs nothing. */
    _st_n = 0;
    RECOMP_STORE_N(0x00100018u, 0x000FFFF0u);
    /* DF set, 4 dwords down from edi 0x12008: 0x11FFC..0x1200C. */
    g_df = 1; edi = 0x00012008u; ecx = 4;
    _st_n = ecx * 4u; _st_a = g_df ? edi + 4u - _st_n : edi;
    memset(&guest[0x11FFC - MEM_VA], 0x6B, 16);
    RECOMP_STORE_N(0x0010001Cu, 0x000FFFF0u);
    /* A 128 KB memset over both ranges: one clipped row per range. */
    _st_n = 0x20000u; _st_a = 0x00008000u;
    memset(&guest[0x11000 - MEM_VA], 0x7C, 8);
    RECOMP_STORE_N(0x00100020u, 0x000FFFF0u);

#if !defined(_WIN32)
    {
        pthread_t t;
        pthread_create(&t, NULL, other_thread, NULL);
        pthread_join(t, NULL);
    }
#endif
    recomp_store_trace_flush();

    {
        const char *base = slash ? slash + 1 : out;
        rows = read_rows(base, &n);
    }
    check(rows != NULL, "the trace file exists, header magic, row size 32");
    if (!rows)
        return 1;
#if defined(_WIN32)
    check(n == 7, "seven rows: three hits and four string rows");
#else
    check(n == 8, "eight rows: three hits, four string rows, the other thread's");
#endif
    {
        int big = 0;
        for (i = 0; i < (int)n; i++) {
            row_t *r = &rows[i];
            if (r->va == 0x00100000u)
                check(r->addr == 0x11000u && r->size == 4 && r->value == 0x11223344u
                      && r->func == 0x000FFFF0u && r->kind == RECOMP_STORE_KIND_STORE,
                      "a hit logs va, address, size, value and function");
            if (r->va == 0x00100004u || r->va == 0x00100010u)
                check(0, "a store outside every range is not logged");
            if (r->va == 0x00100008u)
                check(r->addr == 0x10FFEu && r->value == 0xAABBCCDDu,
                      "a store overlapping the range's start is logged");
            if (r->va == 0x00100014u)
                check(r->kind == RECOMP_STORE_KIND_STRING && r->addr == 0x12000u
                      && r->size == 8 && r->value == 0x5A5A5A5A5A5A5A5Aull,
                      "rep stos (DF clear) logs its part inside the range");
            if (r->va == 0x00100018u)
                check(0, "a zero-count string logs nothing");
            if (r->va == 0x0010001Cu)
                check(r->addr == 0x12000u && r->size == 0xC
                      && r->value == 0x6B6B6B6B6B6B6B6Bull,
                      "rep stos (DF set) logs the range below edi, clipped");
            if (r->va == 0x00100020u) {
                big++;
                if (r->addr == 0x11000u)
                    check(r->size == 8 && r->value == 0x7C7C7C7C7C7C7C7Cull,
                          "a 128 KB string: the first range's 8 bytes and value");
                else
                    check(r->addr == 0x12000u && r->size == 0x10,
                          "a 128 KB string: the second range's 16 bytes");
            }
            if (r->va == 0x00200000u)
                check(r->thread == 1 && r->value == 0x77u,
                      "another thread's row has its own index");
        }
        check(big == 2, "a string over two ranges is two rows");
    }
    free(rows);

    printf("full ring:\n");
    restart("st_full.bin", "0x11000+8", NULL);
    for (i = 0; i < 8192 + 100; i++)
        store32(0x00110000u, 0x00110000u, 0x00011000u, (uint32_t)i);
    rows = read_rows("st_full.bin", &n);
    check(n == 8192, "a full ring is written when it fills, before any flush");
    free(rows);
    recomp_store_trace_flush();
    rows = read_rows("st_full.bin", &n);
    check(n == 8292 && unique_seqs(rows, n), "then the rest: 8292 rows, none twice");
    free(rows);

    printf("a string in a range over 64 KB:\n");
    restart("st_long.bin", "0x11000+0x30000", NULL);
    _st_n = 0x20010u; _st_a = 0x00011000u;
    memset(&guest[0x11000 - MEM_VA], 0x3D, 0x20010);
    RECOMP_STORE_N(0x00140000u, 0x00140000u);
    recomp_store_trace_flush();
    rows = read_rows("st_long.bin", &n);
    {
        uint32_t next = 0x11000u, sum = 0;
        int whole = n == 3;
        for (i = 0; whole && i < (int)n; i++) {
            whole = rows[i].addr == next && rows[i].size == (i < 2 ? 0xFFFF : 0x12)
                    && rows[i].value == 0x3D3D3D3D3D3D3D3Dull;
            next += rows[i].size;
            sum += rows[i].size;
        }
        check(whole && sum == 0x20010u,
              "128 KB in: three rows (0xFFFF, 0xFFFF, 0x12), back to back");
    }
    free(rows);

#if !defined(_WIN32)
    printf("flip flush:\n");
    s_frame = 1000;
    restart("st_flip.bin", "0x11000+8", NULL);
    {
        pthread_t t;
        pthread_create(&t, NULL, quiet_thread, NULL);
        pthread_join(t, NULL);
    }
    store32(0x00120000u, 0x00120000u, 0x00011000u, 1u);
    rows = read_rows("st_flip.bin", &n);
    check(n == 0, "nothing is written before the interval");
    free(rows);
    s_frame = 1000 + 60;
    store32(0x00120004u, 0x00120000u, 0x00011000u, 2u);
    rows = read_rows("st_flip.bin", &n);
    {
        int quiet = 0;
        for (i = 0; i < (int)n; i++)
            quiet += rows[i].va == 0x00400000u && rows[i].value == 0x51u;
        check(n == 3 && quiet == 1,
              "60 flips on, another thread's hit writes the quiet thread's ring");
    }
    free(rows);
#endif

    printf("budget:\n");
    restart("st_max.bin", "0x11000+8", "stores_max=100");
    for (i = 0; i < 150; i++)
        store32(0x00130000u, 0x00130000u, 0x00011000u, (uint32_t)i);
    recomp_store_trace_flush();
    check(g_store_trace_state == -1, "stores_max: the trace turns off at the budget");
    store32(0x00130004u, 0x00130000u, 0x00011000u, 7u);
    recomp_store_trace_flush();
    rows = read_rows("st_max.bin", &n);
    check(n == 150, "and logs nothing after it");
    free(rows);

#if !defined(_WIN32)
    printf("a flush racing the loggers:\n");
    restart("st_race.bin", "0x11000+8", NULL);
    {
        pthread_t t[RACE_THREADS];
        uintptr_t k;
        for (k = 0; k < RACE_THREADS; k++)
            pthread_create(&t[k], NULL, race_thread, (void *)k);
        s_race_go = 1;
        for (i = 0; i < 2000; i++) {
            recomp_store_trace_flush();
            s_frame += 7;       /* the flip flush as well, from any thread */
        }
        for (k = 0; k < RACE_THREADS; k++)
            pthread_join(t[k], NULL);
    }
    recomp_store_trace_flush();
    rows = read_rows("st_race.bin", &n);
    {
        size_t whole = 0;
        for (i = 0; i < (int)n; i++)
            whole += rows[i].va >= 0x00300000u && rows[i].va < 0x00300000u + RACE_THREADS
                     && rows[i].size == 4;
        check(n == (size_t)RACE_THREADS * RACE_ROWS && whole == n,
              "every row once and whole (4 x 20000)");
        check(unique_seqs(rows, n), "no row written twice");
    }
    free(rows);

    printf("the end of the run:\n");
    {
        static const char *names[3] = {"st_exit.bin", "st_hook.bin", "st_term.bin"};
        static const char *what[3] = {
            "exit: atexit writes every ring, a parked thread's too",
            "the windows' exit hook (ExitProcess) writes them",
            "SIGTERM writes them, then ends as SIGTERM",
        };
        int how;
        recomp_store_trace_close();
        for (how = 0; how < 3; how++) {
            int st = child_run(names[how], how);
            int ended = how == 2 ? WIFSIGNALED(st) && WTERMSIG(st) == SIGTERM
                                 : WIFEXITED(st) && WEXITSTATUS(st) == 0;
            rows = read_rows(names[how], &n);
            check(ended && n == 2, what[how]);
            free(rows);
        }
    }
#endif

    printf("off:\n");
    recomp_store_trace_close();
    setenv_("RECOMP_TRACE", "stores=0");
    recomp_env_reload();
    g_store_trace_state = 0;
    check(!recomp_store_hit(0x11000u, 4), "stores=0 leaves every hook off");
    check(g_store_trace_state == -1, "and says so once");
    check(RECOMP_STORE_TRACE_MAX_RANGES == 32, "the range table is the library's");

    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
