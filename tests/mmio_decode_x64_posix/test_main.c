/*
 * mmio_decode_x64_posix - does the x86-64 trapped-MMIO decoder service a
 * real fault on Linux: a PROT_NONE page, SIGSEGV, the ucontext's gregs, and a
 * resume past the instruction. tests/mmio_decode covers the opcode table
 * against a Windows CONTEXT without faulting; this covers what only a signal
 * can get wrong: the gregs index of each register (RAX and R10 are slots 13
 * and 2, not 0 and 10), RIP and EFLAGS written back where the kernel reloads
 * them, and the page-fault error code's write bit, which the APU trace counts
 * by. An opcode the decoder does not know must still be refused.
 *
 * Skips (exit 77) on hosts that are not x86-64 Linux.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#include "mmio_decode.h"

static int failures;
#define CHECK_U64(name, got, want) \
    do { uint64_t g_=(uint64_t)(got), w_=(uint64_t)(want); \
         if (g_ != w_) { printf("FAIL: %s (got 0x%llX, want 0x%llX)\n", name, \
                   (unsigned long long)g_, (unsigned long long)w_); failures++; } } while (0)

#define WIN 0x4000u
static uint8_t *win;                 /* PROT_NONE */
static uint8_t model[WIN];           /* the device behind it */
static int traps, unknown, last_write;
static int skip;                     /* length to step over a refused one */

static uint64_t rd(void *d, uint32_t off, int size)
{ uint64_t v = 0; memcpy(&v, (uint8_t *)d + off, size); return v; }
static void wr(void *d, uint32_t off, uint64_t v, int size)
{ memcpy((uint8_t *)d + off, &v, size); }

static void handler(int sig, siginfo_t *si, void *ucv)
{
    ucontext_t *uc = (ucontext_t *)ucv;
    uintptr_t f = (uintptr_t)si->si_addr;

    if (f >= (uintptr_t)win && f < (uintptr_t)win + WIN) {
        traps++;
        last_write = MMIO_FAULT_IS_WRITE(uc);
        if (mmio_emulate(uc, (uint32_t)(f - (uintptr_t)win), model, rd, wr))
            return;
        unknown++;
        /* Step over it anyway so the test can report instead of dying. */
        MMIO_RIP(uc) += skip;
        return;
    }
    signal(sig, SIG_DFL);
}

int main(void)
{
    struct sigaction sa;
    uint64_t x;

    win = mmap(NULL, WIN, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (win == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);

    /* MOV r/m32 both ways (89 / 8B): the store reaches the model and is
     * reported as a write; the load clears 63:32 of the destination. */
    __asm__ volatile("movl %1, (%0)" : : "r"(win + 0x40), "r"(0xDEADBEEFu) : "memory");
    CHECK_U64("movl store lands in model", *(uint32_t *)(model + 0x40), 0xDEADBEEFu);
    CHECK_U64("store has the write bit", last_write, 1);
    x = ~0ULL;
    __asm__ volatile("movl (%1), %k0" : "+r"(x) : "r"(win + 0x40) : "memory");
    CHECK_U64("movl load clears 63:32", x, 0xDEADBEEFu);
    CHECK_U64("load has no write bit", last_write, 0);

    /* REX.W, 66 and byte forms; MOVZX (0F B6/B7) */
    __asm__ volatile("movq %1, (%0)" : : "r"(win + 0x80), "r"(0x0102030405060708ull) : "memory");
    x = 0;
    __asm__ volatile("movq (%1), %0" : "=r"(x) : "r"(win + 0x80) : "memory");
    CHECK_U64("movq round trip", x, 0x0102030405060708ull);
    __asm__ volatile("movw %w1, (%0)" : : "r"(win + 0x90), "r"(0xBEEFu) : "memory");
    __asm__ volatile("movb %b1, (%0)" : : "r"(win + 0x98), "r"(0x80u) : "memory");
    CHECK_U64("movw store", *(uint16_t *)(model + 0x90), 0xBEEF);
    CHECK_U64("movb store", model[0x98], 0x80);
    x = ~0ULL;
    __asm__ volatile("movzwl (%1), %k0" : "+r"(x) : "r"(win + 0x90) : "memory");
    CHECK_U64("movzwl", x, 0xBEEF);
    x = ~0ULL;
    __asm__ volatile("movzbl (%1), %k0" : "+r"(x) : "r"(win + 0x98) : "memory");
    CHECK_U64("movzbl", x, 0x80);

    /* Immediate store (C7) and read-modify-write ALU forms (83 /0, 09) */
    __asm__ volatile("movl $0x12345678, (%0)" : : "r"(win + 0xA0) : "memory");
    CHECK_U64("movl imm", *(uint32_t *)(model + 0xA0), 0x12345678u);
    __asm__ volatile("addl $1, (%0)" : : "r"(win + 0xA0) : "memory", "cc");
    CHECK_U64("addl imm8 RMW", *(uint32_t *)(model + 0xA0), 0x12345679u);
    __asm__ volatile("orl %1, (%0)" : : "r"(win + 0xA0), "r"(0x80000000u) : "memory", "cc");
    CHECK_U64("orl reg RMW", *(uint32_t *)(model + 0xA0), 0x92345679u);

    /* Flags written back to REG_EFL: the poll loop's test and cmp. */
    {
        uint8_t z = 0xFF, nz = 0xFF, b = 0xFF;
        *(uint32_t *)(model + 0xB0) = 4;
        __asm__ volatile("testl $4, (%1)\n\tsetz %0" : "=r"(z) : "r"(win + 0xB0) : "memory", "cc");
        __asm__ volatile("testl $8, (%1)\n\tsetz %0" : "=r"(nz) : "r"(win + 0xB0) : "memory", "cc");
        __asm__ volatile("cmpl $5, (%1)\n\tsetb %0" : "=r"(b) : "r"(win + 0xB0) : "memory", "cc");
        CHECK_U64("test hit clears ZF", z, 0);
        CHECK_U64("test miss sets ZF", nz, 1);
        CHECK_U64("cmp below sets CF", b, 1);
    }

    /* Register slots that differ from the CONTEXT order: AH (byte register 4
     * without REX is bits 15:8 of RAX, gregs slot 13) and R10/R9 (slots 2
     * and 1). */
    {
        uint64_t a = 0x1111111111111111ull;
        model[0xC0] = 0x5A;
        __asm__ volatile("movb (%1), %%ah" : "+a"(a) : "r"(win + 0xC0) : "memory");
        CHECK_U64("movb to AH merges 15:8 of RAX", a, 0x1111111111115A11ull);
    }
    {
        register uintptr_t r9 __asm__("r9") = (uintptr_t)(win + 0x40);
        register uint64_t r10 __asm__("r10") = ~0ULL;
        __asm__ volatile("movl (%1), %k0" : "+r"(r10) : "r"(r9) : "memory");
        CHECK_U64("movl (r9) -> r10d", r10, 0xDEADBEEFu);
    }

    /* Refused: a string load (AD) must not be reported as handled. */
    {
        int before = unknown;
        const uint8_t *s = win + 0x40;
        skip = 1;
        __asm__ volatile("lodsl" : "+S"(s) : : "eax", "memory");
        skip = 0;
        CHECK_U64("lodsl refused", unknown - before, 1);
    }

    printf("%d traps, %d unknown, %d failures\n", traps, unknown, failures);
    return failures ? 1 : 0;
}
#else
int main(void) { printf("mmio_decode_x64_posix: not an x86-64 Linux host, skipped\n"); return 77; }
#endif
