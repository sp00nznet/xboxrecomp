/*
 * Kernel thread objects, memory and file bridges, through the path a title
 * takes.
 *
 *   Thread objects            a thread handle leads to a guest thread object
 *                             whose SignalState and ExitStatus follow the host
 *                             thread, also through a duplicate of the handle.
 *   NtFreeVirtualMemory (199) reads its arguments as guest pointers; it used
 *                             to call the host VirtualFree on a guest VA and
 *                             fail every call.
 *   NtSetInformationFile (226) FileRenameInformation: a title that writes
 *                             x.tmp and renames it to x.dat must find x.dat.
 *   FATX overwrite            an overwrite followed by a smaller end-of-file
 *                             keeps the bytes below the new end.
 *
 * Each bridge is driven by ordinal through recomp_lookup_kernel, as in
 * tests/kernel_bridge, but against a real guest window from
 * xbox_MemoryLayoutInit.
 */
#include "kernel.h"
#include "xbox_memory_layout.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

typedef void (*recomp_func_t)(void);
static void worker_returns(void);
static void worker_terminates(void);
/* Two guest "start routines" for the thread-object checks. */
recomp_func_t recomp_lookup(uint32_t va)
{
    return va == 0x00010000u ? worker_returns
         : va == 0x00020000u ? worker_terminates : NULL;
}
recomp_func_t recomp_lookup_manual(uint32_t va) { (void)va; return NULL; }

extern recomp_func_t recomp_lookup_kernel(uint32_t xbox_va);
extern RECOMP_TLS uint32_t g_eax, g_esp, g_ecx;
extern ptrdiff_t g_xbox_mem_offset;

static int failures;

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-62s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

#define G32(va) (*(volatile uint32_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G16(va) (*(volatile uint16_t *)((uintptr_t)(va) + g_xbox_mem_offset))
#define G8(va)  (*(volatile uint8_t  *)((uintptr_t)(va) + g_xbox_mem_offset))
#define GP(va)  ((uint8_t *)((uintptr_t)(va) + g_xbox_mem_offset))

enum { ORD_MMALLOC = 165, ORD_MMALLOCEX = 166, ORD_MMFREE = 171,
       ORD_MMQUERYSIZE = 180, ORD_NTALLOC = 184, ORD_NTCLOSE = 187,
       ORD_NTCREATE = 190, ORD_NTFREE = 199, ORD_NTSETINFO = 226,
       ORD_KEWAIT = 159, ORD_NTDUP = 197, ORD_NTWAIT = 233, ORD_OBREF = 246,
       ORD_OBDEREF = 250, ORD_PSCREATE = 255, ORD_PSTERM = 258,
       ORD_NTWRITE = 236, ORD_NTQUERYINFO = 211 };
static const uint32_t ORDS[] = { ORD_MMALLOC, ORD_MMALLOCEX, ORD_MMFREE,
    ORD_MMQUERYSIZE, ORD_NTALLOC, ORD_NTCLOSE, ORD_NTCREATE, ORD_NTFREE,
    ORD_NTSETINFO, ORD_KEWAIT, ORD_NTDUP, ORD_NTWAIT, ORD_OBREF, ORD_OBDEREF,
    ORD_PSCREATE, ORD_PSTERM, ORD_NTWRITE, ORD_NTQUERYINFO };
#define NORDS (sizeof ORDS / sizeof ORDS[0])

static uint32_t g_thunks, g_stack, g_scratch;
static volatile int g_worker_go;

/* stdcall: return address, then the arguments. */
static uint32_t call(uint32_t ordinal, int argc, const uint32_t *argv)
{
    unsigned i;
    recomp_func_t fn = NULL;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            fn = recomp_lookup_kernel(G32(g_thunks + 4 * i));
    if (!fn) {
        printf("  ordinal %u did not resolve\n", ordinal);
        failures++;
        return 0xFFFFFFFFu;
    }
    G32(g_stack) = 0xBEEF0001u;
    for (i = 0; i < (unsigned)argc; i++)
        G32(g_stack + 4 + 4 * i) = argv[i];
    g_esp = g_stack;
    g_eax = 0xDEADBEEFu;
    fn();
    return g_eax;
}
#define CALL(ord, ...) call((ord), \
    (int)(sizeof((uint32_t[]){__VA_ARGS__}) / 4), (uint32_t[]){__VA_ARGS__})

