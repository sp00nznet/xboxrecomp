/*
 * mem_stats.c - the game's RAM and GPU memory for the bench (mem_stats.h).
 */
#include "mem_stats.h"
#include "host_time.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#ifndef PSAPI_VERSION
#define PSAPI_VERSION 2   /* K32GetProcessMemoryInfo: kernel32, no psapi.lib */
#endif
#include <windows.h>
#include <psapi.h>
#else
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

/* ---- the GPU's notes ----------------------------------------------------- */

/* One writer (the backend's flip), any reader: aligned 64-bit atomics. */
static uint64_t s_gpu = MEM_STATS_NA, s_gpu_peak = MEM_STATS_NA;
static uint64_t s_gpu_nl = MEM_STATS_NA, s_gpu_nl_peak = MEM_STATS_NA;
static const char *s_gpu_src;
static uint64_t s_ramcache = MEM_STATS_NA, s_ramcache_peak = MEM_STATS_NA;

static void note_one(uint64_t *cur, uint64_t *peak, uint64_t v)
{
    uint64_t p;

    __atomic_store_n(cur, v, __ATOMIC_RELAXED);
    if (v == MEM_STATS_NA)
        return;
    p = __atomic_load_n(peak, __ATOMIC_RELAXED);
    if (p == MEM_STATS_NA || v > p)
        __atomic_store_n(peak, v, __ATOMIC_RELAXED);
}

void mem_stats_gpu_note(const char *src, uint64_t local, uint64_t nonlocal)
{
    note_one(&s_gpu, &s_gpu_peak, local);
    note_one(&s_gpu_nl, &s_gpu_nl_peak, nonlocal);
    __atomic_store_n(&s_gpu_src, src, __ATOMIC_RELEASE);
}

void mem_stats_ramcache_note(uint64_t used)
{
    note_one(&s_ramcache, &s_ramcache_peak, used);
}

/* ---- the process's RAM ------------------------------------------------------ */

#if defined(__linux__)
/* "VmRSS:\t  123456 kB" -> bytes; MEM_STATS_NA when the key is absent. */
static uint64_t status_kb(const char *text, const char *key)
{
    const char *p = text;
    size_t kl = strlen(key);
    uint64_t v = 0;

    while (*p) {
        if (!strncmp(p, key, kl)) {
            p += kl;
            while (*p == ' ' || *p == '\t')
                p++;
            if (*p < '0' || *p > '9')
                return MEM_STATS_NA;
            while (*p >= '0' && *p <= '9')
                v = v * 10 + (uint64_t)(*p++ - '0');
            return v * 1024;
        }
        while (*p && *p != '\n')
            p++;
        if (*p)
            p++;
    }
    return MEM_STATS_NA;
}
#endif

int mem_stats_read(struct mem_stats *m)
{
    int ok = 0;

    m->ram = m->ram_peak = m->commit = m->commit_peak = MEM_STATS_NA;
    m->ram_src = "none";
#if defined(_WIN32)
    {
        PROCESS_MEMORY_COUNTERS pmc;
        memset(&pmc, 0, sizeof pmc);
        pmc.cb = sizeof pmc;
        if (K32GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
            m->ram = pmc.WorkingSetSize;
            m->ram_peak = pmc.PeakWorkingSetSize;
            m->commit = pmc.PagefileUsage;
            m->commit_peak = pmc.PeakPagefileUsage;
            m->ram_src = "working_set";
            ok = 1;
        }
    }
#elif defined(__APPLE__)
    {
        task_vm_info_data_t vi;
        mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &n) == KERN_SUCCESS) {
            m->ram = vi.phys_footprint;
            /* The ledger peak arrived with rev 1 (macOS 10.13); before it,
             * no peak rather than a wrong one. */
            if (n >= TASK_VM_INFO_REV1_COUNT)
                m->ram_peak = (uint64_t)vi.ledger_phys_footprint_peak;
            m->ram_src = "phys_footprint";
            ok = 1;
        }
    }
#elif defined(__linux__)
    {
        char text[4096];
        int fd = open("/proc/self/status", O_RDONLY);
        if (fd >= 0) {
            ssize_t got = read(fd, text, sizeof text - 1);
            close(fd);
            if (got > 0) {
                text[got] = 0;
                m->ram = status_kb(text, "VmRSS:");
                m->ram_peak = status_kb(text, "VmHWM:");
                m->ram_src = "rss";
                ok = m->ram != MEM_STATS_NA;
            }
        }
    }
#endif
    m->gpu = __atomic_load_n(&s_gpu, __ATOMIC_RELAXED);
    m->gpu_peak = __atomic_load_n(&s_gpu_peak, __ATOMIC_RELAXED);
    m->gpu_nl = __atomic_load_n(&s_gpu_nl, __ATOMIC_RELAXED);
    m->gpu_nl_peak = __atomic_load_n(&s_gpu_nl_peak, __ATOMIC_RELAXED);
    m->ramcache = __atomic_load_n(&s_ramcache, __ATOMIC_RELAXED);
    m->ramcache_peak = __atomic_load_n(&s_ramcache_peak, __ATOMIC_RELAXED);
    m->gpu_src = __atomic_load_n(&s_gpu_src, __ATOMIC_ACQUIRE);
    if (!m->gpu_src)
        m->gpu_src = "none";
    return ok;
}

