/* The APU reads the copy of a page the title named.
 *
 * Here the contiguous window and RAM below 64 MB are separate storage, and the
 * heap's physical numbers (MmGetPhysicalAddress is the identity outside the
 * window) overlap the numbers the window arena hands out. A title that loads
 * its sound effects into the heap gives DirectSound those physical numbers;
 * the APU used to resolve every page the arena had allocated to the window,
 * and played whatever the window held there as the effect.
 *
 * Checked: a window page nobody named from RAM stays the window (a title's
 * DirectSound voice array lives there); once MmGetPhysicalAddress names the
 * page from a heap address it is RAM, and a later window name of the same
 * number does not undo that (D3D names its window pages constantly); freeing
 * the heap block gives the page back to the window, except a page the freed
 * block shares with a live neighbour, which goes back once that neighbour is
 * freed too. Only heap pages are recorded: an image or stack page is never
 * freed, so its record would never clear (and a voice array at a window
 * number inside a large image must stay the window). */
#include "kernel.h"
#include "xbox_memory_layout.h"
#include "apu_shim.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Provided by the generated title; this harness has none. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static unsigned char *load_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long n;

    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    rewind(f);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (size_t)n;
    return buf;
}

/* Where the resolver sends a physical address: 0 for RAM, 1 for the window. */
static int side(uint32_t phys)
{
    uintptr_t off = (uintptr_t)(mcpx_apu_phys(phys) - g_apu_ram_ptr);
    if (off == phys) return 0;
    if (off == 0x80000000u + (uintptr_t)phys) return 1;
    return -1;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tools/conformance/test.xbe";
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t a, b, c, w;

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
    /* mcpx_apu_init's job in the runtime; only the base matters here. */
    g_apu_ram_ptr = (uint8_t *)(uintptr_t)xbox_GetMemoryOffset();

    /* Three heap blocks: a and c whole pages, b sharing c's first page. */
    a = xbox_HeapAlloc(0x3000, 4096);
    b = xbox_HeapAlloc(0x800, 4096);
    c = xbox_HeapAlloc(0x2000, 16);
    if (!a || !b || !c || c + 0x2000 > XBOX_CONTIG_SIZE) {
        printf("heap blocks 0x%08X 0x%08X 0x%08X: need them below 64 MB\n", a, b, c);
        return 2;
    }
    printf("apu_phys: heap blocks at 0x%08X 0x%08X 0x%08X\n", a, b, c);

    /* The arena hands out the same physical numbers as window pages. */
    xbox_ContiguousClaim(XBOX_CONTIG_BASE + a, (c + 0x2000) - a);
    check(side(a + 0x10) == 1, "an unnamed window page reads the window");
    check(!strcmp(mcpx_apu_phys_class(a), "win"), "  ...and its trace class is win");

    /* DirectSound names each page of a heap buffer. */
    for (w = a; w < a + 0x3000; w += 0x1000)
        check(xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)w) == w,
              "MmGetPhysicalAddress is the identity for the heap");
    xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)b);
    check(side(a + 0x10) == 0 && side(a + 0x2FF0) == 0,
          "a page named from the heap reads RAM");
    check(!strcmp(mcpx_apu_phys_class(a), "named"), "  ...and its trace class is named");
    check(side(c + 0x1800) == 1, "a page nobody named still reads the window");

    xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)(XBOX_CONTIG_BASE + a));
    check(side(a + 0x10) == 0, "a window name of the same number keeps RAM");

    /* Not heap: the image and the main stack are never recorded. */
    xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)g_xbox_image_lo);
    xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)(XBOX_STACK_BASE + 0x100));
    check(!xbox_PhysRamNameMap()[g_xbox_image_lo >> 12],
          "an image page is not recorded");
    check(!xbox_PhysRamNameMap()[(XBOX_STACK_BASE + 0x100) >> 12],
          "a stack page is not recorded");

    xbox_HeapFree(a);
    check(side(a + 0x10) == 1 && side(a + 0x2FF0) == 1,
          "freeing the heap block gives its pages back to the window");

    for (w = c; w < c + 0x2000; w += 0x1000)
        xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)w);
    xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)(c + 0x1FFF));
    check(side(c + 0x1000) == 0, "c's pages named from the heap read RAM");
    xbox_HeapFree(c);
    check(side(b) == 0, "a page shared with a live block stays RAM");
    check(side((c + 0x1000) & ~0xFFFu) == 1,
          "a page wholly inside a freed block goes back to the window");
    check(side(c + 0x1FFF) == 1,
          "the freed tail page with no live block after it goes back too");

    /* b merges with free a and c; the page b shared with c is free now. */
    xbox_HeapFree(b);
    check(side(b) == 1, "a page shared by two blocks both now free goes back");

    printf("apu_phys: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
