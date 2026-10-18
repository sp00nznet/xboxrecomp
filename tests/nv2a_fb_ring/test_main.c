/* RECOMP_DEBUG=fb_dump_prog: the ring hands frames back oldest first across
 * wrap-around, and the trigger's flip arithmetic matches the dump names
 * (flip k+1 presents a draw made at flip count k). */
#include "nv2a_fb_ring.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

/* A 2x2 32bpp frame whose bytes all equal the flip's low byte. */
static void push(struct nv2a_fb_ring *r, uint32_t flip)
{
    uint8_t px[16];
    memset(px, (int)(flip & 0xFF), sizeof px);
    CHECK(nv2a_fb_ring_push(r, flip, px, 8, 2, 2, 4) == 0);
}

static void test_ring(void)
{
    struct nv2a_fb_ring r;
    uint32_t i;

    CHECK(nv2a_fb_ring_init(&r, NV2A_FB_RING_MAX + 1) == -1);

    /* Size 0 keeps nothing and is still a ring. */
    CHECK(nv2a_fb_ring_init(&r, 0) == 0);
    push(&r, 1);
    CHECK(r.count == 0);
    nv2a_fb_ring_free(&r);

    /* Filling: oldest first from slot 0. */
    CHECK(nv2a_fb_ring_init(&r, 4) == 0);
    push(&r, 10);
    push(&r, 11);
    CHECK(r.count == 2);
    CHECK(nv2a_fb_ring_get(&r, 0)->flip == 10);
    CHECK(nv2a_fb_ring_get(&r, 1)->flip == 11);

    /* Wrapped: the last four, oldest first, with their own pixels. */
    for (i = 12; i < 17; i++)
        push(&r, i);
    CHECK(r.count == 4);
    for (i = 0; i < 4; i++) {
        const struct nv2a_fb_frame *f = nv2a_fb_ring_get(&r, i);
        CHECK(f->flip == 13 + i);
        CHECK(f->px[0] == 13 + i && f->px[15] == 13 + i);
        CHECK(f->w == 2 && f->h == 2 && f->pitch == 8 && f->bpp == 4);
    }

    /* A larger frame grows its slot; the rest keep theirs. */
    {
        static uint8_t big[640 * 4 * 480];
        const struct nv2a_fb_frame *f;
        memset(big, 0xAB, sizeof big);
        CHECK(nv2a_fb_ring_push(&r, 17, big, 640 * 4, 640, 480, 4) == 0);
        f = nv2a_fb_ring_get(&r, 3);
        CHECK(f->flip == 17 && f->w == 640 && f->h == 480);
        CHECK(f->cap >= sizeof big && f->px[sizeof big - 1] == 0xAB);
        CHECK(nv2a_fb_ring_get(&r, 0)->flip == 14);
    }
    nv2a_fb_ring_free(&r);
    CHECK(r.slot == NULL && r.size == 0 && r.count == 0);
}

static void test_range(void)
{
    uint32_t first, lo, hi;

    /* SSX 3's rider: first drawn at flip count 5327. */
    nv2a_fb_trigger_range(5327, 30, 60, &first, &lo, &hi);
    CHECK(first == 5298 && lo == 5328 && hi == 5387);

    /* Early in a run the ring can't reach back past flip 1. */
    nv2a_fb_trigger_range(5, 30, 60, &first, &lo, &hi);
    CHECK(first == 1 && lo == 6 && hi == 65);
    nv2a_fb_trigger_range(0, 30, 2, &first, &lo, &hi);
    CHECK(first == 1 && lo == 1 && hi == 2);

    /* after 0: nothing after the trigger. */
    nv2a_fb_trigger_range(100, 0, 0, &first, &lo, &hi);
    CHECK(first == 101 && lo > hi);

    /* No wrap at the top of the count. */
    nv2a_fb_trigger_range(0xFFFFFFF0u, 4, 60, &first, &lo, &hi);
    CHECK(lo == 0xFFFFFFF1u && hi == 0xFFFFFFFFu);
    nv2a_fb_trigger_range(0xFFFFFFFFu, 4, 60, &first, &lo, &hi);
    CHECK(lo > hi);
}

int main(void)
{
    test_ring();
    test_range();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("nv2a_fb_ring: all checks passed\n");
    return 0;
}
