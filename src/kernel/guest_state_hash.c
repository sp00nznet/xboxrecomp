/*
 * guest_state_hash.c - see guest_state_hash.h.
 */
#include "kernel.h"
#include "guest_state_hash.h"
#include "guest_time.h"
#include "xbox_memory_layout.h"
#include "recomp_env.h"
#include "usb_gamepad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern ptrdiff_t g_xbox_mem_offset;

#define PAGE_SHIFT  16                       /* 64 KB pages */
#define PAGE_SIZE   ((size_t)1 << PAGE_SHIFT)
#define MAX_SKIPS   16
#define MAX_DUMPS   16

static FILE *s_log, *s_pages;
static char *s_path;
static uint32_t s_skip_lo[MAX_SKIPS], s_skip_hi[MAX_SKIPS];
static int s_nskips;
static uint32_t s_dump[MAX_DUMPS];
static int s_ndumps;
static uint64_t *s_page_hash;
static uint8_t *s_record;        /* one .pages record, at most every page */
static size_t s_npages;
static int s_first = 1;

/* Four independent multiply-xor lanes over 64-bit words: fast enough for
 * 64 MB a frame, and any changed word changes the result. Not a
 * cryptographic hash; two runs are compared, nobody is attacking them. */
static uint64_t hash_words(const uint64_t *w, size_t n, uint64_t seed)
{
    const uint64_t k = 0x9E3779B97F4A7C15ULL;
    uint64_t a = seed, b = seed ^ 0xC2B2AE3D27D4EB4FULL,
             c = seed ^ 0x165667B19E3779F9ULL, d = seed ^ 0x27D4EB2F165667C5ULL;
    size_t i = 0;

    for (; i + 4 <= n; i += 4) {
        a = (a ^ w[i + 0]) * k;
        b = (b ^ w[i + 1]) * k;
        c = (c ^ w[i + 2]) * k;
        d = (d ^ w[i + 3]) * k;
    }
    for (; i < n; i++)
        a = (a ^ w[i]) * k;
    a ^= (b << 1 | b >> 63) ^ (c << 2 | c >> 62) ^ (d << 3 | d >> 61);
    a ^= a >> 29;
    a *= k;
    return a ^ (a >> 32);
}

uint64_t xbox_GuestStateHashBytes(const void *p, size_t len)
{
    return hash_words((const uint64_t *)p, len / 8, (uint64_t)len);
}

/* The hash of the 64 KB page at guest address lo, read from src, with the
 * skipped ranges read as zero. */
static uint64_t page_hash_at(const uint8_t *src, uint32_t lo)
{
    uint64_t hi = (uint64_t)lo + PAGE_SIZE;
    int i, masked = 0;

    for (i = 0; i < s_nskips; i++)
        if (s_skip_lo[i] < hi && s_skip_hi[i] > lo)
            masked = 1;
    if (!masked)
        return hash_words((const uint64_t *)src, PAGE_SIZE / 8, lo >> PAGE_SHIFT);
    {
        static uint64_t buf[PAGE_SIZE / 8];
        memcpy(buf, src, PAGE_SIZE);
        for (i = 0; i < s_nskips; i++) {
            uint64_t a = s_skip_lo[i] > lo ? s_skip_lo[i] : lo;
            uint64_t b = s_skip_hi[i] < hi ? s_skip_hi[i] : hi;
            if (a < b)
                memset((uint8_t *)buf + (a - lo), 0, (size_t)(b - a));
        }
        return hash_words(buf, PAGE_SIZE / 8, lo >> PAGE_SHIFT);
    }
}

static uint64_t page_hash(size_t page)
{
    uint32_t lo = (uint32_t)(page << PAGE_SHIFT);

    return page_hash_at((const uint8_t *)(uintptr_t)g_xbox_mem_offset + lo, lo);
}

uint64_t xbox_GuestStateHashPage(const void *p, uint32_t va)
{
    return page_hash_at((const uint8_t *)p, va & ~(uint32_t)(PAGE_SIZE - 1));
}

static void hash_warn(const char *key, const char *item, const char *want)
{
    fprintf(stderr, "  [HASH] %s: ignored \"%s\" (want %s)\n", key, item,
            want);
    fflush(stderr);
}

/* One comma-separated item: its text for a warning, and where the next one
 * starts (NULL after the last). */
static const char *next_item(const char *v, char *item, size_t cap)
{
    size_t n = strcspn(v, ",");

    if (n >= cap)
        n = cap - 1;
    memcpy(item, v, n);
    item[n] = 0;
    v += strcspn(v, ",");
    return *v == ',' ? v + 1 : NULL;
}

