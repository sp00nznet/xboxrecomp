/*
 * RtlTryEnterCriticalSection and RtlLeaveCriticalSection share one lock.
 *
 * TryEnter kept a host-lock table of its own, so a section it took was
 * released by Leave on another host lock, one the thread never took. The
 * section stayed held for every other thread, and the other lock's recursion
 * went negative. Under RECOMP_CS_MODE=guest the release was ignored. A title
 * that takes a lock with TryEnter and drops it with Leave then hangs the next
 * thread that wants it.
 *
 * One process per mode, through the thunk dispatcher like a title's calls:
 *
 *   shadow   the default, a host lock behind each guest section
 *   guest    RECOMP_CS_MODE=guest, the guest's own bytes
 *
 * Checks, in each mode:
 *   1. TryEnter on a free section succeeds; Leave releases it.
 *   2. Another thread then takes it with TryEnter, and releases it.
 *   3. While another thread holds it, TryEnter here fails.
 *
 * Runs the real memory layout on the small synthetic XBE from
 * tools/conformance, so no title and no game files are needed.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp, g_fs_base;

enum { S_INIT, S_ENTER, S_LEAVE, S_TRY, N_SLOTS };
static const uint32_t ORD[N_SLOTS] = {
    291,   /* RtlInitializeCriticalSection */
    277,   /* RtlEnterCriticalSection */
    294,   /* RtlLeaveCriticalSection */
    306,   /* RtlTryEnterCriticalSection */
};

static uint32_t scratch;                 /* guest VA of a 64 KB block */
#define THUNK_VA  (scratch)
#define STACK_A   (scratch + 0x1000)     /* this thread's guest stack */
#define STACK_B   (scratch + 0x2000)     /* the other thread's */
#define CS_VA     (scratch + 0x3000)
#define TIB_B     (scratch + 0x4000)     /* the other thread's TIB address */

static uint32_t slot_va[N_SLOTS];
static int failures;

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

/* One stdcall argument, the section; the stack is the calling thread's. */
static uint32_t call(int slot, uint32_t stack)
{
    uint32_t *sp = G(stack);

    sp[0] = 0xBEEF0001u;
    sp[1] = CS_VA;
    g_esp = stack;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(slot_va[slot])();
    return g_eax & 0xFFu;   /* BOOLEAN */
}

/* The other thread: an action, run to completion, and its result. */
enum { B_TRY_LEAVE, B_ENTER_HOLD };
static volatile int b_action, b_result, b_holding, b_release;

#ifdef _WIN32
static DWORD WINAPI thread_b(LPVOID arg)
#else
static void *thread_b(void *arg)
#endif
{
    (void)arg;
    /* A spawned worker has its own TIB, and guest-mode locks tell threads
     * apart by it; a new host thread starts with the main thread's. */
    g_fs_base = TIB_B;
    if (b_action == B_TRY_LEAVE) {
        b_result = (int)call(S_TRY, STACK_B);
        if (b_result)
            call(S_LEAVE, STACK_B);
    } else {
        call(S_ENTER, STACK_B);
        b_holding = 1;
        while (!b_release) {
#ifdef _WIN32
            Sleep(1);
#endif
        }
        call(S_LEAVE, STACK_B);
    }
    return 0;
}

#ifdef _WIN32
typedef HANDLE thread_t;
static thread_t start_b(int action)
{
    b_action = action;
    return CreateThread(NULL, 0, thread_b, NULL, 0, NULL);
}
static int join_b(thread_t t)
{
    int ok = WaitForSingleObject(t, 5000) == WAIT_OBJECT_0;
    CloseHandle(t);
    return ok;
}
#else
typedef pthread_t thread_t;
static thread_t start_b(int action)
{
    thread_t t;
    b_action = action;
    pthread_create(&t, NULL, thread_b, NULL);
    return t;
}
static int join_b(thread_t t)
{
    return pthread_join(t, NULL) == 0;
}
#endif

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "shadow";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int guest = !strcmp(mode, "guest");
    size_t xbe_len = 0;
    unsigned char *xbe;
    thread_t t;
    uint32_t r;
    int i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!guest && strcmp(mode, "shadow")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
#if defined(_WIN32)
    _putenv_s("RECOMP_CS_MODE", guest ? "guest" : "");
#else
    if (guest) setenv("RECOMP_CS_MODE", "guest", 1);
    else       unsetenv("RECOMP_CS_MODE");
#endif

    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }

    scratch = xbox_HeapAlloc(0x10000, 4096);
    for (i = 0; i < N_SLOTS; i++)
        *G(THUNK_VA + 4 * i) = 0x80000000u | ORD[i];
    xbox_kernel_set_thunk_address(THUNK_VA, N_SLOTS);
    xbox_kernel_bridge_init();
    for (i = 0; i < N_SLOTS; i++)
        slot_va[i] = *G(THUNK_VA + 4 * i);

    printf("TryEnter and Leave, %s mode\n", mode);
    call(S_INIT, STACK_A);

    r = call(S_TRY, STACK_A);
    check(r == 1, "1. TryEnter on a free section succeeds", NULL);
    call(S_LEAVE, STACK_A);

    t = start_b(B_TRY_LEAVE);
    check(join_b(t), "   the other thread finishes", NULL);
    check(b_result == 1, "2. after Leave, another thread's TryEnter succeeds",
          "the section stayed held");

    b_holding = 0;
    b_release = 0;
    t = start_b(B_ENTER_HOLD);
    while (!b_holding) {
#ifdef _WIN32
        Sleep(1);
#endif
    }
    r = call(S_TRY, STACK_A);
    check(r == 0, "3. TryEnter fails while another thread holds it", NULL);
    if (r == 1)
        call(S_LEAVE, STACK_A);
    b_release = 1;
    check(join_b(t), "   the other thread releases it", NULL);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
