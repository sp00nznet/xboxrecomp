/*
 * XBE section paging (src/kernel/xbe_sections.c), against a small fake guest
 * map holding two section headers and their shared-page counts:
 *  - (a) paging off: load and unload only count, the section is not touched,
 *    and an unload at 0 stays at 0 (the pre-paging behaviour);
 *  - (b) paging on: a registered non-preload section is zero until its first
 *    load, which copies its bytes and zeroes the BSS tail; a second load only
 *    counts;
 *  - (c) the reload after a full unload is fresh: bytes the title patched
 *    while it was loaded are back to the image's;
 *  - (d) head/tail shared-page counts follow the 0 -> 1 and 1 -> 0 edges, and
 *    the resident tally is page-rounded;
 *  - (e) load_preload makes a preload section resident at refcount 1 and
 *    leaves the non-preload one alone;
 *  - (f) an unregistered header only counts; a NULL or out-of-map header is
 *    STATUS_INVALID_PARAMETER;
 *  - (g) two packed sections sharing one WORD count (A's tail is B's head,
 *    BLiNX 1's layout): the count follows both, and A's fresh copy leaves
 *    B's bytes in the shared page alone;
 *  - (h) a section inside one page (head and tail the same count) moves it
 *    by two;
 *  - (i) a reload after the title wrote into the BSS tail zeroes it again.
 */
#include "xbe_sections.h"

#include <stdio.h>
#include <string.h>

#define MAP   0x20000u
#define HDR_A 0x1000u /* non-preload, 0x1800 bytes at 0x4000, 0x10 raw */
#define HDR_B 0x1040u /* preload, 0x800 bytes at 0x8000 */
#define HDR_X 0x1080u /* never registered */
#define SHARED 0x1200u /* WORD counts: A head, A tail, B head, B tail */

static uint8_t mem[MAP];
static int fails;

#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
    printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t r32(uint32_t va) { uint32_t v; memcpy(&v, mem + va, 4); return v; }
static uint16_t r16(uint32_t va) { uint16_t v; memcpy(&v, mem + va, 2); return v; }
static void w32(uint32_t va, uint32_t v) { memcpy(mem + va, &v, 4); }

static void header(uint32_t hdr, uint32_t flags, uint32_t va, uint32_t vsize, uint32_t shared)
{
    w32(hdr + XBE_SECTHDR_FLAGS, flags);
    w32(hdr + XBE_SECTHDR_VA, va);
    w32(hdr + XBE_SECTHDR_VSIZE, vsize);
    w32(hdr + XBE_SECTHDR_REFCOUNT, 0);
    w32(hdr + XBE_SECTHDR_HEAD_SHARED, shared);
    w32(hdr + XBE_SECTHDR_TAIL_SHARED, shared + 2);
}

static const uint8_t raw_a[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
static const uint8_t raw_b[4] = { 0xB0, 0xB1, 0xB2, 0xB3 };

static void setup(int paging)
{
    memset(mem, 0, sizeof mem);
    header(HDR_A, 0, 0x4000, 0x1800, SHARED);
    header(HDR_B, XBE_SECTION_FLAG_PRELOAD, 0x8000, 0x800, SHARED + 4);
    header(HDR_X, 0, 0xA000, 0x100, 0);
    xbe_sections_reset();
    xbe_sections_set_paging(paging);
    CHECK(xbe_sections_add(HDR_A, 0, 0x4000, 0x1800, raw_a, sizeof raw_a) == 0, "add A");
    CHECK(xbe_sections_add(HDR_B, XBE_SECTION_FLAG_PRELOAD, 0x8000, 0x800, raw_b,
                           sizeof raw_b) == 0, "add B");
}

static int all_zero(uint32_t va, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++)
        if (mem[va + i]) return 0;
    return 1;
}

