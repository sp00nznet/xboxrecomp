/*
 * RtlTryEnterCriticalSection (ordinal 306) against Enter (277) and Leave
 * (294), through the bridge's thunk seam as a title calls them.
 *
 * TryEnter once kept a lock table of its own while Enter and Leave used
 * kernel_rtl.c's shadows. A TryEnter then Leave left the try lock held by
 * that thread for good, and excluded nobody who took the section with
 * Enter. Quantum Redshift's audio worker (sub_000DF660) services the music
 * streams only when its TryEnter of the music manager's section succeeds,
 * so once another thread had TryEntered it the music never played.
 * The cases, each on two host threads:
 *  - TryEnter then Leave on one thread: the other thread's TryEnter
 *    succeeds (and its Enter returns);
 *  - a section held through Enter: the other thread's TryEnter fails, and
 *    succeeds once the holder leaves;
 *  - a section held through TryEnter: the other thread's Enter waits until
 *    the holder leaves;
 *  - a section held through TryEnter with the other thread already waiting
 *    in Enter: a recursive TryEnter and one Leave keep it waiting, the
 *    last Leave lets it in;
 *  - recursion: TryEnter twice, Leave twice, and the section is free;
 *  - an owner that took the section with Enter: its TryEnter succeeds, and
 *    two Leaves free it;
 *  - after all of it, no thread is left holding a record
 *    (xbox_RtlCsReleaseOwned finds nothing).
 * Each case takes a section of its own (next_section), so none passes on
 * what an earlier case left held. A watchdog fails the test instead of
 * hanging it.
 */
#include "kernel.h"
#include "kernel_guest_unwind.h"
#include "recomp_env.h"
#include "xbox_memory_layout.h"   /* RECOMP_TLS */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp;
extern ptrdiff_t g_xbox_mem_offset;

#define THUNK_VA  0x10000u
#define CS_VA     0x18000u    /* the guest sections; the address is the key */
#define CS_STRIDE 0x40u
#define STACK_A   0x20000u
#define STACK_B   0x28000u

static uint8_t *s_mem;
/* The thunk entries' synthetic VAs. The dispatcher reads the slot that
 * recomp_lookup_kernel stored for the calling thread, so each call looks
 * its entry up on its own thread, as the lifted indirect call does. */
static uint32_t s_try, s_enter, s_leave;
/* The section the current case uses, for both threads. */
static volatile LONG s_cs = CS_VA;
static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static DWORD WINAPI watchdog(LPVOID unused)
{
    (void)unused;
    Sleep(30000);
    fprintf(stderr, "rtl_try_cs: watchdog: a case hung\n");
    fflush(stderr);
    _Exit(1);
    return 0;
}

/* One stdcall into a bridge entry with the section as its argument, on the
 * calling thread's own guest stack. */
static uint32_t call(uint32_t entry, uint32_t stack_va)
{
    recomp_func_t fn = recomp_lookup_kernel(entry);
    uint32_t *sp = (uint32_t *)(s_mem + stack_va);
    sp[0] = 0xBEEF0001u;
    sp[1] = (uint32_t)InterlockedCompareExchange(&s_cs, 0, 0);
    g_esp = stack_va;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}

static int try_a(void) { return call(s_try, STACK_A) & 0xFF; }
static void enter_a(void) { call(s_enter, STACK_A); }
static void leave_a(void) { call(s_leave, STACK_A); }

/* Thread B: one operation per request, so A decides the order. */
enum { OP_NONE, OP_TRY, OP_TRY_LEAVE, OP_ENTER_LEAVE, OP_QUIT };
static volatile LONG s_op, s_done, s_result;

static DWORD WINAPI thread_b(LPVOID unused)
{
    (void)unused;
    for (;;) {
        LONG op = InterlockedExchange(&s_op, OP_NONE);
        if (op == OP_NONE) { Sleep(1); continue; }
        if (op == OP_QUIT) break;
        if (op == OP_TRY) {
            InterlockedExchange(&s_result, (LONG)(call(s_try, STACK_B) & 0xFF));
        } else if (op == OP_TRY_LEAVE) {
            LONG r = (LONG)(call(s_try, STACK_B) & 0xFF);
            if (r)
                call(s_leave, STACK_B);
            InterlockedExchange(&s_result, r);
        } else if (op == OP_ENTER_LEAVE) {
            call(s_enter, STACK_B);
            call(s_leave, STACK_B);
            InterlockedExchange(&s_result, 1);
        }
        InterlockedIncrement(&s_done);
    }
    InterlockedExchange(&s_result, xbox_RtlCsReleaseOwned());
    InterlockedIncrement(&s_done);
    return 0;
}

/* Ask B for op; wait up to ms for it to finish. 1 when it did. */
static int ask_b(LONG op, DWORD ms)
{
    LONG before = InterlockedCompareExchange(&s_done, 0, 0);
    DWORD t;

    InterlockedExchange(&s_result, -1);
    InterlockedExchange(&s_op, op);
    for (t = 0; t < ms; t++) {
        if (InterlockedCompareExchange(&s_done, 0, 0) != before)
            return 1;
        Sleep(1);
    }
    return 0;
}

