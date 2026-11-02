/*
 * nv2a_overlay.c - the overlay's layers: arming, the triple buffers, the
 * visible mask, placement, toasts and image slots (nv2a_overlay.h, R2,
 * R7).
 *
 * Every hand-over between threads is one atomic word: a layer's "ready"
 * slot index with its fresh bit, the visible mask, the placement packed
 * into 64 bits, the toast ring's head and each entry's sequence. Nothing
 * here takes a lock the presenting thread could wait on.
 */
#include "nv2a_overlay_int.h"
#include "nv2a_stats.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define FRESH 4u

/* Per-layer room. The menu's is the largest page of R4 with room to
 * spare; the strip and the toasts are a few dozen quads. Three indices a
 * vertex: a rounded rect's fan and feather ring take about 4.5, a glyph
 * quad 1.5. */
static const uint32_t s_cap_vtx[NV2A_OVL_COUNT] = {
    [NV2A_OVL_STATS] = 2048, [NV2A_OVL_STATUS] = 4096, [NV2A_OVL_TOAST] = 2048,
    [NV2A_OVL_MENU] = 16384, [NV2A_OVL_CURSOR] = 64,
};
static const uint32_t s_cap_cmd[NV2A_OVL_COUNT] = {
    [NV2A_OVL_STATS] = 16, [NV2A_OVL_STATUS] = 32, [NV2A_OVL_TOAST] = 16,
    [NV2A_OVL_MENU] = 256, [NV2A_OVL_CURSOR] = 2,
};
static const char *const s_layer_name[NV2A_OVL_COUNT] = {
    [NV2A_OVL_STATS] = "stats", [NV2A_OVL_STATUS] = "status", [NV2A_OVL_TOAST] = "toast",
    [NV2A_OVL_MENU] = "menu", [NV2A_OVL_CURSOR] = "cursor",
};

struct slot {
    struct nv2a_ovl_vtx *vtx;
    uint16_t *idx;
    struct nv2a_ovl_cmd *cmd;
    struct nv2a_ovl_list list;
};

struct layer {
    struct slot s[3];
    unsigned ready;                 /* atomic: slot | FRESH */
    unsigned back;                  /* the writer's */
    unsigned front;                 /* the reader's */
    struct nv2a_ovl_build build;
    mu_Context *ctx;
    uint64_t gen;
    uint32_t ui_w, ui_h;
    int armed;                      /* atomic: release once the slots exist */
    int cut_logged;
};

static struct layer s_layer[NV2A_OVL_COUNT];
static int s_armed;                 /* atomic */
static uint32_t s_visible;          /* atomic */
static uint64_t s_epoch;            /* atomic: moves at publishes and visibility */
static uint64_t s_place;            /* atomic: x, y, w, h as 16 bits each */
static const char *s_presenter;     /* atomic */
static int s_stats_on;              /* atomic */
static int s_menu_lock;             /* atomic */
static int s_published;             /* atomic: a layer published a list */
static void (*s_wake)(void);        /* atomic: the presenter's wake */

