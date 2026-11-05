/*
 * The AC'97 bus-master channel reset on a POSIX host, reached the way a
 * title reaches it: DirectSound sets RR (bit 1 of a channel's control byte,
 * NABM +0x0B) and spins until the controller clears it. The page is
 * PROT_READ under RECOMP_AC97_READY, so the store faults, the process's
 * SIGBUS/SIGSEGV handler hands it to xbox_PosixMmioFault, the store is
 * emulated and RR is cleared before the thread resumes.
 *
 * x86-64 Linux had no decoder for this before: the trap was left unarmed,
 * the store landed in plain memory, and the title spun on RR forever. A host
 * that still has none skips (exit 77) after checking it says so.
 */
#include "xbox_memory_layout.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define NABM 0xFEC00000u
#define RR   0x02u

static int failures;
static uintptr_t s_base;
static volatile sig_atomic_t s_traps, s_unhandled;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-52s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

/* What a title's own fault handler does. */
static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr)) {
        s_traps++;
        return;
    }
    s_unhandled = 1;
    signal(sig, SIG_DFL);           /* re-faults and dies: a FAIL by crash */
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf = NULL;
    long n;

    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) > 0) {
        rewind(f);
        buf = malloc((size_t)n);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        }
        *len = (size_t)n;
    }
    fclose(f);
    return buf;
}

/* DirectSound's reset: set RR, then poll it. Returns the polls it took, or
 * -1 when the bit never cleared (a spin, bounded here). */
static int reset_channel(uint32_t ctl_va, uint8_t keep)
{
    volatile uint8_t *ctl = (volatile uint8_t *)(s_base + ctl_va);
    int i;

    *ctl = (uint8_t)(keep | RR);
    for (i = 0; i < 1000; i++)
        if (!(*ctl & RR))
            return i;
    return -1;
}

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    char detail[96];
    int polls;

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = argc > 1 ? load_file(argv[1], &xbe_len) : NULL;
    if (!xbe) {
        fprintf(stderr, "usage: %s test.xbe (tools/conformance/mkxbe.py)\n",
                argv[0]);
        return 2;
    }
    /* Before the environment snapshot: the trap is armed with the rest of
     * RECOMP_AC97_READY, inside xbox_MemoryLayoutInit. */
    setenv("RECOMP_AC97_READY", "1", 1);

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_base = (uintptr_t)xbox_GetMemoryOffset();

#if !defined(__aarch64__) && !(defined(__linux__) && defined(__x86_64__))
    polls = reset_channel(NABM + 0x10B, 0);
    check(polls < 0 && s_traps == 0,
          "no decoder on this host: the reset spins, untrapped", NULL);
    printf("\n%d failure(s); trap path not available here (skipped)\n",
           failures);
    return failures ? 1 : 77;
#else
    polls = reset_channel(NABM + 0x10B, 0);
    snprintf(detail, sizeof detail, "polls %d, traps %d", polls, (int)s_traps);
    check(polls == 0 && s_traps == 1,
          "channel +0x100 reset completes on the store", detail);

    /* DDS9 resets a second channel at +0x60 next; the sweep covers it. */
    polls = reset_channel(NABM + 0x16B, 0);
    snprintf(detail, sizeof detail, "polls %d", polls);
    check(polls == 0, "channel +0x160 reset completes too", detail);

    /* RPBM (bit 0) is software's and survives the reset. */
    polls = reset_channel(NABM + 0x10B, 0x01);
    snprintf(detail, sizeof detail, "ctl 0x%02X",
             *(volatile uint8_t *)(s_base + NABM + 0x10B));
    check(polls == 0
          && *(volatile uint8_t *)(s_base + NABM + 0x10B) == 0x01,
          "RPBM is kept, only RR clears", detail);

    /* A dword store whose top byte is the control byte. */
    *(volatile uint32_t *)(s_base + NABM + 0x108) = 0x02345678u;
    snprintf(detail, sizeof detail, "read 0x%08X",
             *(volatile uint32_t *)(s_base + NABM + 0x108));
    check(*(volatile uint32_t *)(s_base + NABM + 0x108) == 0x00345678u,
          "a dword store lands and RR clears", detail);

    /* Reads stay plain: the page is readable, nothing traps. */
    {
        int before = s_traps;
        volatile uint32_t v = *(volatile uint32_t *)(s_base + NABM + 0x130);
        (void)v;
        check(s_traps == before, "a read does not trap", NULL);
    }
    check(!s_unhandled, "no access fell through to the crash path", NULL);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
#endif
}
