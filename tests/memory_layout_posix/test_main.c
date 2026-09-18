/*
 * The Xbox memory model on a POSIX host -- written to fail.
 *
 * These are the properties a port has to satisfy, asserted before the port
 * exists so that "done" is defined by something other than opinion. On
 * arm64 macOS today every one of them fails, and each failure names a
 * distinct problem rather than one problem seen four times:
 *
 *   1. host page size    the code hardcodes 4096 in places; Apple Silicon
 *                        uses 16384, so a VirtualProtect over "one page"
 *                        silently covers four times what it means to
 *   2. base view         __PAGEZERO spans 0..4GB, so every candidate base in
 *                        try_bases[] is unmappable, and the "let the OS
 *                        choose" sentinel is never reached because the loop
 *                        condition terminates on it
 *   3. mirrors           28 views at 64MB intervals must alias the same
 *                        physical pages: the 26-bit address bus means
 *                        0x04070000 reads what 0x00070000 holds
 *   4. teardown          shutdown must release every view, or a second init
 *                        in one process fails on addresses it already owns
 *
 * Deliberately not asserted: the tiled aperture at 0xF0000000. Whether that
 * specific architectural alias survives relocation to a high host base is a
 * question about what guest code assumes, and guessing at it here would bake
 * in an answer nobody has established.
 */
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/wait.h>


/* Supplied by recompiled game code in a real build. The memory model links
 * against them but never calls them here, and a standalone regression has no
 * recompiled title to provide them. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

static int failures = 0;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-44s %s%s%s\n", what, ok ? "PASS" : "FAIL",
           detail && !ok ? " -- " : "", detail && !ok ? detail : "");
    if (!ok) failures++;
}

/* Read one byte in a forked child.
 *
 * The obvious approaches both failed here. Catching SIGSEGV and recovering
 * with siglongjmp works until two unmapped mirrors fall next to each other,
 * at which point the recovery itself dies. Asking mach_vm_region first reports
 * some of these addresses as mapped when reading them still faults. A child
 * process cannot be wrong about it: if the read faults, the child dies and the
 * parent sees the signal, and the 27 other mirrors still get checked.
 *
 * Returns 1 and sets *out when the read succeeds, 0 when it faults.
 */