static recomp_func_t lookup_ord(uint32_t ordinal)
{
    unsigned i;

    for (i = 0; i < NORDS; i++)
        if (ORDS[i] == ordinal)
            return recomp_lookup_kernel(G32(g_thunks + 4 * i));
    return NULL;
}

/* Runs on the worker's own thread and stack (g_esp is thread-local). */
static void worker_returns(void)
{
    g_eax = 7;
}

static void worker_terminates(void)
{
    recomp_func_t term = lookup_ord(ORD_PSTERM);

    while (!g_worker_go)
        usleep(1000);
    g_esp -= 8;
    G32(g_esp) = 0;              /* return address */
    G32(g_esp + 4) = 0x1234u;    /* ExitStatus */
    term();                      /* does not return */
}

static unsigned char *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *b;
    long len;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    rewind(f);
    b = malloc((size_t)len);
    if (b && fread(b, 1, (size_t)len, f) != (size_t)len) { free(b); b = NULL; }
    fclose(f);
    *n = (size_t)len;
    return b;
}

/* ---- NtAllocateVirtualMemory / NtFreeVirtualMemory -------------------- */
static void test_virtual_memory(void)
{
    char d[160];
    uint32_t pbase = g_scratch, psize = g_scratch + 4, base, next, st, i;
    int intact = 1, zeroed = 1;

    printf("NtFreeVirtualMemory (%s)\n", xbox_HeapReclaimEnabled()
           ? "RECOMP_HEAP_RECLAIM" : "default");
    G32(pbase) = 0;
    G32(psize) = 0x10000;
    st = CALL(ORD_NTALLOC, pbase, 0, psize, 0x3000 /* RESERVE|COMMIT */, 4);
    base = G32(pbase);
    check(st == 0 && base, "allocate 64 KB", NULL);
    memset(GP(base), 0xAB, 0x10000);

    /* Decommit one page in the middle, unaligned base, as RtlHeap does. */
    G32(pbase) = base + 0x1010;
    G32(psize) = 0x10;
    st = CALL(ORD_NTFREE, pbase, psize, 0x4000 /* MEM_DECOMMIT */);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "MEM_DECOMMIT succeeds (guest pointers read as guest)", d);
    snprintf(d, sizeof d, "base 0x%08X size 0x%X", G32(pbase), G32(psize));
    check(G32(pbase) == base + 0x1000 && G32(psize) == 0x1000,
          "rounded base and size written back to the guest", d);
    for (i = 0; i < 0x1000; i++) zeroed &= GP(base + 0x1000)[i] == 0;
    intact = GP(base + 0xFFF)[0] == 0xAB && GP(base + 0x2000)[0] == 0xAB;
    check(zeroed, "decommitted page reads back as zero", NULL);
    check(intact, "neighbouring pages untouched", NULL);

    /* A decommit running past the block stops at its end. */
    G32(pbase) = 0;
    G32(psize) = 0x10000;
    st = CALL(ORD_NTALLOC, pbase, 0, psize, 0x3000, 4);
    next = G32(pbase);
    check(st == 0 && next, "allocate a second 64 KB", NULL);
    memset(GP(next), 0xCD, 0x10000);
    G32(pbase) = base + 0xF000;
    G32(psize) = 0x3000;
    st = CALL(ORD_NTFREE, pbase, psize, 0x4000);
    snprintf(d, sizeof d, "status 0x%08X base 0x%08X size 0x%X", st,
             G32(pbase), G32(psize));
    check(st == 0 && G32(pbase) == base + 0xF000 && G32(psize) == 0x1000,
          "decommit past the block is clamped to it", d);
    check(GP(next)[0] == 0xCD && GP(base + 0x10000 - 1)[0] == 0,
          "the next block is untouched", NULL);

    /* Release only from the start of the block. */
    G32(pbase) = base + 0x2000;
    G32(psize) = 0;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0xC000009Fu, "MEM_RELEASE inside a block: FREE_VM_NOT_AT_BASE", d);

    /* Release: size must be 0; the whole block goes back to the heap. The
     * RECOMP_HEAP_RECLAIM path (upstream's) takes a nonzero size as 0. */
    G32(pbase) = base;
    G32(psize) = 0x1000;
    if (!xbox_HeapReclaimEnabled()) {
        st = CALL(ORD_NTFREE, pbase, psize, 0x8000);
        check(st != 0, "MEM_RELEASE with a nonzero size is refused", NULL);
    }
    G32(psize) = 0;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000 /* MEM_RELEASE */);
    snprintf(d, sizeof d, "status 0x%08X size 0x%X", st, G32(psize));
    check(st == 0 && G32(psize) == 0x10000, "MEM_RELEASE frees the region", d);
    G32(pbase) = 0;
    G32(psize) = 0x10000;
    CALL(ORD_NTALLOC, pbase, 0, psize, 0x3000, 4);
    snprintf(d, sizeof d, "got 0x%08X, released 0x%08X", G32(pbase), base);
    check(G32(pbase) == base, "released region is reused", d);

    G32(pbase) = 0x00001000;
    G32(psize) = 0;
    st = CALL(ORD_NTFREE, pbase, psize, 0x8000);
    check(st == 0xC00000A0u, "free below the heap is refused, not zeroed", NULL);
}