uint64_t nv2a_ovl_now_ns(void)
{
    struct timespec ts;
#if defined(_WIN32)
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* A layer can be armed after the presenter started (nv2a_overlay_set_stats,
 * 3C's menu): the acquire pairs with arm_layer's release, so a reader that
 * sees the layer armed also sees its slots. */
static int layer_armed(enum nv2a_overlay_layer l)
{
    return __atomic_load_n(&s_layer[l].armed, __ATOMIC_ACQUIRE);
}

static void wake(void)
{
    void (*fn)(void) = __atomic_load_n(&s_wake, __ATOMIC_ACQUIRE);
    if (fn)
        fn();
}

/* What the overlay draws moved: a presenter that redraws only on change
 * (the SDL loop, asleep up to 100 ms) is woken, so a menu edit shows
 * while the guest sits in a loading pause. */
static void bump(void)
{
    __atomic_add_fetch(&s_epoch, 1, __ATOMIC_RELEASE);
    wake();
}

static void show(enum nv2a_overlay_layer l, int on)
{
    uint32_t bit = 1u << l, old;
    old = on ? __atomic_fetch_or(&s_visible, bit, __ATOMIC_RELEASE)
             : __atomic_fetch_and(&s_visible, ~bit, __ATOMIC_RELEASE);
    if (!(old & bit) != !on)
        bump();
}

/* ── Arming ── */

static int arm_layer(enum nv2a_overlay_layer l)
{
    struct layer *y = &s_layer[l];
    uint32_t nv = s_cap_vtx[l], ni = nv * 3, nc = s_cap_cmd[l];
    int i;
    for (i = 0; i < 3; i++) {
        y->s[i].vtx = malloc(nv * sizeof *y->s[i].vtx);
        y->s[i].idx = malloc(ni * sizeof *y->s[i].idx);
        y->s[i].cmd = malloc(nc * sizeof *y->s[i].cmd);
        if (!y->s[i].vtx || !y->s[i].idx || !y->s[i].cmd)
            return -1;
        memset(&y->s[i].list, 0, sizeof y->s[i].list);
    }
    y->ctx = nv2a_ovl_ui_new();
    if (!y->ctx)
        return -1;
    y->back = 0;
    y->ready = 1;
    y->front = 2;
    y->build.cap_vtx = nv;
    y->build.cap_idx = ni;
    y->build.cap_cmd = nc;
    __atomic_store_n(&y->armed, 1, __ATOMIC_RELEASE);
    return 0;
}

static void free_layer(struct layer *y)
{
    int i;
    for (i = 0; i < 3; i++) {
        free(y->s[i].vtx);
        free(y->s[i].idx);
        free(y->s[i].cmd);
    }
    free(y->ctx);
    memset(y, 0, sizeof *y);
}

int nv2a_overlay_arm(uint32_t layers, uint32_t out_h)
{
    int l, added = 0, menu = (layers & (1u << NV2A_OVL_MENU)) != 0;
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) {
        if (nv2a_ovl_font_bake(out_h, menu) < 0)
            return -1;
    } else if (menu && !nv2a_ovl_font_symbols()) {
        /* enhance.c arms the strip and the toasts, then the menu, on the
         * init thread: nothing has drawn from the text-only atlas yet, so
         * it is baked again with the symbols. Once a list is out, a
         * presenter or writer may hold the old one, and the menu's
         * prompts write the controls' names instead. */
        if (__atomic_load_n(&s_published, __ATOMIC_ACQUIRE))
            fprintf(stderr, OVL_LOGP "menu: armed after a layer drew; prompts show names\n");
        else if (nv2a_ovl_font_bake(out_h, 1) < 0 && nv2a_ovl_font_bake(out_h, 0) < 0)
            return -1;
    }
    for (l = 0; l < NV2A_OVL_COUNT; l++) {
        if (!(layers & (1u << l)) || layer_armed((enum nv2a_overlay_layer)l))
            continue;
        if (arm_layer((enum nv2a_overlay_layer)l) < 0) {
            free_layer(&s_layer[l]);
            fprintf(stderr, OVL_LOGP "%s: out of memory; the layer stays off\n",
                    s_layer_name[l]);
            if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE) && !added)
                nv2a_ovl_font_free();
            return -1;
        }
        added++;
    }
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) {
        char names[64] = "";
        for (l = 0; l < NV2A_OVL_COUNT; l++)
            if (s_layer[l].armed) {
                strcat(names, " ");
                strcat(names, s_layer_name[l]);
            }
        fprintf(stderr, OVL_LOGP "armed:%s\n", names);
    }
    __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
    return 0;
}

int nv2a_overlay_armed(void)
{
    return __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
}

int nv2a_overlay_layer_armed(enum nv2a_overlay_layer l)
{
    return (unsigned)l < NV2A_OVL_COUNT && layer_armed(l);
}