static int read_byte_in_child(const unsigned char *addr, unsigned char *out)
{
    int fd[2];
    if (pipe(fd) != 0) return 0;

    pid_t pid = fork();
    if (pid < 0) { close(fd[0]); close(fd[1]); return 0; }

    if (pid == 0) {
        close(fd[0]);
        unsigned char v = *addr;          /* faults here, or does not */
        ssize_t n = write(fd[1], &v, 1);
        _exit(n == 1 ? 0 : 1);
    }

    close(fd[1]);
    unsigned char v = 0;
    ssize_t got = read(fd[0], &v, 1);
    close(fd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (got == 1 && WIFEXITED(status) && WEXITSTATUS(status) == 0) {
        *out = v;
        return 1;
    }
    return 0;
}

/* The model parses section layout from an XBE header; the synthetic one from
 * tools/conformance/mkxbe.py is enough to get through init.
 *
 * Reads the whole file, however big. A fixed buffer is what this used to do,
 * and a real title overran it: default.xbe is 4MB, the buffer was 1MB, and
 * fread stopped at the cap without saying so. Every section's raw data lives
 * past the first megabyte, so the loader copied nothing while still reporting
 * "Loaded 17/17 sections", and the kernel thunk table -- which sits at the
 * start of .rdata, 3.5MB in -- read back as zeroes. The truncation was
 * indistinguishable from a title that imports no kernel functions. */
static unsigned char *load_xbe(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;

    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long len = ftell(f);
    if (len <= 0) { fclose(f); return NULL; }
    rewind(f);

    unsigned char *buf = malloc((size_t)len);
    if (!buf) { fclose(f); return NULL; }

    size_t n = fread(buf, 1, (size_t)len, f);
    fclose(f);

    /* A short read means the bytes the loader is about to be judged on are
     * not the bytes on disk. Refuse rather than test a truncated image. */
    if (n != (size_t)len) { free(buf); return NULL; }

    *out_len = n;
    return buf;
}

int main(int argc, char **argv)
{
    /* Unbuffered: these checks poke at mappings that may fault, and buffered
     * output is discarded when they do -- leaving a crash with no indication
     * of which check reached it. */
    setvbuf(stdout, NULL, _IONBF, 0);

    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t n = 0;
    unsigned char *xbe = load_xbe(path, &n);
    if (!xbe) {
        fprintf(stderr, "cannot read %s -- run: python3 tools/conformance/mkxbe.py\n",
                path);
        return 2;
    }
    printf("XBE %s: %zu bytes\n", path, n);

    long host_page = sysconf(_SC_PAGESIZE);
    printf("host page size %ld, %d mirrors expected\n\n",
           host_page, XBOX_NUM_MIRRORS);

    /* 1. The model must come up at all. */
    BOOL ok = xbox_MemoryLayoutInit(xbe, n);
    check(ok, "xbox_MemoryLayoutInit succeeds",
          "base view unmappable: every try_bases[] entry is inside __PAGEZERO");
    if (!ok) {
        printf("\n%d failure(s); later checks need a live mapping.\n", failures);
        free(xbe);
        return 1;
    }

    void  *base = xbox_GetMemoryBase();
    size_t size = xbox_GetMappedSize();
    check(base != NULL, "base pointer is non-NULL", NULL);
    check(size > 0, "mapped size is non-zero", NULL);

    /* 2. Host page granularity, not the guest's 4 KB. */
    check(((uintptr_t)base % (uintptr_t)host_page) == 0,
          "base is aligned to the HOST page size",
          "aligned to 4096 but not to the host's larger page");
    check((size % (size_t)host_page) == 0,
          "mapped size is a whole number of host pages", NULL);

    /* 3. The wrap. Write through the base, read through each mirror: the
     *    26-bit bus means every mirror is the same physical memory. */
    unsigned char *p = (unsigned char *)base;
    const size_t probe = 0x70000;
    fprintf(stderr, "[base=%p size=0x%zx probe=0x%zx addr=%p]\n",
            base, size, probe, (void *)(p + probe));          /* the address the comment cites */
    if (probe < size) {
        p[probe] = 0x5A;
        int aliased = 0, checked = 0, unmapped = 0;
        for (int m = 1; m <= XBOX_NUM_MIRRORS; m++) {
            unsigned char *mp = p + (size_t)m * size + probe;
            checked++;
            /* Ask before touching. A mirror the layout could not place is not
             * mapped at all, and dereferencing it would abort this test with a
             * signal instead of reporting which mirrors are missing. */
            unsigned char got = 0;
            if (!read_byte_in_child(mp, &got)) { unmapped++; continue; }
            if (got == 0x5A) aliased++;
        }
        char detail[128];
        snprintf(detail, sizeof detail,
                 "%d of %d aliased, %d never mapped", aliased, checked, unmapped);
        check(aliased == checked, "every mirror aliases the base page", detail);

        /* And the other direction: a write through a mirror is visible at the
         * base, which is what a title's out-of-range write actually does. */
        unsigned char *m1 = p + size + probe;
        unsigned char ignored = 0;
        if (read_byte_in_child(m1, &ignored)) {
            *m1 = 0xA5;
            check(p[probe] == 0xA5, "a write through mirror 1 reaches the base",
                  "mirror is a separate copy, not an alias");
        } else {
            check(0, "a write through mirror 1 reaches the base",
                  "mirror 1 is not accessible");
        }
    } else {
        check(0, "mapped region covers the 0x70000 probe", "region too small");
    }

    /* 4. Teardown has to give the addresses back, or nothing can re-init. */
    xbox_MemoryLayoutShutdown();
    BOOL again = xbox_MemoryLayoutInit(xbe, n);
    check(again, "a second init after shutdown succeeds",
          "shutdown leaked views, so the addresses are still taken");

    /* Returning TRUE is not the property worth asserting. The apertures are
     * best-effort inside init, so a run that leaks them on shutdown still
     * reports success on the second init while having no contiguous window and
     * no device apertures at all -- initialised in name only. Check the memory
     * is really there. */
    if (again) {
        void *base2 = xbox_GetMemoryBase();
        check(base2 != NULL, "the second init produced a base", NULL);
        if (base2) {
            const struct { uint32_t va; const char *what; } apertures[] = {
                { 0x80000000u, "contiguous window at 0x80000000" },
                { 0xFD000000u, "NV2A aperture at 0xFD000000" },
                { 0xFE800000u, "MCPX aperture at 0xFE800000" },
                { 0xFF000000u, "flash aperture at 0xFF000000" },
                { 0xF0000000u, "tiled aperture at 0xF0000000" },
            };
            for (size_t i = 0; i < sizeof apertures / sizeof apertures[0]; i++) {
                /* host = guest + base: XBOX_MAP_START is 0, so the base
                 * pointer corresponds to guest address 0, not to
                 * XBOX_BASE_ADDRESS. Subtracting the latter probes 64 KB below
                 * each aperture, which is unmapped, and reads as a failure of
                 * the thing being tested rather than of the arithmetic. */
                unsigned char *p2 = (unsigned char *)base2 + (size_t)apertures[i].va;
                unsigned char got = 0;
                char what[96];
                snprintf(what, sizeof what, "re-init mapped the %s",
                         apertures[i].what);
                check(read_byte_in_child(p2, &got), what,
                      "not mapped after shutdown + init");
            }
        }
        xbox_MemoryLayoutShutdown();
    }

    /*
     * 5. RECOMP_TRAP_NULL has to actually trap.
     *
     * The guard is opt-in, and on a 16 KB-page host it currently opts itself
     * back out: protecting guest page zero is done at host page granularity,
     * so the 4 KB request covers guest 0..0x3FFF and takes XBOX_TIB_MAIN at
     * 0x1000 with it. Init writes the TIB moments later and the run dies, so
     * the code declines to install the guard at all.
     *
     * Declining is not the same as working. On every Apple Silicon host the
     * diagnostic silently does nothing: a guest null dereference reads zero
     * and the bug surfaces somewhere else entirely, which is the failure the
     * guard exists to catch. Both halves are asserted here, because a fix
     * that traps page zero by clobbering the TIB is not a fix.
     */
    {
        setenv("RECOMP_TRAP_NULL", "1", 1);
        BOOL trapped = xbox_MemoryLayoutInit(xbe, n);
        check(trapped, "init succeeds with RECOMP_TRAP_NULL set",
              "the guard must not cost the run");

        if (trapped) {
            unsigned char *b = (unsigned char *)xbox_GetMemoryBase();
            unsigned char got = 0;

            /* Guest address 0 must fault. Today it reads back as zero. */
            check(!read_byte_in_child(b + 0, &got),
                  "RECOMP_TRAP_NULL faults on a read of guest address 0",
                  "page zero is readable, so the guard never installed");

            /* ...and the TIB must survive it. The first dword of the TIB is
             * the SEH chain terminator, 0xFFFFFFFF, so its low byte is 0xFF;
             * a zero here means the protect reached past page zero and the
             * TIB was never written. */
            unsigned char tib = 0;
            int tib_ok = read_byte_in_child(b + XBOX_FS_BASE, &tib);
            check(tib_ok && tib == 0xFF,
                  "the TIB is intact with RECOMP_TRAP_NULL set",
                  tib_ok ? "TIB reads zero -- the trap clobbered it"
                         : "TIB is unmapped -- the trap covered it");

            xbox_MemoryLayoutShutdown();
        }
        unsetenv("RECOMP_TRAP_NULL");
    }

    printf("\n%d failure(s)\n", failures);
    free(xbe);
    return failures ? 1 : 0;
}