/* ---- contiguous arena -------------------------------------------------- */
static void test_contiguous(void)
{
    char d[160];
    uint32_t a, b, c, e, big, i, ok = 1;

    printf("contiguous arena\n");
    a = CALL(ORD_MMALLOC, 0x2000);
    b = CALL(ORD_MMALLOC, 0x1000);
    snprintf(d, sizeof d, "a=0x%08X b=0x%08X", a, b);
    check(a >= XBOX_CONTIG_BASE && b >= XBOX_CONTIG_BASE && a != b,
          "allocations come from the window", d);
    snprintf(d, sizeof d, "got %u", CALL(ORD_MMQUERYSIZE, a));
    check(CALL(ORD_MMQUERYSIZE, a) == 0x2000,
          "MmQueryAllocationSize answers for a contiguous block", d);
    check(CALL(ORD_MMQUERYSIZE, a + 0x1800) == 0x800,
          "MmQueryAllocationSize on an interior address", NULL);
    CALL(ORD_MMFREE, a);
    check(CALL(ORD_MMQUERYSIZE, a) == 0, "freed block reports size 0", NULL);
    c = CALL(ORD_MMALLOC, 0x1000);
    snprintf(d, sizeof d, "c=0x%08X a=0x%08X", c, a);
    check(c == a, "freed pages are reused", d);

    /* MmAllocateContiguousMemoryEx: range and alignment honoured. */
    e = CALL(ORD_MMALLOCEX, 0x3000, 0x01000000, 0x01FFFFFF, 0x10000, 4);
    snprintf(d, sizeof d, "e=0x%08X", e);
    check(e && (e - XBOX_CONTIG_BASE) >= 0x01000000
            && (e - XBOX_CONTIG_BASE) + 0x3000 - 1 <= 0x01FFFFFF
            && (e & 0xFFFF) == 0,
          "Ex honours lowest/highest and alignment", d);
    check(CALL(ORD_MMALLOCEX, 0x1000, 0x5000000, 0x4000000, 0, 4) == 0,
          "Ex with highest < lowest fails", NULL);
    CALL(ORD_MMFREE, e);
    CALL(ORD_MMFREE, b);
    CALL(ORD_MMFREE, c);

    /* Churn: 40 MB allocated and freed eight times would exhaust a bump
     * allocator over a 64 MB window on the second pass. */
    for (i = 0; i < 8 && ok; i++) {
        big = CALL(ORD_MMALLOC, 40u << 20);
        ok = big != 0;
        if (ok) CALL(ORD_MMFREE, big);
    }
    check(ok, "allocate/free 40 MB eight times without exhausting", NULL);
    snprintf(d, sizeof d, "%u bytes still in use",
             xbox_ContiguousInUseBytes());
    check(xbox_ContiguousInUseBytes() == 0, "everything freed is released", d);
}

