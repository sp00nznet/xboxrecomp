/*
 * The in-process sampler (host_prof.c): a thread that spins in one function
 * must be sampled there, and the report must name the thread, the module and
 * the samples. Runs on Windows (native and Wine) and macOS.
 */
#include "host_prof.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

static int failures;
static volatile int s_go = 1;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

/* Kept out of line and busy, so its pcs are the spinner's samples. */
__attribute__((noinline)) static unsigned spin(void)
{
    volatile unsigned x = 0;
    while (s_go)
        for (unsigned i = 0; i < 1000; i++)
            x += i * 3u;
    return x;
}

#ifdef _WIN32
static DWORD WINAPI spinner(LPVOID a)
{
    typedef HRESULT(WINAPI * set_fn)(HANDLE, PCWSTR);
    set_fn set = (set_fn)(void *)GetProcAddress(GetModuleHandleA("kernel32.dll"),
                                                "SetThreadDescription");
    (void)a;
    if (set)
        set(GetCurrentThread(), L"spinner");
    spin();
    return 0;
}
#else
static void *spinner(void *a)
{
    (void)a;
    pthread_setname_np("spinner");
    spin();
    return NULL;
}
#endif

static void nap(unsigned ms)
{
#ifdef _WIN32
    Sleep(ms);
#else
    usleep(ms * 1000u);
#endif
}

int main(void)
{
    char path[] = "host_prof_test_report.txt";
    char line[600];
    int threads = 0, modules = 0, rows = 0;
    uintptr_t top, fn = (uintptr_t)&spin;
    FILE *f;

#ifdef _WIN32
    HANDLE h = CreateThread(NULL, 0, spinner, NULL, 0, NULL);
#else
    pthread_t t;
    pthread_create(&t, NULL, spinner, NULL);
#endif
    printf("host_prof: a spinning thread sampled at 1000 Hz for 0.5 s\n");
    check(host_prof_start(1000, 0, path) == 1, "the sampler starts");
    check(host_prof_start(1000, 0, path) == 0, "a second start is refused");
    nap(500);
    s_go = 0;
#ifdef _WIN32
    WaitForSingleObject(h, INFINITE);
#else
    pthread_join(t, NULL);
#endif
    host_prof_dump();
    top = host_prof_top_pc("spinner");
    printf("  samples %llu, spinner's top pc %p, spin() at %p\n",
           (unsigned long long)host_prof_samples(), (void *)top, (void *)fn);
    check(host_prof_samples() >= 50, "at least 50 samples in 0.5 s");
    check(top >= fn && top < fn + 4096, "the spinner's hottest pc is in spin()");
    f = fopen(path, "r");
    check(f != NULL, "the report is written");
    if (f) {
        check(fgets(line, sizeof line, f) && !strncmp(line, "# recomp-prof 1 ", 16),
              "the report starts with its header");
        while (fgets(line, sizeof line, f)) {
            threads += !strncmp(line, "thread ", 7) && strstr(line, " spinner");
            modules += !strncmp(line, "module ", 7);
            rows += !strncmp(line, "s ", 2);
        }
        fclose(f);
        remove(path);
    }
    check(threads == 1, "the spinner thread is named in the report");
    check(modules >= 1 && rows >= 1, "the report has modules and sample rows");
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
