/*
 * XBE section paging (xbe_sections.h has the model).
 */
#include "xbe_sections.h"

#include <stdatomic.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <sched.h>
#endif
#include <stdlib.h>
#include <string.h>

/* BLiNX 1 has 56 sections; the XBE format allows more but no title on the
 * list comes near this. A section past the table only counts. */
#define XBE_SECT_MAX 512

typedef struct {
    uint32_t hdr_va, flags, va, vsize, raw_size;
    uint8_t *raw;
    int resident;
} XbeSect;

static XbeSect s_sect[XBE_SECT_MAX];
static unsigned s_n;
static int s_paging;
static uint64_t s_resident_bytes;
static unsigned s_resident;

/* The console serialises XeLoadSection on a mutex; two guest threads loading
 * the same section must not both copy it, nor both miss the 0 -> 1 edge.
 * Loads are rare, so a spin lock is enough and keeps this free of the
 * runtime's host layer. The holder can be copying a 4 MB section, so a waiter
 * yields after a short spin instead of burning a core (four on the Deck). */
static atomic_flag s_lock = ATOMIC_FLAG_INIT;
static void lock(void)
{
    unsigned spins = 0;
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        if (++spins >= 64) {
            spins = 0;
#if defined(_WIN32)
            SwitchToThread();
#else
            sched_yield();
#endif
        }
    }
}
static void unlock(void) { atomic_flag_clear_explicit(&s_lock, memory_order_release); }

void xbe_sections_set_paging(int on) { s_paging = on != 0; }
int  xbe_sections_paging(void)       { return s_paging; }

void xbe_sections_reset(void)
{
    lock();
    for (unsigned i = 0; i < s_n; i++)
        free(s_sect[i].raw);
    memset(s_sect, 0, sizeof s_sect);
    s_n = 0;
    s_resident_bytes = 0;
    s_resident = 0;
    unlock();
}

int xbe_sections_add(uint32_t hdr_va, uint32_t flags, uint32_t va,
                     uint32_t vsize, const void *raw, uint32_t raw_size)
{
    uint8_t *copy = NULL;
    if (raw_size > vsize) raw_size = vsize;
    if (raw_size && raw) {
        copy = (uint8_t *)malloc(raw_size);
        if (!copy) return -1;
        memcpy(copy, raw, raw_size);
    } else {
        raw_size = 0;
    }
    lock();
    if (s_n >= XBE_SECT_MAX) {
        unlock();
        free(copy);
        return -1;
    }
    XbeSect *s = &s_sect[s_n++];
    s->hdr_va = hdr_va;
    s->flags = flags;
    s->va = va;
    s->vsize = vsize;
    s->raw_size = raw_size;
    s->raw = copy;
    s->resident = 0;
    unlock();
    return 0;
}

static XbeSect *find(uint32_t hdr_va)
{
    for (unsigned i = 0; i < s_n; i++)
        if (s_sect[i].hdr_va == hdr_va) return &s_sect[i];
    return NULL;
}

static int in_map(size_t mem_size, uint32_t va, uint32_t bytes)
{
    return va && (uint64_t)va + bytes <= mem_size;
}

static uint32_t rd32(const uint8_t *mem, uint32_t va)
{
    uint32_t v;
    memcpy(&v, mem + va, 4);
    return v;
}

static void wr32(uint8_t *mem, uint32_t va, uint32_t v) { memcpy(mem + va, &v, 4); }

/* Head and tail point at the WORD counts of the pages a section shares with
 * its neighbours; the console's loader keeps a shared page while either
 * section using it is resident. Nothing here frees pages, but titles and the
 * XAPI section helpers can read the counts, so keep them as the console does. */
static void shared_pages(uint8_t *mem, size_t mem_size, uint32_t hdr_va, int delta)
{
    static const uint32_t offs[2] = { XBE_SECTHDR_HEAD_SHARED, XBE_SECTHDR_TAIL_SHARED };
    for (int i = 0; i < 2; i++) {
        uint32_t p = rd32(mem, hdr_va + offs[i]);
        uint16_t w;
        if (!in_map(mem_size, p, 2)) continue;
        memcpy(&w, mem + p, 2);
        if (delta > 0) w++;
        else if (w) w--;
        memcpy(mem + p, &w, 2);
    }
}

static uint64_t pages(const XbeSect *s)
{
    uint64_t lo = s->va & ~0xFFFull;
    uint64_t hi = ((uint64_t)s->va + s->vsize + 0xFFF) & ~0xFFFull;
    return hi - lo;
}

static uint32_t section_op(uint8_t *mem, size_t mem_size, uint32_t hdr_va,
                           int load, uint32_t *count_out)
{
    uint32_t count;
    XbeSect *s;

    if (!hdr_va || !in_map(mem_size, hdr_va, XBE_SECTHDR_TAIL_SHARED + 4))
        return XBE_STATUS_INVALID_PARAMETER;

    lock();
    count = rd32(mem, hdr_va + XBE_SECTHDR_REFCOUNT);
    s = s_paging ? find(hdr_va) : NULL;
    if (load) {
        if (count == 0 && s && in_map(mem_size, s->va, s->vsize)) {
            /* Fresh from the image: a title that loads a section, patches
             * pointers in it and unloads it expects the original bytes on
             * the next load, as the disc read gives it. */
            memset(mem + s->va, 0, s->vsize);
            if (s->raw_size) memcpy(mem + s->va, s->raw, s->raw_size);
            shared_pages(mem, mem_size, hdr_va, +1);
            if (!s->resident) {
                s->resident = 1;
                s_resident++;
                s_resident_bytes += pages(s);
            }
        }
        count++;
    } else if (count) {
        count--;
        if (count == 0 && s) {
            /* The pages go back to the console's pool. The bytes stay here:
             * nothing else can use that VA range, and the next load copies
             * the section again. */
            shared_pages(mem, mem_size, hdr_va, -1);
            if (s->resident) {
                s->resident = 0;
                s_resident--;
                s_resident_bytes -= pages(s);
            }
        }
    }
    wr32(mem, hdr_va + XBE_SECTHDR_REFCOUNT, count);
    unlock();
    if (count_out) *count_out = count;
    return XBE_STATUS_SUCCESS;
}

uint32_t xbe_section_load(uint8_t *mem, size_t mem_size, uint32_t hdr_va,
                          uint32_t *count_out)
{
    return section_op(mem, mem_size, hdr_va, 1, count_out);
}

uint32_t xbe_section_unload(uint8_t *mem, size_t mem_size, uint32_t hdr_va,
                            uint32_t *count_out)
{
    return section_op(mem, mem_size, hdr_va, 0, count_out);
}

void xbe_sections_load_preload(uint8_t *mem, size_t mem_size)
{
    for (unsigned i = 0; i < s_n; i++) {
        if (!(s_sect[i].flags & XBE_SECTION_FLAG_PRELOAD) || s_sect[i].resident)
            continue;
        xbe_section_load(mem, mem_size, s_sect[i].hdr_va, NULL);
    }
}

uint64_t xbe_sections_resident_bytes(void) { return s_resident_bytes; }
unsigned xbe_sections_count(void)          { return s_n; }
unsigned xbe_sections_resident(void)       { return s_resident; }
