/* No contiguous block shares a physical number with a named image page.
 *
 * Here the contiguous window and the RAM the image sits in are separate
 * storage, so a block the arena places at an image page's number gives that
 * number two owners, and a device model holding only the number has to
 * guess which one the title meant. Jet Set Radio Future lost that guess:
 * its second framebuffer (window 0x8011C000) had the number of DirectSound's
 * zero buffer (0x001A77B0), and the APU played pixels after a render-scale
 * change wrote the framebuffer back.
 *
 * Checked, one mode per process (the name record is never cleared):
 *   named    the title names an image page; the arena is then filled to the
 *            last page and no live block covers that page, and the APU
 *            resolves it to the image for structures and sample data alike.
 *            While there was room elsewhere, no block touched the image.
 *   unnamed  the same fill with nothing named: the image's pages are the
 *            arena's last resort (a large image leaves no other room), so
 *            they are used, but only once everything else is gone.
 *            A request whose range is only the image page gets it.
 *   late     a block that took an image page in that last resort, named by
 *            the title afterwards, is the one case left with two owners:
 *            sample data on it reads the image (mcpx_apu_phys_data), APU
 *            structures read the window. Once that block is freed, the
 *            page is never handed out again.
 *   fill-named  RECOMP_CONTIG_AVOID_IMAGE=0 is the old order exactly: the
 *            first block fills from the bottom over a named image page.
 *
 * Not covered: the top-down path stepping below a named page. It serves
 * only requests larger than both sides of the low reserve (~48 MB), and the
 * test image is one page at 0x11000, below any such block. */
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

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-66s %s", what, ok ? "PASS" : "FAIL");
    if (detail && *detail)
        printf("  (%s)", detail);
    putchar('\n');
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

static uint32_t img_first, img_end;   /* the image's pages, [first, end) */
static uint32_t img_block;            /* the block that took img_first, or 0 */

static int in_image(uint32_t page)
{
    return page >= img_first && page < img_end;
}

/* Live window pages that are image pages, and those the title named. */
static void count_image_pages(uint32_t *live, uint32_t *named_live)
{
    const volatile uint8_t *map = xbox_ContiguousPageMap();
    const volatile uint8_t *named = xbox_PhysImageNameMap();
    uint32_t p;

    *live = *named_live = 0;
    for (p = img_first; p < img_end; p++) {
        if (map[p] == 1) {
            (*live)++;
            if (named[p])
                (*named_live)++;
        }
    }
}

/* Fill the arena to its last page, 1 MB blocks first. Each block is checked
 * as it comes: while a free run of its size is left outside the image, it
 * may not touch an image page. Returns the number of blocks that touched
 * the image before the rest of the window was full. */
static uint32_t fill_arena(uint32_t *blocks)
{
    static const uint32_t sizes[] = { 1u << 20, 64u << 10, 4u << 10 };
    const volatile uint8_t *map = xbox_ContiguousPageMap();
    uint32_t early = 0, n = 0;
    size_t i;

    for (i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
        uint32_t va;
        while ((va = xbox_ContiguousAlloc(sizes[i], 4096)) != 0) {
            uint32_t first = (va - XBOX_CONTIG_BASE) >> 12;
            uint32_t pages = sizes[i] >> 12, p;
            int touches = 0, room = 0;

            n++;
            for (p = first; p < first + pages; p++)
                touches |= in_image(p);
            if (first <= img_first && img_first < first + pages)
                img_block = va;
            if (touches) {
                /* A free run of the same size outside the image (and the
                 * 15-16 MB low reserve, which a general request skips)
                 * means the arena had a choice. */
                uint32_t run = 0;
                for (p = 1; p < (XBOX_CONTIG_SIZE - XBOX_GPU_INSTANCE_DEFAULT) >> 12; p++) {
                    int ok = !map[p] && !in_image(p)
                          && !(p >= (15u << 8) && p < (16u << 8));
                    run = ok ? run + 1 : 0;
                    if (run >= pages) {
                        room = 1;
                        break;
                    }
                }
                if (room)
                    early++;
            }
        }
    }
    *blocks = n;
    return early;
}

/* 0 for RAM (the image), 1 for the window. */
static int side(uint8_t *host, uint32_t phys)
{
    uintptr_t off = (uintptr_t)(host - g_apu_ram_ptr);
    if (off == phys) return 0;
    if (off == 0x80000000u + (uintptr_t)phys) return 1;
    return -1;
}