void nv2a_overlay_set_stats(int on)
{
    if (!nv2a_overlay_armed())
        return;
    if (on && !layer_armed(NV2A_OVL_STATS) && nv2a_overlay_arm(1u << NV2A_OVL_STATS, 1080) < 0)
        return;
    __atomic_store_n(&s_stats_on, !!on, __ATOMIC_RELAXED);
    nv2a_stats_use(NV2A_STATS_STRIP, on);
    if (!on)
        show(NV2A_OVL_STATS, 0);
    else
        show(NV2A_OVL_STATS, 1);
}

/* Written by the window's title setters on the main thread before the
 * guest starts (the XBE load, the game's own title), read by the menu's
 * draw later: the last byte is never written, so the text always ends. */
static char s_game[128];

void nv2a_overlay_set_game(const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0;
    if (n >= sizeof s_game) {
        n = sizeof s_game - 1;
        /* Not inside a UTF-8 sequence: back to its lead byte. */
        while (n > 0 && ((unsigned char)utf8[n] & 0xC0) == 0x80)
            n--;
    }
    if (n)
        memcpy(s_game, utf8, n);
    s_game[n] = 0;
}

const char *nv2a_overlay_game(void)
{
    return s_game;
}

int nv2a_overlay_stats(void)
{
    return __atomic_load_n(&s_stats_on, __ATOMIC_RELAXED);
}

/* ── Placement, presenter ── */

void nv2a_overlay_set_wake(void (*fn)(void))
{
    __atomic_store_n(&s_wake, fn, __ATOMIC_RELEASE);
}

void nv2a_overlay_set_placement(int x, int y, int w, int h)
{
    uint64_t v = (uint64_t)(uint16_t)x | (uint64_t)(uint16_t)y << 16
               | (uint64_t)(uint16_t)w << 32 | (uint64_t)(uint16_t)h << 48;
    __atomic_store_n(&s_place, v, __ATOMIC_RELAXED);
}

void nv2a_overlay_placement(int *x, int *y, int *w, int *h)
{
    uint64_t v = __atomic_load_n(&s_place, __ATOMIC_RELAXED);
    *x = (int16_t)(v & 0xffff);
    *y = (int16_t)((v >> 16) & 0xffff);
    *w = (int)((v >> 32) & 0xffff);
    *h = (int)((v >> 48) & 0xffff);
}

void nv2a_overlay_set_presenter(const char *name)
{
    __atomic_store_n(&s_presenter, name, __ATOMIC_RELAXED);
}

const char *nv2a_overlay_presenter(void)
{
    return __atomic_load_n(&s_presenter, __ATOMIC_RELAXED);
}

/* ── Writer side ── */

struct nv2a_ovl_build *nv2a_overlay_build_begin(enum nv2a_overlay_layer l,
                                                uint32_t out_w, uint32_t out_h)
{
    struct layer *y = &s_layer[l];
    struct slot *s;
    if (!layer_armed(l))
        return NULL;
    s = &y->s[y->back];
    y->build.vtx = s->vtx;
    y->build.idx = s->idx;
    y->build.cmd = s->cmd;
    y->build.nvtx = y->build.nidx = y->build.ncmd = 0;
    y->build.out_w = out_w;
    y->build.out_h = out_h;
    y->build.cut = 0;
    return &y->build;
}

void nv2a_overlay_build_publish(enum nv2a_overlay_layer l, uint32_t sum)
{
    struct layer *y = &s_layer[l];
    struct slot *s;
    const struct nv2a_ovl_atlas *a;
    if (!layer_armed(l))
        return;
    s = &y->s[y->back];
    a = nv2a_ovl_font_atlas();
    s->list.vtx = s->vtx;
    s->list.idx = s->idx;
    s->list.cmd = s->cmd;
    s->list.nvtx = y->build.nvtx;
    s->list.nidx = y->build.nidx;
    s->list.ncmd = y->build.ncmd;
    s->list.out_w = y->build.out_w;
    s->list.out_h = y->build.out_h;
    s->list.layer = (uint32_t)l;
    s->list.gen = ++y->gen;
    s->list.atlas_gen = a ? a->gen : 0;
    s->list.sum = sum;
    __atomic_store_n(&s_published, 1, __ATOMIC_RELEASE);
    if (y->build.cut && !y->cut_logged) {
        y->cut_logged = 1;
        fprintf(stderr, OVL_LOGP "%s: the page did not fit (%u vertices, %u commands);"
                " the rest is cut\n", s_layer_name[l], y->build.cap_vtx, y->build.cap_cmd);
    }
    y->back = __atomic_exchange_n(&y->ready, y->back | FRESH, __ATOMIC_ACQ_REL) & 3u;
    show(l, 1);
    bump();
}

