/*
 * The USB OHCI register model on a POSIX host, reached the way a title
 * reaches it: a load or store to the controller's aperture faults, the
 * process's SIGBUS/SIGSEGV handler hands the fault to xbox_OhciHandleMmio,
 * and the instruction completes with the model's value.
 *
 * Before this was ported the trap existed on Windows only: on POSIX the
 * aperture stayed plain zeroed memory and xbox_OhciHandleMmio returned 0, so
 * a title's USB driver found no controller and no pad.
 *
 * A host without a trapped-MMIO decoder (anything but arm64 here) keeps the
 * model off by design; the test checks that it says so and exits 77.
 */
#include "xbox_memory_layout.h"
#include "ohci.h"

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int failures;
static uintptr_t s_base;
static volatile sig_atomic_t s_unhandled;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-52s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

/* What a title's own fault handler does: hand the fault to the toolkit's
 * trapped-MMIO dispatch, which routes it to the OHCI model. */
static void on_fault(int sig, siginfo_t *si, void *ucv)
{
    if (xbox_PosixMmioFault(ucv, (uintptr_t)si->si_addr))
        return;
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

int main(int argc, char **argv)
{
    size_t xbe_len = 0;
    unsigned char *xbe;
    struct sigaction sa;
    char detail[96];

    setvbuf(stdout, NULL, _IONBF, 0);
    xbe = argc > 1 ? load_file(argv[1], &xbe_len) : NULL;
    if (!xbe) {
        fprintf(stderr, "usage: %s test.xbe (tools/conformance/mkxbe.py)\n",
                argv[0]);
        return 2;
    }
    /* Before anything reads the environment: recomp_env takes its snapshot
     * on the first read, and xbox_MemoryLayoutInit is one. */
    setenv("RECOMP_DEBUG", "ohci", 1);
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        fprintf(stderr, "xbox_MemoryLayoutInit failed\n");
        return 2;
    }
    s_base = (uintptr_t)xbox_GetMemoryOffset();

    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGSEGV, &sa, NULL);

    xbox_OhciInit();

#if !defined(__aarch64__)
    check(!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE),
          "no decoder on this host: the model stays off", NULL);
    printf("\n%d failure(s); trap path not available here (skipped)\n",
           failures);
    return failures ? 1 : 77;
#else
    {
        volatile uint32_t *hc0 = (volatile uint32_t *)(s_base + XBOX_OHCI0_BASE);
        volatile uint32_t *hc1 = (volatile uint32_t *)(s_base + XBOX_OHCI1_BASE);
        uint32_t v;
        uint64_t q;

        check(xbox_OhciOwnsAddress(XBOX_OHCI0_BASE)
              && xbox_OhciOwnsAddress(XBOX_OHCI1_BASE + 0x54),
              "both controllers claim their apertures", NULL);
        check(!xbox_OhciOwnsAddress(XBOX_OHCI0_BASE + XBOX_OHCI_SIZE),
              "the byte past a controller is not claimed", NULL);

        v = hc0[0];                               /* HcRevision */
        snprintf(detail, sizeof detail, "read 0x%08X", v);
        check(v == 0x10u, "HC0 HcRevision reads OHCI 1.0 through the trap",
              detail);
        v = hc1[0];
        snprintf(detail, sizeof detail, "read 0x%08X", v);
        check(v == 0x10u, "HC1 HcRevision reads OHCI 1.0", detail);

        v = hc0[0x48 / 4] & 0xFFu;                /* HcRhDescriptorA.NDP */
        snprintf(detail, sizeof detail, "NDP %u", v);
        check(v >= 1 && v <= 4, "root hub reports its downstream ports",
              detail);

        v = *(volatile uint8_t *)(s_base + XBOX_OHCI0_BASE);
        snprintf(detail, sizeof detail, "read 0x%02X", v);
        check(v == 0x10u, "a byte read takes its slice of the register",
              detail);

        hc0[0x10 / 4] = 0x00000004u;              /* HcInterruptEnable: SF */
        v = hc0[0x10 / 4];
        snprintf(detail, sizeof detail, "enable mask 0x%08X", v);
        check((v & 0x4u) != 0, "a store reaches the model (enable SF)",
              detail);
        hc0[0x14 / 4] = 0x00000004u;              /* HcInterruptDisable */
        v = hc0[0x10 / 4];
        snprintf(detail, sizeof detail, "enable mask 0x%08X", v);
        check((v & 0x4u) == 0, "HcInterruptDisable clears it again", detail);

        q = *(volatile uint64_t *)(s_base + XBOX_OHCI0_BASE);
        snprintf(detail, sizeof detail, "read 0x%016llX",
                 (unsigned long long)q);
        check((uint32_t)q == 0x10u,
              "an 8-byte read is two register reads", detail);
    }
    check(!s_unhandled, "no access fell through to the crash path", NULL);
    xbox_OhciReport();
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
#endif
}