int main(void)
{
    uint32_t c, st;

    /* (a) */
    setup(0);
    st = xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(st == XBE_STATUS_SUCCESS && c == 1, "off: load -> %u (st %08X)", c, st);
    CHECK(all_zero(0x4000, 0x1800), "off: section touched");
    CHECK(r16(SHARED) == 0, "off: shared count moved");
    xbe_section_unload(mem, MAP, HDR_A, &c);
    xbe_section_unload(mem, MAP, HDR_A, &c);
    CHECK(c == 0 && r32(HDR_A + XBE_SECTHDR_REFCOUNT) == 0, "off: unload at 0 -> %u", c);
    CHECK(xbe_sections_resident() == 0, "off: resident %u", xbe_sections_resident());

    /* (b) */
    setup(1);
    CHECK(all_zero(0x4000, 0x1800), "on: section present before load");
    mem[0x4000 + 0x100] = 0xEE;   /* stale byte in the BSS tail */
    st = xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(st == 0 && c == 1, "on: load -> %u", c);
    CHECK(memcmp(mem + 0x4000, raw_a, sizeof raw_a) == 0, "on: bytes not copied");
    CHECK(all_zero(0x4000 + sizeof raw_a, 0x1800 - sizeof raw_a), "on: BSS not zeroed");
    mem[0x4000] = 0x55;           /* the title patches it while loaded */
    xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(c == 2 && mem[0x4000] == 0x55, "on: second load recopied (%u, %02X)", c, mem[0x4000]);

    /* (d) after the first load */
    CHECK(r16(SHARED) == 1 && r16(SHARED + 2) == 1, "on: shared %u/%u",
          r16(SHARED), r16(SHARED + 2));
    CHECK(xbe_sections_resident() == 1 && xbe_sections_resident_bytes() == 0x2000,
          "on: resident %u, %llu bytes", xbe_sections_resident(),
          (unsigned long long)xbe_sections_resident_bytes());

    /* (c) */
    xbe_section_unload(mem, MAP, HDR_A, &c);
    CHECK(c == 1 && xbe_sections_resident() == 1, "on: 2 -> 1 dropped it");
    xbe_section_unload(mem, MAP, HDR_A, &c);
    CHECK(c == 0 && xbe_sections_resident() == 0 && xbe_sections_resident_bytes() == 0,
          "on: 1 -> 0 kept it resident");
    CHECK(r16(SHARED) == 0 && r16(SHARED + 2) == 0, "on: shared after unload %u/%u",
          r16(SHARED), r16(SHARED + 2));
    xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(c == 1 && mem[0x4000] == 1, "on: reload not fresh (%02X)", mem[0x4000]);

    /* (e) */
    setup(1);
    xbe_sections_load_preload(mem, MAP);
    CHECK(r32(HDR_B + XBE_SECTHDR_REFCOUNT) == 1, "preload: refcount %u",
          r32(HDR_B + XBE_SECTHDR_REFCOUNT));
    CHECK(memcmp(mem + 0x8000, raw_b, sizeof raw_b) == 0, "preload: bytes");
    CHECK(r32(HDR_A + XBE_SECTHDR_REFCOUNT) == 0 && all_zero(0x4000, 0x1800),
          "preload: non-preload section loaded");
    CHECK(r16(SHARED + 4) == 1 && xbe_sections_resident() == 1, "preload: tally");
    xbe_sections_load_preload(mem, MAP);
    CHECK(r32(HDR_B + XBE_SECTHDR_REFCOUNT) == 1, "preload twice: refcount %u",
          r32(HDR_B + XBE_SECTHDR_REFCOUNT));

    /* (f) */
    st = xbe_section_load(mem, MAP, HDR_X, &c);
    CHECK(st == 0 && c == 1 && all_zero(0xA000, 0x100), "unregistered: %u", c);
    CHECK(xbe_section_load(mem, MAP, 0, &c) == XBE_STATUS_INVALID_PARAMETER, "NULL header");
    CHECK(xbe_section_load(mem, MAP, MAP - 8, &c) == XBE_STATUS_INVALID_PARAMETER,
          "header past the map");

    /* (g) A = 0x4000..0x57F8, B = 0x57F8..0x6000: page 0x5000 is shared,
     * one count at SHARED + 2 is A's tail and B's head. */
    memset(mem, 0, sizeof mem);
    xbe_sections_reset();
    xbe_sections_set_paging(1);
    header(HDR_A, 0, 0x4000, 0x17F8, SHARED);
    header(HDR_B, 0, 0x57F8, 0x808, SHARED + 2);
    xbe_sections_add(HDR_A, 0, 0x4000, 0x17F8, raw_a, sizeof raw_a);
    xbe_sections_add(HDR_B, 0, 0x57F8, 0x808, raw_b, sizeof raw_b);
    xbe_section_load(mem, MAP, HDR_A, &c);
    xbe_section_load(mem, MAP, HDR_B, &c);
    CHECK(r16(SHARED) == 1 && r16(SHARED + 2) == 2 && r16(SHARED + 4) == 1,
          "packed: counts %u/%u/%u", r16(SHARED), r16(SHARED + 2), r16(SHARED + 4));
    mem[0x57F8 + 8] = 0x77;       /* B's BSS, in the shared page */
    xbe_section_unload(mem, MAP, HDR_A, &c);
    CHECK(r16(SHARED + 2) == 1, "packed: shared count after A's unload %u", r16(SHARED + 2));
    xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(memcmp(mem + 0x57F8, raw_b, sizeof raw_b) == 0 && mem[0x57F8 + 8] == 0x77,
          "packed: A's reload touched B's bytes");
    CHECK(r16(SHARED + 2) == 2, "packed: shared count after A's reload %u", r16(SHARED + 2));

    /* (h) */
    memset(mem, 0, sizeof mem);
    xbe_sections_reset();
    xbe_sections_set_paging(1);
    header(HDR_A, 0, 0x4100, 0x200, SHARED);
    w32(HDR_A + XBE_SECTHDR_TAIL_SHARED, SHARED);
    xbe_sections_add(HDR_A, 0, 0x4100, 0x200, raw_a, sizeof raw_a);
    xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(r16(SHARED) == 2 && xbe_sections_resident_bytes() == 0x1000,
          "one page: count %u, %llu bytes", r16(SHARED),
          (unsigned long long)xbe_sections_resident_bytes());
    xbe_section_unload(mem, MAP, HDR_A, &c);
    CHECK(r16(SHARED) == 0, "one page: count after unload %u", r16(SHARED));

    /* (i) */
    setup(1);
    xbe_section_load(mem, MAP, HDR_A, &c);
    memset(mem + 0x4000 + sizeof raw_a, 0xCC, 0x1800 - sizeof raw_a);
    xbe_section_unload(mem, MAP, HDR_A, &c);
    xbe_section_load(mem, MAP, HDR_A, &c);
    CHECK(all_zero(0x4000 + sizeof raw_a, 0x1800 - sizeof raw_a),
          "reload: BSS written while loaded not zeroed");

    xbe_sections_reset();
    printf(fails ? "xbe_sections: %d FAIL\n" : "xbe_sections: all pass\n", fails);
    return fails != 0;
}