/* A fresh, never-touched section for the next case. */
static void next_section(void)
{
    InterlockedExchangeAdd(&s_cs, CS_STRIDE);
}

/* Start op on B without waiting; the s_done count to pass to wait_b. */
static LONG start_b(LONG op)
{
    LONG before = InterlockedCompareExchange(&s_done, 0, 0);
    InterlockedExchange(&s_result, -1);
    InterlockedExchange(&s_op, op);
    return before;
}

static int wait_b(DWORD ms, LONG before)
{
    DWORD t;
    for (t = 0; t < ms; t++) {
        if (InterlockedCompareExchange(&s_done, 0, 0) != before)
            return 1;
        Sleep(1);
    }
    return 0;
}

int main(void)
{
    HANDLE b;
    LONG before;

    setvbuf(stdout, NULL, _IONBF, 0);
    CreateThread(NULL, 0, watchdog, NULL, 0, NULL);

    s_mem = VirtualAlloc(NULL, 16 * 1024 * 1024, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
    if (!s_mem) {
        printf("  guest memory stand-in: FAIL\n");
        return 1;
    }
    g_xbox_mem_offset = (ptrdiff_t)s_mem;
    *(uint32_t *)(s_mem + THUNK_VA)     = 0x80000000u | 306u;
    *(uint32_t *)(s_mem + THUNK_VA + 4) = 0x80000000u | 277u;
    *(uint32_t *)(s_mem + THUNK_VA + 8) = 0x80000000u | 294u;
    xbox_kernel_set_thunk_address(THUNK_VA, 3);
    xbox_kernel_bridge_init();
    s_try   = *(uint32_t *)(s_mem + THUNK_VA);
    s_enter = *(uint32_t *)(s_mem + THUNK_VA + 4);
    s_leave = *(uint32_t *)(s_mem + THUNK_VA + 8);
    if (!recomp_lookup_kernel(s_try) || !recomp_lookup_kernel(s_enter)
            || !recomp_lookup_kernel(s_leave)) {
        check(0, "ordinals 306, 277 and 294 resolve");
        return 1;
    }
    check(1, "ordinals 306, 277 and 294 resolve");
    b = CreateThread(NULL, 0, thread_b, NULL, 0, NULL);

    /* TryEnter then Leave frees the section for the other thread. */
    check(try_a() == 1, "TryEnter of a free section succeeds");
    leave_a();
    check(ask_b(OP_TRY_LEAVE, 2000) && s_result == 1,
          "after TryEnter and Leave, another thread's TryEnter succeeds");
    check(ask_b(OP_ENTER_LEAVE, 2000) && s_result == 1,
          "after TryEnter and Leave, another thread's Enter returns");

    /* Held through Enter: TryEnter on the other thread fails, then not. */
    next_section();
    enter_a();
    check(ask_b(OP_TRY, 2000) && s_result == 0,
          "a section held through Enter refuses another thread's TryEnter");
    leave_a();
    check(ask_b(OP_TRY_LEAVE, 2000) && s_result == 1,
          "once its holder leaves, the other thread's TryEnter succeeds");

    /* Held through TryEnter: Enter on the other thread waits for it. */
    next_section();
    check(try_a() == 1, "TryEnter takes a fresh section");
    before = start_b(OP_ENTER_LEAVE);
    check(!wait_b(200, before),
          "a section held through TryEnter keeps another thread's Enter waiting");
    leave_a();
    check(wait_b(2000, before) && s_result == 1,
          "the waiting Enter returns once the holder leaves");

    /* The other thread already waits in Enter while the holder, who took
     * the section through TryEnter, recurses through TryEnter. */
    next_section();
    check(try_a() == 1, "TryEnter takes another fresh section");
    before = start_b(OP_ENTER_LEAVE);
    check(!wait_b(200, before), "another thread's Enter waits on the TryEnter holder");
    check(try_a() == 1, "the holder's TryEnter succeeds while the other thread waits");
    leave_a();
    check(!wait_b(200, before), "one Leave of the two keeps the other thread waiting");
    leave_a();
    check(wait_b(2000, before) && s_result == 1,
          "the waiting Enter returns after the holder's last Leave");

    /* Recursion through TryEnter. */
    next_section();
    check(try_a() == 1 && try_a() == 1, "TryEnter is recursive on its owner");
    leave_a();
    check(ask_b(OP_TRY, 2000) && s_result == 0,
          "one Leave of two recursions keeps the section held");
    leave_a();
    check(ask_b(OP_TRY_LEAVE, 2000) && s_result == 1,
          "the second Leave frees it");

    /* An owner that took the section through Enter TryEnters it again. */
    next_section();
    enter_a();
    check(try_a() == 1, "TryEnter by the owner of an Enter succeeds");
    leave_a();
    check(ask_b(OP_TRY, 2000) && s_result == 0,
          "one Leave of Enter plus TryEnter keeps the section held");
    leave_a();
    check(ask_b(OP_TRY_LEAVE, 2000) && s_result == 1,
          "the second Leave frees it for the other thread's TryEnter");

    check(xbox_RtlCsReleaseOwned() == 0, "this thread holds no section records");
    check(ask_b(OP_QUIT, 2000) && s_result == 0,
          "the other thread holds no section records");
    WaitForSingleObject(b, 2000);

    printf("\n%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
