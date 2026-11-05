/*
 * host_prof.c - the in-process sampling profiler (host_prof.h).
 *
 * Nothing here may take a lock or allocate while another thread is
 * suspended: that thread may hold the heap or stdio lock, and the sampler
 * would wait on it for ever. So the thread list (which allocates) is
 * refreshed between rounds, and a round only does system calls and stores
 * into the static table.
 */
#include "host_prof.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#include <tlhelp32.h>
#define HOST_PROF_OK 1
#elif defined(__APPLE__)
#include <dlfcn.h>
#include <mach/mach.h>
#include <pthread.h>
#include <time.h>
#define HOST_PROF_OK 1
#endif

#define SLOTS 1024 /* threads seen over the run */
#define TABLE (1u << 17) /* distinct (thread, pc) pairs */
#define NAME 32

typedef struct {
    uintptr_t pc;
    uint32_t slot1; /* slot + 1; 0 = empty */
    uint32_t count;
} entry;

typedef struct {
    unsigned long tid;
    char name[NAME];
    uint64_t samples;
} slot_info;

static entry s_tab[TABLE];
static slot_info s_slot[SLOTS];
static int s_nslots;
static uint64_t s_samples, s_dropped, s_rounds;
static unsigned s_hz;
static char s_out[1024];
static volatile int s_running, s_stop, s_dumped;
/* Set while the sampler is inside a round (refresh and sample), not while
 * it naps: the dump waits on this, not on s_running, so a dump during the
 * start delay or between rounds sorts the table instead of finding the
 * sampler "busy" for a whole second and writing it unsorted. */
static volatile int s_in_round;
static int s_all_threads; /* Windows: no usable cycle counter, every thread counts */

static int slot_for(unsigned long tid, const char *name)
{
    for (int i = 0; i < s_nslots; i++)
        if (s_slot[i].tid == tid) {
            if (name && name[0])
                snprintf(s_slot[i].name, NAME, "%s", name);
            return i;
        }
    if (s_nslots == SLOTS)
        return -1;
    s_slot[s_nslots].tid = tid;
    snprintf(s_slot[s_nslots].name, NAME, "%s", name && name[0] ? name : "?");
    return s_nslots++;
}

static void record(int slot, uintptr_t pc)
{
    uint64_t h = ((uint64_t)pc * 0x9E3779B97F4A7C15ull) ^ (uint64_t)slot * 0xC2B2AE3D27D4EB4Full;
    uint32_t i = (uint32_t)(h >> 40) & (TABLE - 1);

    /* The dump may be compacting s_tab; a sample taken after the stop is
     * dropped rather than written into a table being sorted. */
    if (s_stop)
        return;
    s_samples++;
    s_slot[slot].samples++;
    for (uint32_t n = 0; n < TABLE; n++, i = (i + 1) & (TABLE - 1)) {
        entry *e = &s_tab[i];
        if (e->slot1 == 0) {
            e->pc = pc;
            e->slot1 = (uint32_t)slot + 1;
            e->count = 1;
            return;
        }
        if (e->pc == pc && e->slot1 == (uint32_t)slot + 1) {
            e->count++;
            return;
        }
    }
    s_dropped++;
}

uint64_t host_prof_samples(void)
{
    return s_samples;
}

uintptr_t host_prof_top_pc(const char *name)
{
    uintptr_t best = 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < TABLE; i++) {
        const entry *e = &s_tab[i];
        if (e->slot1 && !strcmp(s_slot[e->slot1 - 1].name, name) && e->count > n) {
            n = e->count;
            best = e->pc;
        }
    }
    return best;
}

/* ---- host threads ------------------------------------------------------ */

#if defined(_WIN32)

typedef HRESULT(WINAPI *get_desc_fn)(HANDLE, PWSTR *);

typedef struct {
    HANDLE h;
    DWORD tid;
    int slot;
    ULONG64 cycles;
    int live;
} wthread;

static wthread s_th[256];
static int s_nth;

static void thread_name(HANDLE h, char *out)
{
    static get_desc_fn get_desc;
    static int resolved;
    PWSTR w = NULL;
    int i;

    out[0] = 0;
    if (!resolved) {
        HMODULE m = GetModuleHandleA("kernel32.dll");
        if (m)
            get_desc = (get_desc_fn)(void *)GetProcAddress(m, "GetThreadDescription");
        resolved = 1;
    }
    if (!get_desc || FAILED(get_desc(h, &w)) || !w)
        return;
    for (i = 0; w[i] && i < NAME - 1; i++)
        out[i] = w[i] < 128 ? (char)w[i] : '?';
    out[i] = 0;
    LocalFree(w);
}

