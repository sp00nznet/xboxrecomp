/* dbg_draw - see CMakeLists.txt. */
#include "nv2a_dbg_draw.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <pthread.h>
#endif

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
#define NEAR(a, b) (fabsf((a) - (b)) < 1e-4f)

struct vtx {
    float p[3];
    uint32_t argb;
};

static const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};

/* A D3D left-handed perspective, row-vector: zn 1, zf 101, square. */
static void persp(float m[16])
{
    float zn = 1.0f, zf = 101.0f, q = zf / (zf - zn);

    memset(m, 0, 16 * sizeof *m);
    m[0] = 1.0f;
    m[5] = 1.0f;
    m[10] = q;
    m[11] = 1.0f;
    m[14] = -zn * q;
}

static void test_transform(void)
{
    float view[16], proj[16], out[4], p[3] = {1, 2, 3};

    /* Row-vector: the translation is the bottom row. */
    memcpy(view, I, sizeof view);
    view[12] = 10.0f;
    view[13] = 20.0f;
    view[14] = 30.0f;
    nv2a_dbg_transform(view, I, p, 0, out);
    CHECK(NEAR(out[0], 11) && NEAR(out[1], 22) && NEAR(out[2], 33) && NEAR(out[3], 1));

    /* Perspective: z / w at the near plane is 0, at the far plane 1. */
    persp(proj);
    p[0] = 0;
    p[1] = 0;
    p[2] = 1;
    nv2a_dbg_transform(I, proj, p, 0, out);
    CHECK(NEAR(out[2] / out[3], 0.0f));
    p[2] = 101;
    nv2a_dbg_transform(I, proj, p, 0, out);
    CHECK(NEAR(out[2] / out[3], 1.0f));

    /* The pull keeps the pixel (x/w, y/w) and moves the depth nearer. */
    {
        float a[4], b[4], q[3] = {3, -2, 20};

        nv2a_dbg_transform(I, proj, q, 0, a);
        nv2a_dbg_transform(I, proj, q, 1, b);
        CHECK(NEAR(a[0] / a[3], b[0] / b[3]) && NEAR(a[1] / a[3], b[1] / b[3]));
        CHECK(b[2] / b[3] < a[2] / a[3]);
        CHECK(NEAR(b[3], 20.0f * (1.0f - NV2A_DBG_PULL_K)));
    }
}

static void test_lists(void)
{
    struct vtx t[6] = {{{0, 0, 0}, 0x80112233u}, {{1, 0, 0}, 0x80112233u}, {{0, 1, 0}, 0x80112233u},
                       {{0, 0, 1}, 0xFF445566u}, {{1, 0, 1}, 0xFF445566u}, {{0, 1, 1}, 0xFF445566u}};
    const struct nv2a_dbg_list *l;

    nv2a_dbg_reset();
    CHECK(nv2a_dbg_take() == NULL);
    /* Nothing outside begin/publish. */
    CHECK(nv2a_dbg_add(0, t, sizeof *t, 1, 0) == 0);
    CHECK(nv2a_dbg_publish() == -1);
    CHECK(nv2a_dbg_take() == NULL);

    CHECK(nv2a_dbg_begin(I, I) == 1);
    CHECK(nv2a_dbg_begin(I, I) == 0);            /* the open list stays */
    CHECK(nv2a_dbg_add(0, t, sizeof *t, 2, 0) == 2);
    CHECK(nv2a_dbg_add(0, t, sizeof *t, 1, NV2A_DBG_OPAQUE) == 1);
    CHECK(nv2a_dbg_add(0, t, sizeof *t, 1, NV2A_DBG_XRAY | NV2A_DBG_OPAQUE) == 1);
    CHECK(nv2a_dbg_add(1, t, sizeof *t, 3, 0) == 3);
    CHECK(nv2a_dbg_add(1, t, sizeof *t, 1, NV2A_DBG_XRAY | NV2A_DBG_PULL) == 1);
    CHECK(nv2a_dbg_add(1, t, sizeof *t, 1, 0x8u) == 0);      /* unknown flag */
    CHECK(nv2a_dbg_add(1, t, 12, 1, 0) == 0);                /* stride too short */
    CHECK(nv2a_dbg_take() == NULL);              /* not published yet */
    CHECK(nv2a_dbg_publish() == 6 + 3 + 3 + 6 + 2);

    l = nv2a_dbg_take();
    CHECK(l != NULL);
    if (!l)
        return;
    CHECK(l->n[NV2A_DBG_TRI_BLEND] == 6 && l->n[NV2A_DBG_TRI_OPAQUE] == 3);
    CHECK(l->n[NV2A_DBG_TRI_XRAY] == 3 && l->n[NV2A_DBG_LINE] == 6 && l->n[NV2A_DBG_LINE_XRAY] == 2);
    CHECK(l->total == 20 && l->dropped == 0 && l->serial == 1);
    /* 0xAARRGGBB to bytes r, g, b, a. */
    CHECK(l->v[NV2A_DBG_TRI_BLEND][0].rgba[0] == 0x11 && l->v[NV2A_DBG_TRI_BLEND][0].rgba[1] == 0x22
          && l->v[NV2A_DBG_TRI_BLEND][0].rgba[2] == 0x33 && l->v[NV2A_DBG_TRI_BLEND][0].rgba[3] == 0x80);
    CHECK(l->v[NV2A_DBG_TRI_BLEND][3].rgba[0] == 0x44 && l->v[NV2A_DBG_TRI_BLEND][3].rgba[3] == 0xFF);
    CHECK(NEAR(l->v[NV2A_DBG_TRI_BLEND][4].x, 1) && NEAR(l->v[NV2A_DBG_TRI_BLEND][4].z, 1)
          && NEAR(l->v[NV2A_DBG_TRI_BLEND][4].w, 1));
    CHECK(sizeof(struct nv2a_dbg_vtx) == 20);

    /* No new publish: the flip keeps the same list. */
    CHECK(nv2a_dbg_take() == l);

    /* An empty frame clears it. */
    CHECK(nv2a_dbg_begin(I, I) == 1);
    CHECK(nv2a_dbg_publish() == 0);
    l = nv2a_dbg_take();
    CHECK(l && l->total == 0 && l->serial == 2);

    /* Several publishes between two takes: the flip sees the newest. */
    for (int k = 1; k <= 3; k++) {
        nv2a_dbg_begin(I, I);
        nv2a_dbg_add(1, t, sizeof *t, (uint32_t)k, 0);
        nv2a_dbg_publish();
    }
    l = nv2a_dbg_take();
    CHECK(l && l->n[NV2A_DBG_LINE] == 6 && l->serial == 5);

    /* Hidden until the next publish. */
    nv2a_dbg_hide();
    CHECK(nv2a_dbg_take() == NULL);
    nv2a_dbg_begin(I, I);
    nv2a_dbg_add(0, t, sizeof *t, 1, 0);
    nv2a_dbg_publish();
    l = nv2a_dbg_take();
    CHECK(l && l->total == 3);
    CHECK(nv2a_dbg_published() == 6);

    /* A cancelled frame publishes nothing and lets the next begin open. */
    CHECK(nv2a_dbg_begin(I, I) == 1);
    nv2a_dbg_add(0, t, sizeof *t, 2, 0);
    nv2a_dbg_cancel();
    CHECK(nv2a_dbg_add(0, t, sizeof *t, 1, 0) == 0);
    CHECK(nv2a_dbg_publish() == -1);
    CHECK(nv2a_dbg_take() == l);
    CHECK(nv2a_dbg_begin(I, I) == 1);
    CHECK(nv2a_dbg_publish() == 0);
    CHECK(nv2a_dbg_published() == 7);
}

