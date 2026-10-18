/*
 * nv2a_zbuf - the CPU raster's depth-buffer selection (nv2a_zbuf_cache.h).
 *
 * The bug: one shared buffer was reset when a level switched to its
 * 256x256 offscreen pass and back, and the sky then drew over the cliff.
 *
 *   cc -I src/kernel tests/nv2a_zbuf/test_main.c
 */
#include <stdio.h>
#include <string.h>

#include "nv2a_zbuf_cache.h"

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

static void main_and_offscreen(void)
{
    ZbCache c;
    ZbPick a, b, a2, b2;
    memset(&c, 0, sizeof c);
    a  = zb_select(&c, 0x35C000, 640, 480);
    b  = zb_select(&c, 0x15E9000, 256, 256);
    a2 = zb_select(&c, 0x35C000, 640, 480);
    b2 = zb_select(&c, 0x15E9000, 256, 256);
    CHECK("A is fresh on first use", a.fresh == 1);
    CHECK("B is fresh on first use", b.fresh == 1);
    CHECK("B gets its own slot", b.index != a.index);
    CHECK("A -> B -> A returns A's slot", a2.index == a.index);
    CHECK("A -> B -> A keeps A (fresh=0)", a2.fresh == 0);
    CHECK("B again keeps B", b2.index == b.index && b2.fresh == 0);
    CHECK("no evictions", c.evictions == 0);
}

static void same_zeta_other_extent(void)
{
    ZbCache c;
    ZbPick a, b;
    memset(&c, 0, sizeof c);
    a = zb_select(&c, 0x35C000, 640, 480);
    b = zb_select(&c, 0x35C000, 320, 240);
    CHECK("extent is part of the key", b.index != a.index && b.fresh == 1);
}

static void lru_eviction(int n)
{
    ZbCache c;
    ZbPick p, q;
    char name[96];
    int k;
    memset(&c, 0, sizeof c);
    for (k = 0; k < n; k++) {
        p = zb_select_n(&c, n, 0x1000u * (uint32_t)(k + 1), 64, 64);
        snprintf(name, sizeof name, "n=%d key %d fresh, no eviction", n, k);
        CHECK(name, p.fresh == 1 && p.evicted == 0);
    }
    /* Touch every key but the second, so key 1 is the least recently used. */
    for (k = 0; k < n; k++)
        if (k != 1)
            zb_select_n(&c, n, 0x1000u * (uint32_t)(k + 1), 64, 64);
    q = zb_select_n(&c, n, 0x1000u, 64, 64);              /* key 0, still there */
    snprintf(name, sizeof name, "n=%d key 0 still cached", n);
    CHECK(name, q.fresh == 0);
    p = zb_select_n(&c, n, 0xFFFF000u, 64, 64);           /* key n+1 */
    snprintf(name, sizeof name, "n=%d key n+1 evicts", n);
    CHECK(name, p.fresh == 1 && p.evicted == 1 && c.evictions == 1);
    for (k = 0; k < n; k++)
        if (c.slot[k].used && c.slot[k].zeta == 0x2000u)
            break;
    snprintf(name, sizeof name, "n=%d the LRU key (1) was the one evicted", n);
    CHECK(name, k == n);
    for (k = 0; k < n; k++)
        if (c.slot[k].used && c.slot[k].zeta == 0x1000u)
            break;
    snprintf(name, sizeof name, "n=%d key 0 survives", n);
    CHECK(name, k < n);
}

int main(void)
{
    main_and_offscreen();
    same_zeta_other_extent();
    lru_eviction(2);
    lru_eviction(4);
    lru_eviction(ZB_MAX);
    CHECK("ZB_MAX is at least 8", ZB_MAX >= 8);
    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("nv2a_zbuf: all passed\n");
    return 0;
}