static void refresh_threads(void)
{
    DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te;
    int i;

    for (i = 0; i < s_nth; i++)
        s_th[i].live = 0;
    if (snap == INVALID_HANDLE_VALUE)
        return;
    te.dwSize = sizeof te;
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self)
            continue;
        for (i = 0; i < s_nth && s_th[i].tid != te.th32ThreadID; i++)
            ;
        if (i == s_nth) {
            HANDLE h;
            if (s_nth == (int)(sizeof s_th / sizeof s_th[0]))
                continue;
            h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                           FALSE, te.th32ThreadID);
            if (!h)
                continue;
            s_th[i].h = h;
            s_th[i].tid = te.th32ThreadID;
            s_th[i].cycles = 0;
            QueryThreadCycleTime(h, &s_th[i].cycles);
            s_nth++;
        }
        {
            char name[NAME];
            thread_name(s_th[i].h, name);
            s_th[i].slot = slot_for(te.th32ThreadID, name);
        }
        s_th[i].live = 1;
    }
    CloseHandle(snap);
    /* Drop the threads that ended: their handles would keep the ids. */
    for (i = 0; i < s_nth;) {
        if (!s_th[i].live) {
            CloseHandle(s_th[i].h);
            s_th[i] = s_th[--s_nth];
        } else {
            i++;
        }
    }
}

static void sample_round(void)
{
    for (int i = 0; i < s_nth; i++) {
        wthread *t = &s_th[i];
        ULONG64 c = 0;
        CONTEXT ctx;

        if (t->slot < 0)
            continue;
        if (!s_all_threads) {
            if (!QueryThreadCycleTime(t->h, &c) || c == t->cycles)
                continue;
            t->cycles = c;
        }
        if (SuspendThread(t->h) == (DWORD)-1)
            continue;
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        if (GetThreadContext(t->h, &ctx))
#if defined(_M_ARM64) || defined(__aarch64__)
            record(t->slot, (uintptr_t)ctx.Pc);
#else
            record(t->slot, (uintptr_t)ctx.Rip);
#endif
        ResumeThread(t->h);
    }
}

/* Wine's cycle counter may not move; then a thread that ran cannot be told
 * from one that waited, and every thread is sampled (waits land in ntdll). */
static void probe_cycles(void)
{
    ULONG64 a = 0, b = 0;
    volatile unsigned x = 0;
    HANDLE me = GetCurrentThread();

    QueryThreadCycleTime(me, &a);
    for (unsigned i = 0; i < 20000000u; i++)
        x += i;
    QueryThreadCycleTime(me, &b);
    s_all_threads = (b == a);
}

static void nap_ms(unsigned ms)
{
    Sleep(ms);
}

static uint64_t now_ms(void)
{
    return GetTickCount64();
}

static void module_of(uintptr_t pc, uintptr_t *base, char *path, size_t n)
{
    HMODULE m = NULL;
    *base = 0;
    snprintf(path, n, "?");
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                               | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)pc, &m)
        && m) {
        *base = (uintptr_t)m;
        if (!GetModuleFileNameA(m, path, (DWORD)n))
            snprintf(path, n, "?");
    }
}

#elif defined(__APPLE__)

typedef struct {
    thread_act_t port;
    int slot;
} mthread;

static mthread s_th[256];
static int s_nth;

static void refresh_threads(void)
{
    thread_act_array_t list;
    mach_msg_type_number_t n = 0;
    thread_act_t self = mach_thread_self();

    for (int i = 0; i < s_nth; i++)
        mach_port_deallocate(mach_task_self(), s_th[i].port);
    s_nth = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS)
        return;
    for (mach_msg_type_number_t i = 0; i < n; i++) {
        thread_identifier_info_data_t id;
        mach_msg_type_number_t cnt = THREAD_IDENTIFIER_INFO_COUNT;
        char name[NAME] = "";
        pthread_t pt;

        if (list[i] == self || s_nth == (int)(sizeof s_th / sizeof s_th[0])) {
            mach_port_deallocate(mach_task_self(), list[i]);
            continue;
        }
        if (thread_info(list[i], THREAD_IDENTIFIER_INFO, (thread_info_t)&id, &cnt)
            != KERN_SUCCESS) {
            mach_port_deallocate(mach_task_self(), list[i]);
            continue;
        }
        pt = pthread_from_mach_thread_np(list[i]);
        if (pt)
            pthread_getname_np(pt, name, sizeof name);
        s_th[s_nth].port = list[i];
        s_th[s_nth].slot = slot_for((unsigned long)id.thread_id, name);
        s_nth++;
    }
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(thread_act_t));
    mach_port_deallocate(mach_task_self(), self);
}

