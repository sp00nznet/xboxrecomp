/*
 * Thread ids: what XAPI's GetCurrentThreadId reads, and what
 * PsCreateSystemThreadEx hands the creator.
 *
 * GetCurrentThreadId is `mov eax, fs:[0x28]; mov eax, [eax+0x12C]`: the
 * current thread object's ETHREAD.UniqueThread. PsCreateSystemThreadEx writes
 * the new thread's id through its ThreadId argument. Neither was set, so every
 * thread's id was 0, and a title that files its threads by id (TDEOD's task
 * system) found them all in the main thread's slot and deadlocked.
 *
 * So, on the real memory layout (the synthetic XBE from tools/conformance)
 * and real worker threads: the main thread and two workers each get a
 * non-zero id, all different; the id the creator was handed is the id the
 * thread sees for itself through fs:, and it is there by the time the call
 * returns. Workers here have no TLS (TlsDataSize 0), the case where fs:[0x28]
 * used to keep pointing at the creator's object, and each worker's TIB names
 * itself at fs:[0x18]. A worker run inline (RECOMP_WORKERS=inline) still hands
 * its creator an id. With the argument "spawn" the same runs in SPAWN mode
 * (Burnout 3's), where the first call spawns too and the entry thread carries
 * on as the host-driven tick: that thread needs an id of its own.
 */
#include "kernel.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;

#define ETHREAD_UNIQUE_THREAD 0x12Cu

static uint32_t *G(uint32_t va)
{
    return (uint32_t *)((uintptr_t)va + xbox_GetMemoryOffset());
}

/* GetCurrentThreadId, as XAPI does it; 0 if there is no thread object. */
static uint32_t own_id(void)
{
    uint32_t obj = g_fs_base ? *G(g_fs_base + 0x28) : 0;
    return obj ? *G(obj + ETHREAD_UNIQUE_THREAD) : 0;
}

/* The generated title's routines, at made-up addresses. */
#define MAIN_VA    0x30000u
#define WORKER1_VA 0x30100u
#define WORKER2_VA 0x30200u
#define WORKER3_VA 0x30300u
/* [0] main routine, [1] worker 1, [2] worker 2, [3] the inline worker. */
static uint32_t s_saw[4], s_self[4], s_fs[4];
static int s_done[4];

static void ran(int i)
{
    s_saw[i] = own_id();
    s_fs[i] = g_fs_base;
    s_self[i] = g_fs_base ? *G(g_fs_base + 0x18) : 0;
    __atomic_store_n(&s_done[i], 1, __ATOMIC_RELEASE);
}
static void main_routine(void) { ran(0); }
static void worker1(void) { ran(1); }
static void worker2(void) { ran(2); }
static void worker3(void) { ran(3); }

static int done(int i) { return __atomic_load_n(&s_done[i], __ATOMIC_ACQUIRE); }
static void wait_done(int i)
{
    for (int k = 0; k < 2000 && !done(i); k++)
        Sleep(1);
}

recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va)
{
    return xbox_va == MAIN_VA ? main_routine
         : xbox_va == WORKER1_VA ? worker1
         : xbox_va == WORKER2_VA ? worker2
         : xbox_va == WORKER3_VA ? worker3 : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f)
        return NULL;
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

static uint32_t s_scratch, s_slot;

/* PsCreateSystemThreadEx(&handle, 0, 0, 0, &id, 0, 0, FALSE, FALSE, routine).
 * Returns the status; *id is what the call wrote. */
