/*
 * host_opts_live: options changed while the game runs (nv2a_backend_common.h
 * nv2a_host_opts_request / _apply / _gen).
 *
 *   request   made on another thread, nothing changes until the flip's
 *             apply; apply returns the fields that changed and bumps the
 *             options generation once; a second apply has nothing to do
 *   pack      a pack_off change sets the pack's kind mask, so the pack
 *             generation the backends' tex_bind compares moves (a pack is
 *             loaded and the entry stops matching)
 *   merge     two requests before one flip: one apply, the later value per
 *             field; scale 0 and 1 are one value; a request of the current
 *             values changes nothing
 *   race      a thread requesting while the walker applies in a loop: only
 *             requested values are ever seen, and the last request wins
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"

static int fails;

#define CHECK(cond, ...) do { \
    if (!(cond)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } \
} while (0)

struct req { struct nv2a_host_opts o; unsigned mask; };

static void *request_thread(void *arg)
{
    const struct req *r = (const struct req *)arg;
    nv2a_host_opts_request(&r->o, r->mask);
    return NULL;
}

static void request_from_thread(const struct nv2a_host_opts *o, unsigned mask)
{
    pthread_t t;
    struct req r;
    r.o = *o;
    r.mask = mask;
    if (pthread_create(&t, NULL, request_thread, &r) != 0) {
        fails++;
        printf("FAIL pthread_create\n");
        return;
    }
    pthread_join(t, NULL);
}

/* A one-entry v2 pack of kind TEXT: hash 0x55, 2x2 at scale 2. */
static void write_pack(const char *path)
{
    uint8_t b[32 + 32 + 16];
    FILE *f;
    memset(b, 0, sizeof b);
    memcpy(b, NV2A_TEX_PACK_MAGIC, 4);
    b[4] = 2; b[8] = 1; b[12] = 2; b[16] = 32;
    b[32] = 0x55;
    b[40] = 2; b[44] = 2; b[48] = 1; b[52] = XRTP_KIND_TEXT;
    b[56] = 64;
    f = fopen(path, "wb");
    if (!f) { fails++; printf("FAIL cannot write %s\n", path); return; }
    fwrite(b, 1, sizeof b, f);
    fclose(f);
}

static void test_request(void)
{
    struct nv2a_host_opts stock, o;
    uint32_t g, pg;
    unsigned m;
    char err[128] = "";

    memset(&stock, 0, sizeof stock);
    nv2a_host_opts_set(&stock);
    write_pack("live.pack");
    CHECK(nv2a_tex_pack_load("live.pack", err, sizeof err) == 1, "pack: %s", err);
    CHECK(nv2a_tex_pack_find(0x55, 1, 1) != NULL, "text entry before");

    g = nv2a_host_opts_gen();
    pg = nv2a_tex_pack_gen();
    CHECK(nv2a_host_opts_apply() == 0, "apply with nothing pending");
    CHECK(nv2a_host_opts_gen() == g, "an empty apply bumped the gen");

    o = stock;
    o.render_scale = 2;
    o.pack_off = NV2A_PACK_OFF_TEXT;
    o.present_filter = NV2A_PRESENT_LINEAR;    /* not in the mask: ignored */
    request_from_thread(&o, NV2A_OPT_SCALE | NV2A_OPT_PACK);
    CHECK(nv2a_host_render_scale() == 1, "applied before the flip");
    CHECK(nv2a_tex_pack_find(0x55, 1, 1) != NULL, "pack changed before the flip");
    m = nv2a_host_opts_apply();
    CHECK(m == (NV2A_OPT_SCALE | NV2A_OPT_PACK), "mask %#x", m);
    CHECK(nv2a_host_render_scale() == 2, "scale %u", nv2a_host_render_scale());
    CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_NEAREST, "unmasked field written");
    CHECK(nv2a_host_opts()->pack_off == NV2A_PACK_OFF_TEXT, "pack_off");
    CHECK(nv2a_host_opts_gen() == g + 1, "gen %u, want %u", nv2a_host_opts_gen(), g + 1);
    CHECK(nv2a_tex_pack_gen() != pg, "pack gen did not move");
    CHECK(nv2a_tex_pack_find(0x55, 1, 1) == NULL, "text entry still matches with text off");
    CHECK(nv2a_host_opts_apply() == 0, "second apply");

    /* Pack back on, and the whole pack off then on: each a change. */
    o.pack_off = 0;
    nv2a_host_opts_request(&o, NV2A_OPT_PACK);
    CHECK(nv2a_host_opts_apply() == NV2A_OPT_PACK, "pack on");
    CHECK(nv2a_tex_pack_find(0x55, 1, 1) != NULL, "text entry back");
    o.pack_off = NV2A_PACK_OFF_ALL;
    pg = nv2a_tex_pack_gen();
    nv2a_host_opts_request(&o, NV2A_OPT_PACK);
    CHECK(nv2a_host_opts_apply() == NV2A_OPT_PACK && nv2a_tex_pack_gen() != pg, "pack off");
    CHECK(nv2a_tex_pack_find(0x55, 1, 1) == NULL, "pack off still matches");
    o.pack_off = 0;
    nv2a_host_opts_request(&o, NV2A_OPT_PACK);
    nv2a_host_opts_apply();
    nv2a_tex_pack_unload();
}