static void sample_round(void)
{
    for (int i = 0; i < s_nth; i++) {
        mthread *t = &s_th[i];
        thread_basic_info_data_t bi;
        mach_msg_type_number_t cnt = THREAD_BASIC_INFO_COUNT;

        if (t->slot < 0)
            continue;
        if (thread_info(t->port, THREAD_BASIC_INFO, (thread_info_t)&bi, &cnt) != KERN_SUCCESS
            || bi.run_state != TH_STATE_RUNNING)
            continue;
        if (thread_suspend(t->port) != KERN_SUCCESS)
            continue;
#if defined(__arm64__)
        {
            arm_thread_state64_t st;
            mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(t->port, ARM_THREAD_STATE64, (thread_state_t)&st, &sc)
                == KERN_SUCCESS)
                record(t->slot, (uintptr_t)arm_thread_state64_get_pc(st));
        }
#else
        {
            x86_thread_state64_t st;
            mach_msg_type_number_t sc = x86_THREAD_STATE64_COUNT;
            if (thread_get_state(t->port, x86_THREAD_STATE64, (thread_state_t)&st, &sc)
                == KERN_SUCCESS)
                record(t->slot, (uintptr_t)st.__rip);
        }
#endif
        thread_resume(t->port);
    }
}

static void probe_cycles(void)
{
}

static void nap_ms(unsigned ms)
{
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000L};
    nanosleep(&ts, NULL);
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

static void module_of(uintptr_t pc, uintptr_t *base, char *path, size_t n)
{
    Dl_info di;
    *base = 0;
    snprintf(path, n, "?");
    if (dladdr((const void *)pc, &di) && di.dli_fbase) {
        *base = (uintptr_t)di.dli_fbase;
        snprintf(path, n, "%s", di.dli_fname ? di.dli_fname : "?");
    }
}

#endif

/* ---- sampler thread and report ----------------------------------------- */

#ifdef HOST_PROF_OK

static unsigned s_delay_ms;

static void sampler_loop(void)
{
    unsigned period_us = 1000000u / s_hz;
    uint64_t next_refresh = 0, t0;
    uint64_t acc_us = 0;

    nap_ms(s_delay_ms);
    probe_cycles();
    t0 = now_ms();
    while (!s_stop) {
        uint64_t now = now_ms();
        /* Flag first, then look at s_stop again (both seq_cst, as the dump
         * stores s_stop then reads the flag): either the dump sees the
         * round or the round sees the stop. */
        __atomic_store_n(&s_in_round, 1, __ATOMIC_SEQ_CST);
        if (__atomic_load_n(&s_stop, __ATOMIC_SEQ_CST)) {
            __atomic_store_n(&s_in_round, 0, __ATOMIC_SEQ_CST);
            break;
        }
        if (now >= next_refresh) {
            refresh_threads();
            next_refresh = now + 500;
        }
        sample_round();
        s_rounds++;
        __atomic_store_n(&s_in_round, 0, __ATOMIC_SEQ_CST);
        /* Millisecond naps: a 250 Hz period of 4 ms is exact, others round
         * and the sleep error carries over so the average rate holds. */
        acc_us += period_us;
        {
            uint64_t due = t0 + acc_us / 1000u, cur = now_ms();
            nap_ms(due > cur ? (unsigned)(due - cur) : 0);
        }
    }
    s_running = 0;
}

#if defined(_WIN32)
static DWORD WINAPI sampler_main(LPVOID arg)
{
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    sampler_loop();
    return 0;
}
#else
static void *sampler_main(void *arg)
{
    (void)arg;
    pthread_setname_np("host-prof");
    sampler_loop();
    return NULL;
}
#endif

static int cmp_entry(const void *a, const void *b)
{
    const entry *x = a, *y = b;
    return x->count < y->count ? 1 : x->count > y->count ? -1 : 0;
}

#define MODS 128

/* What a sample stands for differs by host, so profiles from different
 * hosts are not directly comparable: Windows takes a thread that ran since
 * its last look (its pc may already sit at a wait), macOS one running at
 * that instant, and Wine (no cycle counter) every thread. */
static const char *prof_mode(void)
{
#if defined(_WIN32)
    return s_all_threads ? "all" : "running-since";
#else
    return "running-now";
#endif
}

