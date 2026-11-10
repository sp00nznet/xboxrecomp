/* bind_shadow - see CMakeLists.txt. */
#include "nv2a_bind_shadow.h"

#include <stdio.h>
#include <string.h>

static int s_fail, s_pass;
#define CHECK(c)                                                                \
    do {                                                                        \
        if (c)                                                                  \
            s_pass++;                                                           \
        else {                                                                  \
            s_fail++;                                                           \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);        \
        }                                                                       \
    } while (0)

/* A model encoder: what is really bound, and how many bind calls it took. */
struct enc {
    uint64_t serial;
    const void *vb, *tex[NV2A_BIND_SHADOW_STAGES], *smp[NV2A_BIND_SHADOW_STAGES];
    int binds;
};

static uint64_t s_serial;
static struct nv2a_bind_shadow s_sh;

static void enc_new(struct enc *e)
{
    memset(e, 0, sizeof *e);
    e->serial = ++s_serial;     /* monotonic, never 0, as s_enc_serial */
}

/* The backend's draw path: begin, then each slot through the shadow. */
static void draw(struct enc *e, int on, const void *vb, const void *const tex[4],
                 const void *const smp[4])
{
    int k;
    nv2a_bind_shadow_begin(&s_sh, e->serial, on);
    if (nv2a_bind_shadow_set(&s_sh.vb, vb)) {
        e->vb = vb;
        e->binds++;
    }
    for (k = 0; k < NV2A_BIND_SHADOW_STAGES; k++) {
        if (nv2a_bind_shadow_set(&s_sh.tex[k], tex[k])) {
            e->tex[k] = tex[k];
            e->binds++;
        }
        if (nv2a_bind_shadow_set(&s_sh.smp[k], smp[k])) {
            e->smp[k] = smp[k];
            e->binds++;
        }
    }
}

static int bound(const struct enc *e, const void *vb, const void *const tex[4],
                 const void *const smp[4])
{
    int k;
    if (e->vb != vb)
        return 0;
    for (k = 0; k < NV2A_BIND_SHADOW_STAGES; k++)
        if (e->tex[k] != tex[k] || e->smp[k] != smp[k])
            return 0;
    return 1;
}

static char obj[64];   /* distinct addresses for the "objects" */
#define O(i) ((const void *)&obj[(i)])

static void test_repeat_skips(void)
{
    struct enc e;
    const void *t[4] = {O(1), O(2), NULL, NULL}, *s[4] = {O(10), O(10), O(10), O(10)};
    enc_new(&e);
    draw(&e, 1, O(30), t, s);
    CHECK(bound(&e, O(30), t, s));
    /* vb, two textures, four samplers; the NULL textures are already NULL
     * on a fresh encoder. */
    CHECK(e.binds == 7);
    draw(&e, 1, O(30), t, s);
    CHECK(e.binds == 7);
    CHECK(bound(&e, O(30), t, s));
}

static void test_change_one_stage(void)
{
    struct enc e;
    const void *t[4] = {O(1), O(2), O(3), O(4)}, *s[4] = {O(10), O(11), O(10), O(11)};
    int b;
    enc_new(&e);
    draw(&e, 1, O(30), t, s);
    b = e.binds;
    t[2] = O(5);
    draw(&e, 1, O(30), t, s);
    CHECK(e.binds == b + 1);
    CHECK(bound(&e, O(30), t, s));
    /* Back to an empty stage on the same encoder: NULL must be bound. */
    t[1] = NULL;
    draw(&e, 1, O(30), t, s);
    CHECK(e.binds == b + 2);
    CHECK(e.tex[1] == NULL);
}

static void test_new_encoder_rebinds(void)
{
    struct enc a, b;
    const void *t[4] = {O(1), O(2), O(3), O(4)}, *s[4] = {O(10), O(11), O(12), O(13)};
    enc_new(&a);
    draw(&a, 1, O(30), t, s);
    enc_new(&b);
    draw(&b, 1, O(30), t, s);
    CHECK(b.binds == 9);
    CHECK(bound(&b, O(30), t, s));
}

static void test_off_binds_everything(void)
{
    struct enc e;
    const void *t[4] = {O(1), O(2), O(3), O(4)}, *s[4] = {O(10), O(11), O(12), O(13)};
    const void *n[4] = {NULL, NULL, NULL, NULL};
    enc_new(&e);
    draw(&e, 0, O(30), t, s);
    draw(&e, 0, O(30), t, s);
    CHECK(e.binds == 18);
    /* Off, NULL after an object on one encoder is bound too. */
    draw(&e, 0, O(30), n, s);
    CHECK(bound(&e, O(30), n, s));
    /* On again on the same encoder: the last off draw's binds are known,
     * so only the four textures that differ go out. */
    e.binds = 0;
    draw(&e, 1, O(30), t, s);
    CHECK(e.binds == 4);
    CHECK(bound(&e, O(30), t, s));
}

static void test_drop(void)
{
    struct enc e;
    const void *t[4] = {O(1), O(2), O(3), O(4)}, *s[4] = {O(10), O(11), O(12), O(13)};
    enc_new(&e);
    draw(&e, 1, O(30), t, s);
    /* A debug draw binds its own vertex buffer behind the shadow. */
    e.vb = O(40);
    nv2a_bind_shadow_drop(&s_sh);
    e.binds = 0;
    draw(&e, 1, O(30), t, s);
    CHECK(e.vb == O(30));
    CHECK(e.binds == 9);
    /* Dropped slots are unknown, not empty: a NULL stage is bound. */
    {
        const void *n[4] = {O(1), NULL, O(3), O(4)};
        nv2a_bind_shadow_drop(&s_sh);
        e.tex[1] = O(2);
        draw(&e, 1, O(30), n, s);
        CHECK(e.tex[1] == NULL);
    }
}

/* A long mixed sequence against the model: every draw sees its objects. */
static void test_random(void)
{
    struct enc e;
    uint32_t x = 12345u;
    int i, k, bad = 0, saved = 0, total = 0;
    enc_new(&e);
    for (i = 0; i < 20000; i++) {
        const void *t[4], *s[4], *vb;
        int on;
        x = x * 1664525u + 1013904223u;
        if ((x >> 24) < 4)
            enc_new(&e);
        if ((x >> 16 & 0xFF) < 2) {
            e.vb = O(50);
            nv2a_bind_shadow_drop(&s_sh);
        }
        on = (x >> 8 & 0xFF) >= 8;
        vb = O(30 + (x >> 4 & 1));
        for (k = 0; k < 4; k++) {
            x = x * 1664525u + 1013904223u;
            t[k] = (x >> 28) < 3 ? NULL : O(1 + (x >> 25 & 3));
            s[k] = O(10 + (x >> 22 & 1));
        }
        {
            int b0 = e.binds;
            draw(&e, on, vb, t, s);
            saved += 9 - (e.binds - b0);
            total += 9;
        }
        if (!bound(&e, vb, t, s))
            bad++;
    }
    CHECK(bad == 0);
    CHECK(saved > total / 4);   /* the point of it: most repeats skipped */
}

int main(void)
{
    test_repeat_skips();
    test_change_one_stage();
    test_new_encoder_rebinds();
    test_off_binds_everything();
    test_drop();
    test_random();
    printf("bind_shadow: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail != 0;
}