static void test_merge(void)
{
    struct nv2a_host_opts o;
    uint32_t g;
    unsigned m;

    o = *nv2a_host_opts();
    o.present_filter = NV2A_PRESENT_LINEAR;
    nv2a_host_opts_request(&o, NV2A_OPT_FILTER);
    o.present_filter = NV2A_PRESENT_INTEGER;
    o.fullscreen = 1;
    request_from_thread(&o, NV2A_OPT_FILTER | NV2A_OPT_FULLSCREEN);
    g = nv2a_host_opts_gen();
    m = nv2a_host_opts_apply();
    CHECK(m == (NV2A_OPT_FILTER | NV2A_OPT_FULLSCREEN), "merged mask %#x", m);
    CHECK(nv2a_host_opts()->present_filter == NV2A_PRESENT_INTEGER, "later filter lost");
    CHECK(nv2a_host_opts()->fullscreen == 1, "fullscreen");
    CHECK(nv2a_host_opts_gen() == g + 1, "one bump for a merged apply");

    /* The current values again: nothing changed, no bump. */
    g = nv2a_host_opts_gen();
    nv2a_host_opts_request(nv2a_host_opts(), NV2A_OPT_SCALE | NV2A_OPT_FILTER
                           | NV2A_OPT_FULLSCREEN | NV2A_OPT_PACK);
    CHECK(nv2a_host_opts_apply() == 0 && nv2a_host_opts_gen() == g, "same values changed");

    /* 1 then 0: guest size both times. */
    o = *nv2a_host_opts();
    o.render_scale = 1;
    nv2a_host_opts_request(&o, NV2A_OPT_SCALE);
    CHECK(nv2a_host_opts_apply() == NV2A_OPT_SCALE, "2 -> 1");
    o.render_scale = 0;
    nv2a_host_opts_request(&o, NV2A_OPT_SCALE);
    CHECK(nv2a_host_opts_apply() == 0, "1 -> 0 counted as a change");
    CHECK(nv2a_host_render_scale() == 1, "scale %u", nv2a_host_render_scale());

    nv2a_host_opts_request(NULL, NV2A_OPT_SCALE);
    nv2a_host_opts_request(&o, 0);
    CHECK(nv2a_host_opts_apply() == 0, "a NULL or empty request queued");
}

#define RACE_N 20000

static atomic_int s_race_done;

static void *race_thread(void *arg)
{
    struct nv2a_host_opts o;
    unsigned i;
    (void)arg;
    memset(&o, 0, sizeof o);
    for (i = 1; i <= RACE_N; i++) {
        o.render_scale = 2 + (i & 1);
        o.present_filter = (int)(i % 3);
        nv2a_host_opts_request(&o, NV2A_OPT_SCALE | NV2A_OPT_FILTER);
    }
    atomic_store(&s_race_done, 1);
    return NULL;
}

static void test_race(void)
{
    pthread_t t;
    unsigned applies = 0, bad = 0, s;
    if (pthread_create(&t, NULL, race_thread, NULL) != 0) {
        fails++;
        printf("FAIL pthread_create\n");
        return;
    }
    while (!atomic_load(&s_race_done)) {
        if (nv2a_host_opts_apply())
            applies++;
        s = nv2a_host_render_scale();
        if (s != 2 && s != 3 && !(s == 1 && !applies))
            bad++;
        if ((unsigned)nv2a_host_opts()->present_filter > 2u)
            bad++;
    }
    pthread_join(t, NULL);
    nv2a_host_opts_apply();      /* the flip after the last request */
    CHECK(bad == 0, "%u bad reads", bad);
    CHECK(nv2a_host_render_scale() == 2 && nv2a_host_opts()->present_filter == (int)(RACE_N % 3),
          "last request lost: scale %u filter %d", nv2a_host_render_scale(),
          nv2a_host_opts()->present_filter);
    CHECK(nv2a_host_opts_apply() == 0, "pending after the last apply");
}

int main(void)
{
    test_request();
    test_merge();
    test_race();
    if (fails) {
        printf("host_opts_live: %d failure(s)\n", fails);
        return 1;
    }
    printf("host_opts_live: ok\n");
    return 0;
}