mu_Context *nv2a_overlay_ui_begin(enum nv2a_overlay_layer l)
{
    struct layer *y = &s_layer[l];
    int px, py, w, h;
    mu_Rect r;
    if (!layer_armed(l))
        return NULL;
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (w <= 0 || h <= 0)
        w = 1920, h = 1080;   /* before the first present: the bake's size */
    y->ui_w = (uint32_t)w;
    y->ui_h = (uint32_t)h;
    r = mu_rect(0, 0, w, h);
    mu_begin(y->ctx);
    /* One window over the whole placement: microui needs a root
     * container, and every page draws in output pixels inside it. */
    mu_get_container(y->ctx, "ovl")->rect = r;
    mu_begin_window_ex(y->ctx, "ovl", r,
                       MU_OPT_NOFRAME | MU_OPT_NOTITLE | MU_OPT_NORESIZE | MU_OPT_NOSCROLL
                           | MU_OPT_NOINTERACT | MU_OPT_NOCLOSE);
    return y->ctx;
}

void nv2a_overlay_ui_publish(enum nv2a_overlay_layer l)
{
    struct layer *y = &s_layer[l];
    struct nv2a_ovl_build *b;
    if (!layer_armed(l))
        return;
    mu_end_window(y->ctx);
    mu_end(y->ctx);
    b = nv2a_overlay_build_begin(l, y->ui_w, y->ui_h);
    nv2a_ovl_convert(y->ctx, b);
    nv2a_overlay_build_publish(l, nv2a_overlay_list_sum(b->vtx, b->nvtx, b->idx, b->nidx,
                                                        b->cmd, b->ncmd));
}

void nv2a_overlay_hide(enum nv2a_overlay_layer l)
{
    show(l, 0);
}

int nv2a_overlay_menu_trylock(void)
{
    return !__atomic_exchange_n(&s_menu_lock, 1, __ATOMIC_ACQUIRE);
}

void nv2a_overlay_menu_unlock(void)
{
    __atomic_store_n(&s_menu_lock, 0, __ATOMIC_RELEASE);
}

mu_Context *nv2a_ovl_ctx(enum nv2a_overlay_layer l)
{
    return s_layer[l].ctx;
}

/* ── Reader side ── */

uint32_t nv2a_overlay_visible(void)
{
    return __atomic_load_n(&s_visible, __ATOMIC_RELAXED);
}

const struct nv2a_ovl_list *nv2a_overlay_acquire(enum nv2a_overlay_layer l)
{
    struct layer *y = &s_layer[l];
    if (!(nv2a_overlay_visible() & (1u << l)) || !layer_armed(l))
        return NULL;
    if (__atomic_load_n(&y->ready, __ATOMIC_ACQUIRE) & FRESH)
        y->front = __atomic_exchange_n(&y->ready, y->front, __ATOMIC_ACQ_REL) & 3u;
    return y->s[y->front].list.gen ? &y->s[y->front].list : NULL;
}

int nv2a_overlay_lists(const struct nv2a_ovl_list *out[NV2A_OVL_COUNT])
{
    int l, n = 0;
    for (l = 0; l < NV2A_OVL_COUNT; l++) {
        const struct nv2a_ovl_list *p = nv2a_overlay_acquire((enum nv2a_overlay_layer)l);
        if (p && p->nidx)
            out[n++] = p;
    }
    return n;
}

const struct nv2a_ovl_atlas *nv2a_overlay_atlas(void)
{
    return nv2a_ovl_font_atlas();
}

/* ── Toasts ── */

#define TOASTS 8
#define TOAST_LEN 96
#define TOAST_SHOWN 3
#define TOAST_MS 2000u