int xbox_GuestStateHashSetSkips(const char *v)
{
    char item[64];

    s_nskips = 0;
    while (v && *v) {
        char *end;
        unsigned long long lo, hi;
        const char *next = next_item(v, item, sizeof item);

        lo = strtoull(item, &end, 16);
        hi = 0;
        if (end != item && *end == '-') {
            const char *h = end + 1;
            hi = strtoull(h, &end, 16);
            if (end == h)
                hi = 0;
        }
        if (hi > 0xFFFFFFFFull)
            hi = 0xFFFFFFFFull;   /* to the top of the 32-bit space */
        if (*end || hi <= lo)
            hash_warn("state_hash_skip", item, "hex lo-hi with lo < hi");
        else if (s_nskips == MAX_SKIPS)
            hash_warn("state_hash_skip", item, "at most 16 ranges");
        else {
            s_skip_lo[s_nskips] = (uint32_t)lo;
            s_skip_hi[s_nskips] = (uint32_t)hi;
            s_nskips++;
        }
        v = next;
    }
    return s_nskips;
}

int xbox_GuestStateHashSetDumps(const char *v)
{
    char item[64];

    s_ndumps = 0;
    while (v && *v) {
        char *end;
        unsigned long long poll;
        const char *next = next_item(v, item, sizeof item);

        poll = strtoull(item, &end, 10);
        if (end == item || *end || poll > 0xFFFFFFFFull)
            hash_warn("state_dump", item, "a decimal poll");
        else if (s_ndumps == MAX_DUMPS)
            hash_warn("state_dump", item, "at most 16 polls");
        else
            s_dump[s_ndumps++] = (uint32_t)poll;
        v = next;
    }
    return s_ndumps;
}

static void dump_ram(uint32_t poll)
{
    char *name = malloc(strlen(s_path) + 32);
    FILE *f;

    if (!name)
        return;
    sprintf(name, "%s.%u.ram", s_path, (unsigned)poll);
    f = fopen(name, "wb");
    if (f) {
        fwrite((const void *)(uintptr_t)g_xbox_mem_offset, 1,
               s_npages * PAGE_SIZE, f);
        fclose(f);
        fprintf(stderr, "  [HASH] guest RAM at poll %u -> %s\n",
                (unsigned)poll, name);
        fflush(stderr);
    }
    free(name);
}

/* At the end of each port-0 poll, on the polling thread (the title's own,
 * for a title that answers XInputGetState itself). */
static void on_poll(uint32_t poll)
{
    uint64_t all = 0x84222325CBF29CE4ULL;
    uint32_t nchanged = 0;
    uint8_t *r = s_record + 8;
    size_t p;
    int i;

    /* The record: u32 poll, u32 count, then (u32 page, u64 hash) for each
     * page whose hash moved since the previous poll (all of them on the
     * first). Built whole, then written once, so the file never holds a
     * count that does not match its pages. */
    for (p = 0; p < s_npages; p++) {
        uint64_t h = page_hash(p);
        all = (all ^ h) * 0x100000001B3ULL;
        if (s_first || h != s_page_hash[p]) {
            uint32_t pg = (uint32_t)p;
            s_page_hash[p] = h;
            memcpy(r, &pg, 4);
            memcpy(r + 4, &h, 8);
            r += 12;
            nchanged++;
        }
    }
    s_first = 0;
    memcpy(s_record, &poll, 4);
    memcpy(s_record + 4, &nchanged, 4);
    fwrite(s_record, 1, (size_t)(r - s_record), s_pages);

    fprintf(s_log, "%u %llu %llu %016llx %u\n", (unsigned)poll,
            (unsigned long long)xbox_GuestTimeVblanks(),
            (unsigned long long)xbox_GuestInterruptTime(),
            (unsigned long long)all, (unsigned)nchanged);
    /* Flushed every poll: the runs end by closing the window, and the
     * lines before that are the ones wanted. */
    fflush(s_log);
    fflush(s_pages);

    for (i = 0; i < s_ndumps; i++)
        if (s_dump[i] == poll)
            dump_ram(poll);
}

void xbox_GuestStateHashInit(void)
{
    const char *path = recomp_env(RENV_STATE_HASH);
    char *pages;

    if (!path || !*path)
        return;
    s_npages = g_xbox_total_ram >> PAGE_SHIFT;
    s_page_hash = calloc(s_npages, sizeof *s_page_hash);
    s_record = malloc(8 + 12 * s_npages);
    s_path = malloc(strlen(path) + 1);
    pages = malloc(strlen(path) + 8);
    if (!s_page_hash || !s_record || !s_path || !pages)
        return;
    strcpy(s_path, path);
    sprintf(pages, "%s.pages", path);
    s_log = fopen(path, "w");
    s_pages = fopen(pages, "wb");
    free(pages);
    if (!s_log || !s_pages) {
        fprintf(stderr, "  [HASH] cannot write %s(.pages); state_hash off\n",
                path);
        fflush(stderr);
        return;
    }

    xbox_GuestStateHashSetSkips(recomp_env(RENV_STATE_HASH_SKIP));
    xbox_GuestStateHashSetDumps(recomp_env(RENV_STATE_DUMP));

    fprintf(s_log, "# poll vblank vtime_100ns hash pages_changed;"
            " ram %zu MB, %d skips\n", s_npages >> 4, s_nskips);
    fprintf(stderr, "  [HASH] guest RAM hash per pad poll -> %s (%zu pages,"
            " %d skips, %d dumps)\n", path, s_npages, s_nskips, s_ndumps);
    fflush(stderr);
    usb_gamepad_set_poll_hook(on_poll);
}