static void test_limit(void)
{
    static struct vtx big[4096 * 3];
    uint32_t got = 0;
    const struct nv2a_dbg_list *l;

    nv2a_dbg_reset();
    nv2a_dbg_begin(I, I);
    /* One line takes 2 of NV2A_DBG_MAX_VTX first, so triangles fill the
     * rest a whole triangle at a time. */
    nv2a_dbg_add(1, big, sizeof *big, 1, 0);
    while (got < NV2A_DBG_MAX_VTX) {
        int n = nv2a_dbg_add(0, big, sizeof *big, 4096, 0);
        if (n <= 0)
            break;
        got += (uint32_t)n * 3u;
    }
    nv2a_dbg_publish();
    l = nv2a_dbg_take();
    CHECK(l && l->total <= NV2A_DBG_MAX_VTX && l->total > NV2A_DBG_MAX_VTX - 3);
    CHECK(l && l->dropped > 0);
    nv2a_dbg_reset();
}

#ifndef _WIN32
/* The producer publishes lists whose lines all carry their serial in x;
 * the consumer checks every list it takes is whole and newer or equal. */
#define ROUNDS 20000
static void *producer(void *arg)
{
    struct vtx v[2 * 64];

    (void)arg;
    for (uint32_t r = 1; r <= ROUNDS; r++) {
        uint32_t n = 1 + r % 64;

        for (uint32_t i = 0; i < 2 * n; i++) {
            v[i].p[0] = (float)r;
            v[i].p[1] = (float)n;
            v[i].p[2] = 0;
            v[i].argb = r;
        }
        nv2a_dbg_begin(I, I);
        nv2a_dbg_add(1, v, sizeof *v, n, 0);
        nv2a_dbg_publish();
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t th;
    uint32_t last = 0, bad = 0, takes = 0;

    nv2a_dbg_reset();
    pthread_create(&th, NULL, producer, NULL);
    while (last < ROUNDS) {
        const struct nv2a_dbg_list *l = nv2a_dbg_take();

        if (!l)
            continue;
        takes++;
        if (l->serial < last)
            bad++;
        last = l->serial;
        for (uint32_t i = 0; i < l->n[NV2A_DBG_LINE]; i++)
            if (l->v[NV2A_DBG_LINE][i].x != (float)l->serial
                || l->n[NV2A_DBG_LINE] != 2u * (uint32_t)l->v[NV2A_DBG_LINE][i].y) {
                bad++;
                break;
            }
    }
    pthread_join(th, NULL);
    CHECK(bad == 0);
    CHECK(takes > 0);
    nv2a_dbg_reset();
}
#endif

int main(void)
{
    test_transform();
    test_lists();
    test_limit();
#ifndef _WIN32
    test_threads();
#endif
    printf("dbg_draw: %d passed, %d failed\n", s_pass, s_fail);
    return s_fail ? 1 : 0;
}