static uint32_t create(uint32_t routine, uint32_t *id)
{
    uint32_t stack = s_scratch + 0x1000, out = s_scratch + 0x2000;
    uint32_t *sp = G(stack);

    memset(sp, 0, 11 * 4);
    sp[0] = 0xBEEF0001u;      /* guest return address */
    sp[1] = out;              /* ThreadHandle out */
    sp[5] = out + 4;          /* ThreadId out (arg 4) */
    sp[10] = routine;         /* StartRoutine (arg 9) */
    G(out)[0] = 0;
    G(out)[1] = 0;
    g_esp = stack;
    g_eax = 0xDEADBEEFu;
    recomp_lookup_kernel(s_slot)();
    *id = G(out)[1];
    return g_eax;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;
    int spawn = argc > 2 && !strcmp(argv[2], "spawn");
    uint32_t main_id, id1, id2, id3, tick_id = 0, st;
    char d[160];

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    s_scratch = xbox_HeapAlloc(0x10000, 4096);
    *G(s_scratch) = 0x80000000u | 255u;      /* PsCreateSystemThreadEx */
    xbox_kernel_set_thunk_address(s_scratch, 1);
    xbox_kernel_bridge_init();
    s_slot = *G(s_scratch);
    if (!recomp_lookup_kernel(s_slot)) {
        puts("ordinal 255 did not resolve to a bridge entry point");
        return 2;
    }

    if (spawn)
        xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);

    /* INLINE: the first call runs the title's main routine on this thread.
     * SPAWN: it spawns it, and this thread goes on as the tick. */
    st = create(MAIN_VA, &main_id);
    snprintf(d, sizeof d, "status 0x%08X, id 0x%08X", st, main_id);
    check(st == 0 && main_id != 0, "the main routine gets a non-zero id", d);
    wait_done(0);
    snprintf(d, sizeof d, "it saw 0x%08X, was handed 0x%08X", s_saw[0], main_id);
    check(done(0) && s_saw[0] == main_id, "the main routine sees its own id through fs:", d);
    if (spawn) {
        tick_id = own_id();
        snprintf(d, sizeof d, "tick 0x%08X, main routine 0x%08X", tick_id, main_id);
        check(tick_id != 0 && tick_id != main_id,
              "SPAWN: the entry thread (the tick) has an id of its own", d);
    }

    /* Two workers, each on a host thread of its own. */
    st = create(WORKER1_VA, &id1);
    snprintf(d, sizeof d, "status 0x%08X, id 0x%08X", st, id1);
    check(st == 0 && id1 != 0, "worker 1 gets a non-zero id, before the call returns", d);
    st = create(WORKER2_VA, &id2);
    snprintf(d, sizeof d, "status 0x%08X, id 0x%08X", st, id2);
    check(st == 0 && id2 != 0, "worker 2 gets a non-zero id, before the call returns", d);
    snprintf(d, sizeof d, "main 0x%08X, worker 1 0x%08X, worker 2 0x%08X", main_id, id1, id2);
    check(id1 != main_id && id2 != main_id && id1 != id2 &&
          id1 != tick_id && id2 != tick_id, "the ids are all different", d);

    wait_done(1);
    wait_done(2);
    check(done(1) && done(2), "both workers ran", NULL);
    snprintf(d, sizeof d, "it saw 0x%08X, was handed 0x%08X", s_saw[1], id1);
    check(s_saw[1] == id1, "worker 1 sees its own id through fs: (no TLS)", d);
    snprintf(d, sizeof d, "it saw 0x%08X, was handed 0x%08X", s_saw[2], id2);
    check(s_saw[2] == id2, "worker 2 sees its own id through fs: (no TLS)", d);
    snprintf(d, sizeof d, "fs 0x%08X, fs:[0x18] 0x%08X; fs 0x%08X, fs:[0x18] 0x%08X",
             s_fs[1], s_self[1], s_fs[2], s_self[2]);
    check(s_fs[1] && s_self[1] == s_fs[1] && s_fs[2] && s_self[2] == s_fs[2],
          "each worker's TIB names itself at fs:[0x18]", d);

    /* RECOMP_WORKERS=inline: the routine runs on this thread, before the
     * call returns, and the creator is still handed an id for its tables. */
    recomp_env_set(RENV_WORKERS, "inline");
    st = create(WORKER3_VA, &id3);
    recomp_env_set(RENV_WORKERS, NULL);
    snprintf(d, sizeof d, "status 0x%08X, id 0x%08X, ran %d", st, id3, done(3));
    check(st == 0 && done(3) && id3 != 0 && id3 != id1 && id3 != id2 && id3 != main_id,
          "an inline worker hands its creator a new non-zero id", d);

    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
