/*
 * The title's threads on one host core (kernel_thread.c, xbox_GuestCpuMask
 * and xbox_GuestThreadPin, called where the bridge starts a guest thread).
 *
 * A title's level loader and its loader thread can add entries to one
 * global pool with a read-modify-write that the console's one CPU never
 * interleaved; on two host cores entries are lost. The cases:
 *  - the mask is one core, inside the process's set, and the same on every
 *    call, so a worker created from any core lands with the main thread;
 *  - unset (the default) and RECOMP_GUEST_CPUS=all are no mask at all, so
 *    stock threads are not pinned; =one is the lowest core, =<n> that core;
 *  - where the host has thread affinity, guest-main and a guest-worker both
 *    run on that one core, and a thread that does not pin itself keeps the
 *    process's mask whether it was created before the main pin or after it
 *    by the pinned thread (the kernel timer thread is made that way);
 *  - GetProcessAffinityMask stays the process's on a pinned caller.
 */
#include "kernel.h"
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

/* The tests change the environment the kernel reads its switches from. */
static void test_setenv(const char *name, const char *v)
{
#ifdef _WIN32
    _putenv_s(name, v ? v : "");
#else
    if (v)
        setenv(name, v, 1);
    else
        unsetenv(name);
#endif
}

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
    xbox_GuestThreadPin("guest-worker");
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
    /* A thread the host makes before the guest starts: not pinned. */
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
    /* A host thread (the kernel timer is made this way): not pinned. */
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

    test_setenv("RECOMP_GUEST_CPUS", NULL);
    CHECK(xbox_GuestCpuMask() == 0, "unset, nothing is pinned (opt-in)");
    test_setenv("RECOMP_GUEST_CPUS", "");
    CHECK(xbox_GuestCpuMask() == 0, "empty, nothing is pinned");
    test_setenv("RECOMP_GUEST_CPUS", "one");
    mask = xbox_GuestCpuMask();
    again = xbox_GuestCpuMask();
    CHECK(mask != 0, "=one is one core, got no mask");
    CHECK((mask & (mask - 1)) == 0, "the mask is one core: %#lx", (unsigned long)mask);
    CHECK(mask == again, "the choice is fixed: %#lx then %#lx",
          (unsigned long)mask, (unsigned long)again);
    CHECK(GetProcessAffinityMask(GetCurrentProcess(), &proc, &sys) && (mask & proc),
          "the core is one the process may use: mask %#lx, process %#lx",
          (unsigned long)mask, (unsigned long)proc);

    test_setenv("RECOMP_GUEST_CPUS", "all");
    CHECK(xbox_GuestCpuMask() == 0, "RECOMP_GUEST_CPUS=all leaves every core");
    {
        /* =<n>: the highest core of the process, or the lowest when the
         * number is not one the process may use. */
        char num[16];
        int hi = 0;
        while ((proc >> (hi + 1)) != 0) hi++;
        snprintf(num, sizeof num, "%d", hi);
        test_setenv("RECOMP_GUEST_CPUS", num);
        CHECK(xbox_GuestCpuMask() == ((DWORD_PTR)1 << hi),
              "RECOMP_GUEST_CPUS=%s is that core: %#lx", num,
              (unsigned long)xbox_GuestCpuMask());
        test_setenv("RECOMP_GUEST_CPUS", "63");
        CHECK(xbox_GuestCpuMask() == mask || proc == (DWORD_PTR)-1,
              "RECOMP_GUEST_CPUS=63 falls back to the lowest core");
    }
    /* The pin cases below, with the pin asked for. */
    test_setenv("RECOMP_GUEST_CPUS", "one");

    th = CreateThread(NULL, 0, early, NULL, 0, NULL);
    CHECK(th != NULL, "early thread");
    join(th);

    xbox_GuestThreadPin("guest-main");
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