/* ---- FileRenameInformation -------------------------------------------- */
static uint32_t put_str(uint32_t at, const char *s)
{
    /* ANSI_STRING at `at`, text right after it. */
    uint16_t n = (uint16_t)strlen(s);
    memcpy(GP(at + 8), s, n + 1);
    G16(at) = n;
    G16(at + 2) = (uint16_t)(n + 1);
    G32(at + 4) = at + 8;
    return at;
}

static uint32_t create(const char *path)
{
    uint32_t oa = g_scratch + 0x100, str = g_scratch + 0x200;
    uint32_t ph = g_scratch + 0x10, ios = g_scratch + 0x18, st;

    put_str(str, path);
    G32(oa) = 0; G32(oa + 4) = str; G32(oa + 8) = 0x40;
    st = CALL(ORD_NTCREATE, ph, 0xC0110000u /* R|W|DELETE|SYNC */, oa, ios, 0,
              0x80, 0, 5 /* OVERWRITE_IF */, 0x60);
    return st ? 0 : G32(ph);
}

static uint32_t rename_to(uint32_t h, const char *name, int replace)
{
    uint32_t info = g_scratch + 0x400, ios = g_scratch + 0x18;
    uint16_t n = (uint16_t)strlen(name);

    memset(GP(info), 0, 16);
    G8(info) = (uint8_t)replace;
    G32(info + 4) = 0;
    G16(info + 8) = n;
    G16(info + 10) = (uint16_t)(n + 1);
    G32(info + 12) = info + 16;
    memcpy(GP(info + 16), name, n + 1);
    return CALL(ORD_NTSETINFO, h, ios, info, 16, 10);
}

static int exists(const char *dir, const char *name)
{
    char p[512];
    struct stat st;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    return stat(p, &st) == 0;
}

static void test_rename(const char *cache)
{
    char d[160];
    uint32_t h, h2, st;

    printf("NtSetInformationFile(FileRenameInformation)\n");
    h = create("z:\\a.tmp");
    check(h != 0 && exists(cache, "a.tmp"), "create z:\\a.tmp", NULL);
    st = rename_to(h, "z:\\a.dat", 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "rename to a full path succeeds", d);
    check(exists(cache, "a.dat") && !exists(cache, "a.tmp"),
          "a.tmp became a.dat on the host", NULL);
    CALL(ORD_NTCLOSE, h);

    h2 = create("z:\\b.tmp");
    st = rename_to(h2, "z:\\a.dat", 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0xC0000035u, "existing target without ReplaceIfExists collides", d);
    st = rename_to(h2, "z:\\a.dat", 1);
    check(st == 0 && !exists(cache, "b.tmp"), "ReplaceIfExists replaces", NULL);
    st = rename_to(h2, "c.dat", 0);
    check(st == 0 && exists(cache, "c.dat") && !exists(cache, "a.dat"),
          "bare name renames within the same directory", NULL);
    CALL(ORD_NTCLOSE, h2);
}

/* ---- FATX overwrite: regrowing after an overwrite keeps the data ------ */
static uint32_t set_eof(uint32_t h, uint32_t size)
{
    uint32_t info = g_scratch + 0x400, ios = g_scratch + 0x18;
    G32(info) = size; G32(info + 4) = 0;
    return CALL(ORD_NTSETINFO, h, ios, info, 8, 20 /* EndOfFile */);
}

