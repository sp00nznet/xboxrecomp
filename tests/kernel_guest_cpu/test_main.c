/*
 * The title's threads on one host core (kernel_thread.c, xbox_GuestCpuMask
 * and xbox_GuestThreadPin, called from xbox_log_thread_role).
 *
 * One title's level loader and its loader thread add lights to one global
 * pool with a read-modify-write that the console's one CPU never
 * interleaved; on two host cores it lost entries and the terrain baked
 * black. The cases:
 *  - the mask is one core, inside the process's set, and the same on every
 *    call, so a worker created from any core lands with the main thread;
 *  - RECOMP_GUEST_CPUS=all is no mask at all, =<n> is that core;
 *  - where the host has thread affinity, guest-main and a guest-worker,
 *    logged with their routines, both run on that one core, and a thread
 *    that does not pin itself keeps the process's mask whether it was
 *    created before the main pin or after it by the pinned thread (the
 *    kernel timer thread is made that way). The early thread logs
 *    guest-main with no routine, as the host's main does before it starts
 *    the device threads, and stays unpinned; the late one logs
 *    kernel-timer;
 *  - on Win32 the pin took: a second SetThreadAffinityMask reports the
 *    core as the previous mask.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int s_fail;
#define CHECK(c, ...) do { if (!(c)) { s_fail++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
static DWORD_PTR thread_mask(void)
{
    cpu_set_t set;
    DWORD_PTR m = 0;
    int i;
    if (pthread_getaffinity_np(pthread_self(), sizeof set, &set) != 0)
        return 0;
    for (i = 0; i < (int)(8 * sizeof m) && i < CPU_SETSIZE; i++)
        if (CPU_ISSET(i, &set))
            m |= (DWORD_PTR)1 << i;
    return m;
}
#endif

static DWORD_PTR s_worker_mask = (DWORD_PTR)-1;
static DWORD_PTR s_early_mask = (DWORD_PTR)-1;   /* created before the pin */
static DWORD_PTR s_host_mask = (DWORD_PTR)-1;    /* created after it, unpinned */
static DWORD_PTR s_worker_prev;                  /* Win32: the pin's previous mask */

#if defined(_WIN32)
static DWORD_PTR thread_mask(void)
{
    /* Win32 has no query; a set returns the previous mask. */
    DWORD_PTR proc = 0, sys = 0, prev;
    GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys);
    prev = SetThreadAffinityMask(GetCurrentThread(), proc);
    if (prev)
        SetThreadAffinityMask(GetCurrentThread(), prev);
    return prev;
}
#endif

static DWORD WINAPI worker(LPVOID unused)
{
    (void)unused;
    xbox_log_thread_role("guest-worker", 0x00012340);
#if defined(__linux__) || defined(_WIN32)
    s_worker_mask = thread_mask();
#else
    s_worker_mask = 0;
#endif
    return 0;
}

static DWORD WINAPI early(LPVOID unused)
{
    (void)unused;
    xbox_log_thread_role("guest-main", 0);
#if defined(__linux__) || defined(_WIN32)
    s_early_mask = thread_mask();
#else
    s_early_mask = 0;
#endif
    return 0;
}

static DWORD WINAPI host(LPVOID unused)
{
    (void)unused;
    xbox_log_thread_role("kernel-timer", 0);
#if defined(__linux__) || defined(_WIN32)
    s_host_mask = thread_mask();
#else
    s_host_mask = 0;
#endif
    return 0;
}

static void join(HANDLE th)
{
    if (th) {
        WaitForSingleObject(th, 5000);
        CloseHandle(th);
    }
}

int main(void)
{
    DWORD_PTR proc = 0, sys = 0, mask, again;
    HANDLE th;

    recomp_env_set(RENV_GUEST_CPUS, NULL);
    mask = xbox_GuestCpuMask();
    again = xbox_GuestCpuMask();
    CHECK(mask != 0, "the default is one core, got no mask");
    CHECK((mask & (mask - 1)) == 0, "the mask is one core: %#lx", (unsigned long)mask);
    CHECK(mask == again, "the choice is fixed: %#lx then %#lx",
          (unsigned long)mask, (unsigned long)again);
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && (mask & proc),
          "the core is one the process may use: mask %#lx, process %#lx",
          (unsigned long)mask, (unsigned long)proc);

    recomp_env_set(RENV_GUEST_CPUS, "all");
    CHECK(xbox_GuestCpuMask() == 0, "guest_cpus=all leaves every core");
    {
        /* =<n>: the highest core of the process, or the lowest when the
         * number is not one the process may use. */
        char num[16];
        int hi = 0;
        while ((proc >> (hi + 1)) != 0) hi++;
        snprintf(num, sizeof num, "%d", hi);
        recomp_env_set(RENV_GUEST_CPUS, num);
        CHECK(xbox_GuestCpuMask() == ((DWORD_PTR)1 << hi),
              "guest_cpus=%s is that core: %#lx", num,
              (unsigned long)xbox_GuestCpuMask());
        recomp_env_set(RENV_GUEST_CPUS, "63");
        CHECK(xbox_GuestCpuMask() == mask || proc == (DWORD_PTR)-1,
              "guest_cpus=63 falls back to the lowest core");
    }
    recomp_env_set(RENV_GUEST_CPUS, NULL);

    th = CreateThread(NULL, 0, early, NULL, 0, NULL);
    CHECK(th != NULL, "early thread");
    join(th);

    xbox_log_thread_role("guest-main", 0x00011000);
    th = CreateThread(NULL, 0, worker, NULL, 0, NULL);
    CHECK(th != NULL, "worker thread");
    join(th);
    th = CreateThread(NULL, 0, host, NULL, 0, NULL);
    CHECK(th != NULL, "host thread");
    join(th);
#if defined(__linux__) || defined(_WIN32)
    CHECK(thread_mask() == mask, "the main thread runs on the core: %#lx, wanted %#lx",
          (unsigned long)thread_mask(), (unsigned long)mask);
    CHECK(s_worker_mask == mask, "the worker runs on the core: %#lx, wanted %#lx",
          (unsigned long)s_worker_mask, (unsigned long)mask);
    CHECK(s_early_mask == proc, "a thread made before the pin keeps the process mask: %#lx, wanted %#lx",
          (unsigned long)s_early_mask, (unsigned long)proc);
    CHECK(s_host_mask == proc, "a thread the pinned thread makes keeps the process mask: %#lx, wanted %#lx",
          (unsigned long)s_host_mask, (unsigned long)proc);
    {
        DWORD_PTR p2 = 0, s2 = 0;
        GetProcessAffinityMask(GetCurrentProcess(), &p2, &s2);
        CHECK(p2 == proc, "GetProcessAffinityMask is the process's, not the pinned caller's: %#lx, wanted %#lx",
              (unsigned long)p2, (unsigned long)proc);
    }
#else
    /* No thread affinity on this host: the pin reports it and leaves the
     * threads alone; the threads still ran. */
    CHECK(s_worker_mask == 0 && s_early_mask == 0 && s_host_mask == 0, "the threads ran");
#endif
    (void)s_worker_prev;

    if (s_fail) {
        fprintf(stderr, "%d failure(s)\n", s_fail);
        return 1;
    }
    puts("kernel_guest_cpu: ok");
    return 0;
}