/* ---- the line ------------------------------------------------------------------ */

struct out {
    char *p;
    size_t n, len;
};

static void put(struct out *o, const char *s)
{
    while (*s) {
        if (o->len + 1 < o->n)
            o->p[o->len] = *s;
        o->len++;
        s++;
    }
    if (o->n)
        o->p[o->len < o->n ? o->len : o->n - 1] = 0;
}

static void put_u(struct out *o, uint64_t v)
{
    char d[24];
    int i = (int)sizeof d - 1;

    d[i] = 0;
    do {
        d[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    put(o, d + i);
}

/* Bytes as MiB with one decimal, rounded: integers only (the signal path). */
static void put_mib(struct out *o, const char *key, uint64_t bytes)
{
    put(o, " ");
    put(o, key);
    put(o, "=");
    if (bytes == MEM_STATS_NA) {
        put(o, "n/a");
        return;
    }
    {
        uint64_t tenths = (bytes / 1024 * 10 + 512) / 1024;
        char frac[2] = {(char)('0' + tenths % 10), 0};
        put_u(o, tenths / 10);
        put(o, ".");
        put(o, frac);
    }
}

size_t mem_stats_format(char *buf, size_t n, const char *kind, int64_t t_ms,
                        const struct mem_stats *m, const char *why)
{
    struct out o = {buf, n, 0};

    if (n)
        buf[0] = 0;
    put(&o, kind);
    if (t_ms >= 0) {
        char frac[2] = {(char)('0' + (t_ms / 100) % 10), 0};
        put(&o, " t=");
        put_u(&o, (uint64_t)t_ms / 1000);
        put(&o, ".");
        put(&o, frac);
    }
    put_mib(&o, "ram_mib", m->ram);
    put_mib(&o, "ram_peak_mib", m->ram_peak);
    put_mib(&o, "commit_mib", m->commit);
    put_mib(&o, "commit_peak_mib", m->commit_peak);
    put_mib(&o, "gpu_mib", m->gpu);
    put_mib(&o, "gpu_peak_mib", m->gpu_peak);
    put_mib(&o, "gpu_nl_mib", m->gpu_nl);
    put_mib(&o, "gpu_nl_peak_mib", m->gpu_nl_peak);
    if (m->ramcache != MEM_STATS_NA) {
        put_mib(&o, "ramcache_mib", m->ramcache);
        put_mib(&o, "ramcache_peak_mib", m->ramcache_peak);
    }
    put(&o, " ram_src=");
    put(&o, m->ram_src ? m->ram_src : "none");
    put(&o, " gpu_src=");
    put(&o, m->gpu_src ? m->gpu_src : "none");
    if (why) {
        put(&o, " why=");
        put(&o, why);
    }
    return o.len < n ? o.len : (n ? n - 1 : 0);
}

int mem_stats_parse(const char *line, char *kind, size_t kind_n, struct mem_stats *m)
{
    static const struct {
        const char *key;
        size_t off;
    } keys[] = {
        {"ram_mib", offsetof(struct mem_stats, ram)},
        {"ram_peak_mib", offsetof(struct mem_stats, ram_peak)},
        {"commit_mib", offsetof(struct mem_stats, commit)},
        {"commit_peak_mib", offsetof(struct mem_stats, commit_peak)},
        {"gpu_mib", offsetof(struct mem_stats, gpu)},
        {"gpu_peak_mib", offsetof(struct mem_stats, gpu_peak)},
        {"gpu_nl_mib", offsetof(struct mem_stats, gpu_nl)},
        {"gpu_nl_peak_mib", offsetof(struct mem_stats, gpu_nl_peak)},
        {"ramcache_mib", offsetof(struct mem_stats, ramcache)},
        {"ramcache_peak_mib", offsetof(struct mem_stats, ramcache_peak)},
    };
    const char *p = line;
    int got = 0;
    size_t i;

    for (i = 0; i < sizeof keys / sizeof keys[0]; i++)
        *(uint64_t *)((char *)m + keys[i].off) = MEM_STATS_NA;
    m->ram_src = m->gpu_src = NULL;
    if (!strncmp(p, "[MEM] ", 6))
        p += 6;
    else if (strstr(p, "[MEM] "))
        p = strstr(p, "[MEM] ") + 6;
    else if (*p == '[' || !*p)
        return -1;   /* another tag's line; no tag at all is mem_stats_format's output */
    i = 0;
    while (*p && *p != ' ') {
        if (kind && i + 1 < kind_n)
            kind[i++] = *p;
        p++;
    }
    if (kind && kind_n)
        kind[i] = 0;
    while (*p) {
        const char *eq, *end;
        size_t k;
        while (*p == ' ')
            p++;
        end = p + strcspn(p, " \r\n");
        eq = memchr(p, '=', (size_t)(end - p));
        if (eq) {
            for (k = 0; k < sizeof keys / sizeof keys[0]; k++) {
                if (strlen(keys[k].key) == (size_t)(eq - p)
                    && !strncmp(p, keys[k].key, (size_t)(eq - p))) {
                    char *stop;
                    double v = strtod(eq + 1, &stop);
                    if (stop != eq + 1 && stop <= end && v >= 0) {
                        *(uint64_t *)((char *)m + keys[k].off) =
                            (uint64_t)(v * 1048576.0 + 0.5);
                        got++;
                    }
                    break;
                }
            }
        }
        p = end;
        if (*p == '\r' || *p == '\n')
            break;
    }
    return got;
}

/* ---- the exit paths ----------------------------------------------------------- */

static int s_said;

static void emit(const char *kind, int64_t t_ms, const char *why, int raw)
{
    struct mem_stats m;
    char line[512];
    size_t len;

    mem_stats_read(&m);
    memcpy(line, "[MEM] ", 6);
    len = 6 + mem_stats_format(line + 6, sizeof line - 7, kind, t_ms, &m, why);
    line[len++] = '\n';
#if !defined(_WIN32)
    if (raw) {
        /* The signal path: no stdio (its lock may be held by the thread
         * the signal interrupted). */
        (void)!write(2, line, len);
        return;
    }
#endif
    (void)raw;
    fwrite(line, 1, len, stderr);
    fflush(stderr);
}

void mem_stats_exit_line(const char *why)
{
    if (__atomic_exchange_n(&s_said, 1, __ATOMIC_ACQ_REL))
        return;
    emit("exit", -1, why, 0);
}

static void exit_hook(const char *why)
{
    (void)why;
    mem_stats_exit_line("window");
}

static void exit_atexit(void)
{
    mem_stats_exit_line("atexit");
}

#if defined(_WIN32)
static BOOL WINAPI exit_ctrl(DWORD type)
{
    mem_stats_exit_line(type == CTRL_C_EVENT ? "ctrl-c" : "console");
    return FALSE;   /* the next handler (or the default exit) still runs */
}
#else
static struct sigaction s_old_int, s_old_term;

static void exit_signal(int sig)
{
    if (!__atomic_exchange_n(&s_said, 1, __ATOMIC_ACQ_REL))
        emit("exit", -1, sig == SIGINT ? "sigint" : "sigterm", 1);
    /* Then whatever was there before, so the stop means what it did. */
    sigaction(sig, sig == SIGINT ? &s_old_int : &s_old_term, NULL);
    raise(sig);
}
#endif

/* ---- the periodic sample ---------------------------------------------------- */

static unsigned s_every_ms;
static uint64_t s_t0_ns;

static void sample_once(void)
{
    emit("sample", (int64_t)((xbox_HostNowNs() - s_t0_ns) / 1000000u), NULL, 0);
}

#if defined(_WIN32)
static DWORD WINAPI sampler_main(LPVOID arg)
{
    (void)arg;
    for (;;) {
        Sleep(s_every_ms);
        sample_once();
    }
    return 0;
}
#else
static void *sampler_main(void *arg)
{
    (void)arg;
    for (;;) {
        struct timespec ts = {(time_t)(s_every_ms / 1000), (long)(s_every_ms % 1000) * 1000000L};
        while (nanosleep(&ts, &ts) != 0)
            ;
        sample_once();
    }
    return NULL;
}
#endif

unsigned mem_stats_period_ms(const char *v)
{
    double secs;

    /* By value, not by the first character: "0.5" is half a second, not
     * off. recomp_env gives a bare "mem" as "1". */
    if (!v || !*v)
        return 0;
    secs = strtod(v, NULL);
    if (!(secs > 0))
        return 0;
    if (secs < 0.1)
        secs = 0.1;
    if (secs > 86400)
        secs = 86400;
    return (unsigned)(secs * 1000.0 + 0.5);
}

void mem_stats_start_from_env(void)
{
    static int started;
    const char *v;

    if (__atomic_exchange_n(&started, 1, __ATOMIC_ACQ_REL))
        return;
    s_t0_ns = xbox_HostNowNs();
    recomp_exit_hook_add(exit_hook);
    atexit(exit_atexit);
#if defined(_WIN32)
    SetConsoleCtrlHandler(exit_ctrl, TRUE);
#else
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = exit_signal;
        sigemptyset(&sa.sa_mask);
        /* Only over the default: a handler someone installed on purpose
         * (an SDL build that traps SIGINT) keeps the signal. */
        if (sigaction(SIGINT, NULL, &s_old_int) == 0 && s_old_int.sa_handler == SIG_DFL)
            sigaction(SIGINT, &sa, NULL);
        if (sigaction(SIGTERM, NULL, &s_old_term) == 0 && s_old_term.sa_handler == SIG_DFL)
            sigaction(SIGTERM, &sa, NULL);
    }
#endif
    s_every_ms = mem_stats_period_ms(recomp_env(RENV_MEM_TRACE));
    if (!s_every_ms)
        return;
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
        if (h)
            CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (!pthread_create(&t, NULL, sampler_main, NULL))
            pthread_detach(t);
    }
#endif
    fprintf(stderr, "[MEM] sampling every %u ms\n", s_every_ms);
    fflush(stderr);
}