static long host_file(const char *dir, const char *name, unsigned char *buf,
                      size_t cap)
{
    char p[512];
    FILE *f;
    long n;
    snprintf(p, sizeof p, "%s/%s", dir, name);
    f = fopen(p, "rb");
    if (!f) return -1;
    n = (long)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static void test_overwrite(const char *cache)
{
    unsigned char got[2048];
    uint32_t h, i, buf = g_scratch + 0x800, ios = g_scratch + 0x18;
    long n;
    int same = 1;

    printf("FATX overwrite (the texture-cache copy)\n");
    for (i = 0; i < 1024; i++) G8(buf + i) = (uint8_t)(i * 7 + 1);
    h = create("z:\\o.tmp");
    set_eof(h, 1024);
    CALL(ORD_NTWRITE, h, 0, 0, 0, ios, buf, 1024, 0);
    CALL(ORD_NTCLOSE, h);

    /* second CREATE_ALWAYS, then trim to the true size; XAPI SetEndOfFile
     * queries the position first */
    h = create("z:\\o.tmp");
    CALL(ORD_NTQUERYINFO, h, ios, g_scratch + 0x400, 8, 14 /* Position */);
    set_eof(h, 700);
    CALL(ORD_NTCLOSE, h);
    n = host_file(cache, "o.tmp", got, sizeof got);
    for (i = 0; n == 700 && i < 700; i++)
        if (got[i] != (unsigned char)(i * 7 + 1)) same = 0;
    check(n == 700, "overwrite + end-of-file trims to 700 bytes", NULL);
    check(n == 700 && same, "the bytes below the new end survive", NULL);

    /* an overwrite followed by a write is a plain truncate */
    h = create("z:\\o.tmp");
    CALL(ORD_NTWRITE, h, 0, 0, 0, ios, buf, 4, 0);
    CALL(ORD_NTCLOSE, h);
    check(host_file(cache, "o.tmp", got, sizeof got) == 4,
          "overwrite + 4-byte write leaves 4 bytes", NULL);

    /* an overwrite that is just closed leaves an empty file */
    h = create("z:\\o.tmp");
    CALL(ORD_NTCLOSE, h);
    check(host_file(cache, "o.tmp", got, sizeof got) == 0,
          "overwrite + close leaves 0 bytes", NULL);
}

/* ---- thread handles lead to guest thread objects -------------------- */
static uint32_t spawn(uint32_t routine, uint32_t handle_va)
{
    G32(handle_va) = 0;
    /* ThreadHandle, ExtraSize, KStack, Tls, ThreadId, ctx1, ctx2,
     * CreateSuspended, DebugStack, StartRoutine */
    CALL(ORD_PSCREATE, handle_va, 0, 0, 0, 0, 0, 0, 0, 0, routine);
    return G32(handle_va);
}

static uint32_t obref(uint32_t h)
{
    G32(g_scratch + 0x200) = 0xCCCCCCCCu;
    CALL(ORD_OBREF, h, 0, g_scratch + 0x200);
    return G32(g_scratch + 0x200);
}

static void obderef(uint32_t obj)
{
    recomp_func_t fn = lookup_ord(ORD_OBDEREF);

    g_ecx = obj;
    if (fn) fn();
}

static void test_threads(void)
{
    char d[160];
    uint32_t h, h2, dup, obj, obj2, st, zero = g_scratch + 0x300;

    printf("Thread objects\n");
    xbox_SetThreadMode(XBOX_THREAD_MODE_SPAWN);
    G32(zero) = 0; G32(zero + 4) = 0;   /* a zero relative timeout */

    h = spawn(0x00020000u, g_scratch + 0x100);
    check(h != 0, "PsCreateSystemThreadEx writes a handle", NULL);
    obj = obref(h);
    snprintf(d, sizeof d, "object 0x%08X", obj);
    check(obj != 0 && obj != 0xCCCCCCCCu && G8(obj) == 6,
          "ObReferenceObjectByHandle returns a guest thread object", d);
    check(obj && G32(obj + 4) == 0 && G32(obj + 0x120) == 0x103,
          "a running thread is not signalled (STILL_ACTIVE)", NULL);
    st = CALL(ORD_NTWAIT, h, 0, zero);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0x102, "NtWaitForSingleObject times out while it runs", d);

    g_worker_go = 1;
    st = CALL(ORD_NTWAIT, h, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "NtWaitForSingleObject returns once it terminates", d);
    st = CALL(ORD_KEWAIT, obj, 0, 0, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "KeWaitForSingleObject on the object is satisfied", d);
    snprintf(d, sizeof d, "signal %u exit 0x%08X", G32(obj + 4), G32(obj + 0x120));
    check(G32(obj + 4) == 1 && G32(obj + 0x120) == 0x1234u,
          "PsTerminateSystemThread sets SignalState and ExitStatus", d);
    obderef(obj);
    CALL(ORD_NTCLOSE, h);

    h2 = spawn(0x00010000u, g_scratch + 0x104);
    G32(g_scratch + 0x108) = 0;
    CALL(ORD_NTDUP, h2, g_scratch + 0x108, 2);
    dup = G32(g_scratch + 0x108);
    st = CALL(ORD_NTWAIT, h2, 0, 0);
    check(st == 0, "a worker that returns can be waited on", NULL);
    obj2 = obref(dup);
    snprintf(d, sizeof d, "object 0x%08X signal %u exit 0x%08X", obj2,
             obj2 ? G32(obj2 + 4) : 0, obj2 ? G32(obj2 + 0x120) : 0);
    check(obj2 && obj2 != 0xCCCCCCCCu && G32(obj2 + 4) == 1 &&
          G32(obj2 + 0x120) == 0,
          "returning signals it with STATUS_SUCCESS (via a duplicate)", d);
    obderef(obj2);
    snprintf(d, sizeof d, "source 0x%08X duplicate 0x%08X", h2, dup);
    check(dup != 0 && dup != h2, "NtDuplicateObject gives a new token", d);
    CALL(ORD_NTCLOSE, h2);
    st = CALL(ORD_NTWAIT, dup, 0, 0);
    snprintf(d, sizeof d, "status 0x%08X", st);
    check(st == 0, "the duplicate outlives the first handle", d);
    obj2 = obref(dup);
    check(obj2 && obj2 != 0xCCCCCCCCu && G8(obj2) == 6,
          "the duplicate still leads to the thread object", NULL);
    obderef(obj2);
    CALL(ORD_NTCLOSE, dup);

    obj = obref(0xFFFFFFFEu);
    check(obj && obj != 0xCCCCCCCCu && G8(obj) == 6 && G32(obj + 4) == 0,
          "NtCurrentThread has an unsignalled object", NULL);
    obderef(obj);
}