static struct { unsigned seq; char text[TOAST_LEN]; } s_toast[TOASTS];
static unsigned s_toast_head;       /* atomic: pushes so far */

/* The presenting thread's: what it has drained and what it shows. */
static unsigned s_toast_read;
static struct { char text[TOAST_LEN]; uint64_t t0; } s_shown[TOAST_SHOWN];
static int s_nshown;
static void (*s_toast_hook)(const char *line);   /* the ctest's */
static void (*s_toast_mid)(void);                /* the ctest's */

void nv2a_overlay_toast(const char *fmt, ...)
{
    char line[TOAST_LEN];
    va_list ap;
    unsigned n, k;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fprintf(stderr, "[TOAST] %s\n", line);
    if (!nv2a_overlay_armed() || !layer_armed(NV2A_OVL_TOAST))
        return;
    /* A seqlock per entry: odd while written, so a reader that sees the
     * sequence move under its copy drops the entry instead of a torn line. */
    n = __atomic_fetch_add(&s_toast_head, 1, __ATOMIC_RELAXED);
    k = n % TOASTS;
    if (s_toast_mid)
        s_toast_mid();
    __atomic_store_n(&s_toast[k].seq, 2 * n + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    memcpy(s_toast[k].text, line, sizeof line);
    __atomic_store_n(&s_toast[k].seq, 2 * n + 2, __ATOMIC_RELEASE);
    show(NV2A_OVL_TOAST, 1);
    wake();                         /* already visible: the drain is due */
}

/* New toasts into the shown stack; returns 1 when it changed. */
static int toast_drain(uint64_t now_ms)
{
    unsigned head = __atomic_load_n(&s_toast_head, __ATOMIC_ACQUIRE);
    int changed = 0, i;
    if (head - s_toast_read > TOASTS)
        s_toast_read = head - TOASTS;
    for (; s_toast_read != head; s_toast_read++) {
        unsigned n = s_toast_read, k = n % TOASTS, seq;
        int d;
        char text[TOAST_LEN];
        seq = __atomic_load_n(&s_toast[k].seq, __ATOMIC_ACQUIRE);
        /* The head moves before the entry is written: an entry behind its
         * sequence is still being written (or not begun), so stop and take
         * it at the next poll; one ahead was lapped by a newer push. */
        d = (int)(seq - (2 * n + 2));
        if (d < 0)
            break;
        if (d > 0)
            continue;
        memcpy(text, s_toast[k].text, sizeof text);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        /* The sequence only grows: a change under the copy is a lap. */
        if (__atomic_load_n(&s_toast[k].seq, __ATOMIC_RELAXED) != seq)
            continue;
        text[TOAST_LEN - 1] = 0;
        if (s_toast_hook)
            s_toast_hook(text);
        if (s_nshown == TOAST_SHOWN) {
            memmove(&s_shown[0], &s_shown[1], sizeof s_shown[0] * (TOAST_SHOWN - 1));
            s_nshown--;
        }
        memcpy(s_shown[s_nshown].text, text, sizeof text);
        s_shown[s_nshown++].t0 = now_ms;
        changed = 1;
    }
    for (i = 0; i < s_nshown;) {
        if (now_ms - s_shown[i].t0 >= TOAST_MS) {
            memmove(&s_shown[i], &s_shown[i + 1], sizeof s_shown[0] * (size_t)(s_nshown - i - 1));
            s_nshown--;
            changed = 1;
        } else {
            i++;
        }
    }
    return changed;
}

/* ── Image slots ── */

static struct img {
    uint32_t *px[3];
    uint32_t cap;                   /* texels per buffer */
    struct nv2a_ovl_image im[3];
    unsigned ready, back, front;
    uint64_t gen;
} s_img[NV2A_OVL_IMAGES];

int nv2a_overlay_image_set(uint16_t tex, const uint32_t *rgba, int w, int h)
{
    struct img *m;
    uint32_t n = (uint32_t)w * (uint32_t)h;
    int i;
    if (!nv2a_overlay_armed() || tex < 1 || tex > NV2A_OVL_IMAGES || w <= 0 || h <= 0)
        return -1;
    m = &s_img[tex - 1];
    if (!m->cap) {
        for (i = 0; i < 3; i++) {
            m->px[i] = malloc((size_t)n * 4);
            if (!m->px[i]) {
                while (i--)
                    free(m->px[i]);
                return -1;
            }
        }
        m->cap = n;
        m->back = 0;
        m->ready = 1;
        m->front = 2;
    } else if (n > m->cap) {
        fprintf(stderr, OVL_LOGP "image %u: %dx%d is larger than the slot; refused\n", tex, w, h);
        return -1;
    }
    memcpy(m->px[m->back], rgba, (size_t)n * 4);
    m->im[m->back].px = m->px[m->back];
    m->im[m->back].w = (uint32_t)w;
    m->im[m->back].h = (uint32_t)h;
    m->im[m->back].gen = ++m->gen;
    m->back = __atomic_exchange_n(&m->ready, m->back | FRESH, __ATOMIC_ACQ_REL) & 3u;
    bump();
    return 0;
}

const struct nv2a_ovl_image *nv2a_overlay_image(uint16_t tex)
{
    struct img *m;
    if (tex < 1 || tex > NV2A_OVL_IMAGES)
        return NULL;
    m = &s_img[tex - 1];
    if (!__atomic_load_n(&m->cap, __ATOMIC_RELAXED))
        return NULL;
    if (__atomic_load_n(&m->ready, __ATOMIC_ACQUIRE) & FRESH)
        m->front = __atomic_exchange_n(&m->ready, m->front, __ATOMIC_ACQ_REL) & 3u;
    return m->im[m->front].gen ? &m->im[m->front] : NULL;
}

/* ── The presenting thread's tick ── */

static uint64_t s_stats_t;          /* when the strip was last composed */
static uint32_t s_stats_w, s_stats_h, s_toast_w, s_toast_h;

uint64_t nv2a_overlay_poll(uint64_t now_ms)
{
    uint32_t vis = __atomic_load_n(&s_visible, __ATOMIC_ACQUIRE);
    int px, py, w, h;
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (vis && nv2a_overlay_armed() && w > 0 && h > 0) {
        /* The strip at 2 Hz, and at once after a resize. */
        if ((vis & (1u << NV2A_OVL_STATS)) && layer_armed(NV2A_OVL_STATS)
            && (now_ms - s_stats_t >= 500 || !s_stats_t || (uint32_t)w != s_stats_w
                || (uint32_t)h != s_stats_h)) {
            char line[160];
            int spike;
            mu_Context *ctx;
            const char *pres = __atomic_load_n(&s_presenter, __ATOMIC_RELAXED);
            nv2a_ovl_stats_line(line, sizeof line, pres, &spike);
            ctx = nv2a_overlay_ui_begin(NV2A_OVL_STATS);
            nv2a_ovl_compose_stats(ctx, s_layer[NV2A_OVL_STATS].ui_w,
                                   s_layer[NV2A_OVL_STATS].ui_h, line, spike);
            nv2a_overlay_ui_publish(NV2A_OVL_STATS);
            s_stats_t = now_ms ? now_ms : 1;
            s_stats_w = (uint32_t)w;
            s_stats_h = (uint32_t)h;
        }
        if ((vis & (1u << NV2A_OVL_TOAST)) && layer_armed(NV2A_OVL_TOAST)) {
            int changed = toast_drain(now_ms);
            if ((uint32_t)w != s_toast_w || (uint32_t)h != s_toast_h)
                changed = 1;
            if (!s_nshown && s_toast_read == __atomic_load_n(&s_toast_head, __ATOMIC_ACQUIRE)) {
                show(NV2A_OVL_TOAST, 0);
                /* A push between the drain and the hide shows again. */
                if (__atomic_load_n(&s_toast_head, __ATOMIC_ACQUIRE) != s_toast_read)
                    show(NV2A_OVL_TOAST, 1);
            } else if (changed) {
                const char *lines[TOAST_SHOWN];
                int i;
                mu_Context *ctx;
                for (i = 0; i < s_nshown; i++)
                    lines[i] = s_shown[i].text;
                ctx = nv2a_overlay_ui_begin(NV2A_OVL_TOAST);
                nv2a_ovl_compose_toasts(ctx, s_layer[NV2A_OVL_TOAST].ui_w,
                                        s_layer[NV2A_OVL_TOAST].ui_h, lines, s_nshown);
                nv2a_overlay_ui_publish(NV2A_OVL_TOAST);
                s_toast_w = (uint32_t)w;
                s_toast_h = (uint32_t)h;
            }
        }
    }
    return __atomic_load_n(&s_epoch, __ATOMIC_ACQUIRE)
           ^ ((uint64_t)__atomic_load_n(&s_visible, __ATOMIC_RELAXED) << 56);
}

/* ── Checksum, test reset ── */

static uint32_t fnv(uint32_t h, const void *p, size_t n)
{
    const unsigned char *b = p;
    while (n--) {
        h ^= *b++;
        h *= 16777619u;
    }
    return h;
}

static int32_t q16(float v)
{
    return (int32_t)(v * 16.0f + (v < 0 ? -0.5f : 0.5f));
}

uint32_t nv2a_overlay_list_sum(const struct nv2a_ovl_vtx *vtx, uint32_t nvtx,
                               const uint16_t *idx, uint32_t nidx,
                               const struct nv2a_ovl_cmd *cmd, uint32_t ncmd)
{
    uint32_t h = 2166136261u, i;
    for (i = 0; i < nvtx; i++) {
        /* Positions and UVs at 1/16 px (UVs at 1/16 of a 4096 texel span),
         * so a last-bit float difference between compilers does not move
         * the sum. */
        int32_t q[5];
        q[0] = q16(vtx[i].x);
        q[1] = q16(vtx[i].y);
        q[2] = q16(vtx[i].u * 4096.0f);
        q[3] = q16(vtx[i].v * 4096.0f);
        q[4] = (int32_t)vtx[i].rgba;
        h = fnv(h, q, sizeof q);
    }
    h = fnv(h, idx, nidx * sizeof *idx);
    for (i = 0; i < ncmd; i++) {
        uint32_t c[5];
        c[0] = cmd[i].first;
        c[1] = cmd[i].count;
        c[2] = (uint32_t)(uint16_t)cmd[i].clip[0] | (uint32_t)(uint16_t)cmd[i].clip[1] << 16;
        c[3] = (uint32_t)(uint16_t)cmd[i].clip[2] | (uint32_t)(uint16_t)cmd[i].clip[3] << 16;
        c[4] = cmd[i].tex;
        h = fnv(h, c, sizeof c);
    }
    return h;
}

void nv2a_overlay_reset_for_test(void)
{
    int l, i;
    for (l = 0; l < NV2A_OVL_COUNT; l++)
        free_layer(&s_layer[l]);
    for (l = 0; l < NV2A_OVL_IMAGES; l++) {
        for (i = 0; i < 3; i++)
            free(s_img[l].px[i]);
        memset(&s_img[l], 0, sizeof s_img[l]);
    }
    nv2a_ovl_font_free();
    s_armed = 0;
    s_visible = 0;
    s_epoch = 0;
    s_place = 0;
    s_presenter = NULL;
    s_stats_on = 0;
    s_menu_lock = 0;
    s_published = 0;
    s_game[0] = 0;
    nv2a_overlay_set_ui_scale(1.0f);
    s_wake = NULL;
    s_toast_head = 0;
    s_toast_read = 0;
    s_nshown = 0;
    s_toast_hook = NULL;
    s_toast_mid = NULL;
    memset(s_toast, 0, sizeof s_toast);
    s_stats_t = 0;
    s_stats_w = s_stats_h = s_toast_w = s_toast_h = 0;
}

void nv2a_overlay_toast_hook_for_test(void (*seen)(const char *line), void (*mid_push)(void))
{
    s_toast_hook = seen;
    s_toast_mid = mid_push;
}