void host_prof_dump(void)
{
    static uintptr_t mbase[MODS];
    static char mpath[MODS][512];
    int nmods = 0;
    FILE *f;
    uint32_t n = 0;
    int idle;

    if (__atomic_exchange_n(&s_dumped, 1, __ATOMIC_ACQ_REL))
        return;
    __atomic_store_n(&s_stop, 1, __ATOMIC_SEQ_CST);
    for (int i = 0; i < 200 && __atomic_load_n(&s_in_round, __ATOMIC_SEQ_CST); i++)
        nap_ms(5);
    /* A round can outlast the wait (under Wine, threads stuck in wineserver
     * hold SuspendThread up); its in-flight record() calls could still land
     * in s_tab, so the table is then dumped in place, unsorted. */
    idle = !__atomic_load_n(&s_in_round, __ATOMIC_SEQ_CST);
    f = fopen(s_out, "w");
    if (!f) {
        fprintf(stderr, "[PROF] cannot write %s\n", s_out);
        return;
    }
    /* Compact the table, hottest first, so the report reads top-down. */
    if (idle) {
        for (uint32_t i = 0; i < TABLE; i++)
            if (s_tab[i].slot1)
                s_tab[n++] = s_tab[i];
        qsort(s_tab, n, sizeof s_tab[0], cmp_entry);
    } else {
        n = TABLE;
        fprintf(stderr, "[PROF] sampler still in a round; dumping unsorted\n");
    }
    fprintf(f, "# recomp-prof 1 hz %u rounds %llu samples %llu dropped %llu mode %s\n", s_hz,
            (unsigned long long)s_rounds, (unsigned long long)s_samples,
            (unsigned long long)s_dropped, prof_mode());
    for (int i = 0; i < s_nslots; i++)
        fprintf(f, "thread %d %lu %llu %s\n", i, s_slot[i].tid,
                (unsigned long long)s_slot[i].samples, s_slot[i].name);
    for (uint32_t i = 0; i < n; i++) {
        uintptr_t base;
        char path[512];
        int m;

        if (!s_tab[i].slot1)
            continue;
        module_of(s_tab[i].pc, &base, path, sizeof path);
        for (m = 0; m < nmods && mbase[m] != base; m++)
            ;
        if (m == nmods && nmods < MODS) {
            mbase[nmods] = base;
            snprintf(mpath[nmods], sizeof mpath[0], "%s", path);
            fprintf(f, "module %d 0x%llx %s\n", nmods, (unsigned long long)base, path);
            nmods++;
        }
        if (m == MODS)
            m = -1;
        fprintf(f, "s %u %d 0x%llx %u\n", s_tab[i].slot1 - 1, m,
                (unsigned long long)(s_tab[i].pc - (m >= 0 ? mbase[m] : 0)), s_tab[i].count);
    }
    fclose(f);
    fprintf(stderr, "[PROF] %llu samples in %llu rounds at %u Hz (%s threads) -> %s\n",
            (unsigned long long)s_samples, (unsigned long long)s_rounds, s_hz,
            prof_mode(), s_out);
    fflush(stderr);
}

static void dump_hook(const char *why)
{
    (void)why;
    host_prof_dump();
}

static void dump_atexit(void)
{
    host_prof_dump();
}

int host_prof_start(unsigned hz, unsigned delay_ms, const char *out)
{
    if (__atomic_exchange_n(&s_running, 1, __ATOMIC_ACQ_REL))
        return 0;
    s_hz = hz < 10 ? 10 : hz > 2000 ? 2000 : hz;
    s_delay_ms = delay_ms;
    snprintf(s_out, sizeof s_out, "%s", out && out[0] ? out : "recomp-prof.txt");
    recomp_exit_hook_add(dump_hook);
    atexit(dump_atexit);
#if defined(_WIN32)
    {
        HANDLE h = CreateThread(NULL, 0, sampler_main, NULL, 0, NULL);
        if (!h) {
            s_running = 0;
            return 0;
        }
        CloseHandle(h);
    }
#else
    {
        pthread_t t;
        if (pthread_create(&t, NULL, sampler_main, NULL)) {
            s_running = 0;
            return 0;
        }
        pthread_detach(t);
    }
#endif
    fprintf(stderr, "[PROF] sampling every thread at %u Hz after %u ms -> %s\n", s_hz, delay_ms,
            s_out);
    return 1;
}

#else /* no sampler on this host */

void host_prof_dump(void)
{
}

int host_prof_start(unsigned hz, unsigned delay_ms, const char *out)
{
    (void)hz;
    (void)delay_ms;
    (void)out;
    fprintf(stderr, "[PROF] no sampler on this host (Windows and macOS only)\n");
    return 0;
}

#endif

void host_prof_start_from_env(void)
{
    const char *v = recomp_env(RENV_PROF);
    unsigned long hz;

    if (!v || v[0] == '0')
        return;
    hz = strtoul(v, NULL, 10);
    if (hz <= 1)
        hz = 250;
    host_prof_start((unsigned)hz, (unsigned)(recomp_env_double(RENV_PROF_DELAY, 0) * 1000.0),
                    recomp_env(RENV_PROF_OUT));
}