static void set_env(const char *name, const char *value)
{
#ifdef _WIN32
    _putenv_s(name, value);
#else
    setenv(name, value, 1);
#endif
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "named";
    const char *path = argc > 2 ? argv[2] : "tools/conformance/test.xbe";
    int named_mode = !strcmp(mode, "named");
    int late = !strcmp(mode, "late");
    int fill_named = !strcmp(mode, "fill-named");
    size_t xbe_len = 0;
    unsigned char *xbe;
    uint32_t img, blocks, early, live, named_live;
    char d[160];

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!named_mode && !late && !fill_named && strcmp(mode, "unnamed")) {
        printf("unknown mode '%s'\n", mode);
        return 2;
    }
    if (fill_named)       /* read once, at the arena's first call */
        set_env("RECOMP_CONTIG_AVOID_IMAGE", "0");
    xbe = load_file(path, &xbe_len);
    if (!xbe) {
        printf("cannot read %s -- run: python tools/conformance/mkxbe.py\n", path);
        return 2;
    }
    if (!xbox_MemoryLayoutInit(xbe, xbe_len)) {
        puts("xbox_MemoryLayoutInit failed");
        return 2;
    }
    g_apu_ram_ptr = (uint8_t *)(uintptr_t)xbox_GetMemoryOffset();

    img_first = g_xbox_image_lo >> 12;
    img_end = (g_xbox_image_hi + 0xFFFu) >> 12;
    img = img_first << 12;
    printf("contig_image_alias: mode %s, image pages 0x%08X..0x%08X\n",
           mode, img, img_end << 12);

    /* DirectSound naming its zero buffer, before D3D's surfaces exist. */
    if (named_mode || fill_named) {
        check(xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)(img + 0x10)) == img + 0x10,
              "MmGetPhysicalAddress is the identity for the image", NULL);
        check(xbox_PhysImageNameMap()[img_first] != 0,
              "  ...and the image page is recorded as named", NULL);
    }

    if (fill_named) {
        /* The old order: bottom-up, through the image, named or not. */
        uint32_t va = xbox_ContiguousAlloc(1u << 20, 4096);
        uint32_t first = va ? (va - XBOX_CONTIG_BASE) >> 12 : 0;
        snprintf(d, sizeof d, "first 1 MB block at 0x%08X", va);
        check(va && first <= img_first && img_first < first + 256,
              "=0: the first block fills from the bottom over the named page", d);
        printf("contig_image_alias: %s\n", failures ? "FAILED" : "all passed");
        return failures ? 1 : 0;
    }

    /* A request constrained to the image page: pass 0 has nothing, pass 1
     * may take it unless the title named it. */
    {
        uint32_t va = xbox_ContiguousAllocEx(4096, 4096, img, img + 0xFFF);
        snprintf(d, sizeof d, "got 0x%08X", va);
        if (named_mode)
            check(va == 0, "a request for only the named image page is refused", d);
        else
            check(va == XBOX_CONTIG_BASE + img,
                  "a request for only an unnamed image page gets it", d);
        if (va)
            xbox_ContiguousFree(va);
    }

    early = fill_arena(&blocks);
    count_image_pages(&live, &named_live);
    snprintf(d, sizeof d, "%u blocks, %u touched the image early", blocks, early);
    check(blocks > 0 && early == 0,
          "no block took an image page while the window had other room", d);

    if (named_mode) {
        snprintf(d, sizeof d, "%u live image pages, %u named", live, named_live);
        check(named_live == 0, "a full arena still leaves the named image page alone", d);
        check(side(mcpx_apu_phys(img + 0x10), img + 0x10) == 0
              && !strcmp(mcpx_apu_phys_class(img + 0x10), "img"),
              "the APU reads the named page from the image (structures)", NULL);
        check(side(mcpx_apu_phys_data(img + 0x10), img + 0x10) == 0,
              "  ...and for sample data", NULL);
    } else {
        /* The image's pages are the last resort, and they are used: an
         * image past 48 MB would otherwise leave almost no window. */
        snprintf(d, sizeof d, "%u of %u image pages live", live, img_end - img_first);
        check(live == img_end - img_first,
              "a full arena uses the unnamed image pages as a last resort", d);
    }

    if (late) {
        /* Named after a block took it: still two owners, the case the APU's
         * sample-data rule is kept for. */
        xbox_MmGetPhysicalAddress((PVOID)(uintptr_t)(img + 0x10));
        check(xbox_ContiguousPageMap()[img_first] == 1,
              "a page named after a block took it stays the block's", NULL);
        check(side(mcpx_apu_phys(img + 0x10), img + 0x10) == 1,
              "  ...APU structures on it read the window", NULL);
        check(side(mcpx_apu_phys_data(img + 0x10), img + 0x10) == 0
              && !strcmp(mcpx_apu_phys_data_class(img + 0x10), "imgnamed"),
              "  ...sample data on it reads the image", NULL);

        /* Freed, the page is the image's alone from then on. */
        check(img_block && xbox_ContiguousFree(img_block),
              "the block holding the late-named page frees", NULL);
        {
            uint32_t va = xbox_ContiguousAllocEx(4096, 4096, img, img + 0xFFF);
            snprintf(d, sizeof d, "got 0x%08X", va);
            check(va == 0, "  ...and the named page is never handed out again", d);
        }
    }

    printf("contig_image_alias: %s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