int main(int argc, char **argv)
{
    char tmpl[] = "/tmp/kmembridgeXXXXXX", cache[256], cmd[300];
    size_t n = 0;
    unsigned char *xbe;
    unsigned i;

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!mkdtemp(tmpl)) return 2;
    setenv("XDG_DATA_HOME", tmpl, 1);
    snprintf(cache, sizeof cache, "%s/xboxrecomp/Cache", tmpl);
    snprintf(cmd, sizeof cmd, "mkdir -p '%s'", cache);
    if (system(cmd) != 0) return 2;

    xbe = load(argc > 1 ? argv[1] : "tools/conformance/test.xbe", &n);
    if (!xbe || !xbox_MemoryLayoutInit(xbe, n)) {
        printf("cannot map guest memory (run tools/conformance/mkxbe.py)\n");
        return 2;
    }

    g_thunks  = xbox_HeapAlloc(0x1000, 4096);
    g_stack   = xbox_HeapAlloc(0x1000, 4096);
    g_scratch = xbox_HeapAlloc(0x1000, 4096);
    for (i = 0; i < NORDS; i++)
        G32(g_thunks + 4 * i) = 0x80000000u | ORDS[i];
    G32(g_thunks + 4 * NORDS) = 0;
    xbox_kernel_set_thunk_address(g_thunks, NORDS + 1);
    xbox_kernel_bridge_init();

    test_virtual_memory();
    test_contiguous();
    test_rename(cache);
    test_overwrite(cache);
    test_threads();

    snprintf(cmd, sizeof cmd, "rm -rf '%s'", tmpl);
    (void)system(cmd);
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
