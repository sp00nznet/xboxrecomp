/*
 * hud_capture.c - the HUD mapping capture (hud_capture.h; openspec change
 * hud-anchors, design A15).
 *
 * Lines are formatted on the guest thread that records them and appended
 * to one buffer under a spin lock. At each xbox_mod_frame the buffer is
 * sealed into the frame just ended, and the oldest frames whose pictures
 * are settled (taken, failed, or passed) are written out in order: their
 * sprite lines, then the frame line. So the file never names a frame
 * before that frame's lines and BMP are complete.
 */
#include "hud_capture.h"

#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <time.h>
#endif

#include "nv2a_backend_common.h"
#include "recomp_env.h"

#define LOGP "[hud.capture] "

#define CAP_BUF_MAX   (8u << 20)    /* bytes of lines held at once */
#define CAP_PEND_MAX  64u           /* frames waiting for their picture */
#define CAP_IDLE_MS   200           /* the wait for the walker at a captured frame */
#define CAP_TIMEOUT   120u          /* frames a picture is waited for */
#define CAP_END_MS    2000          /* shutdown's wait for the pictures */
#define CAP_SCAN      96u           /* stack words the level-0 scan reads */
#define CAP_COUNT_MAX 10000u
#define CAP_LINE      2048u
/* Guest reads past this are not made: retail RAM, where the stack, the
 * code and the vertices live (xbox_memory_layout.h). */
#define CAP_VA_TOP    (64u * 1024u * 1024u)
#define CAP_LOCK_MS   1000          /* shutdown's wait for the guest's frame */
#define CAP_WARN_HIGH 1
#define CAP_WARN_KERN 2

struct pend {
    uint32_t frame;
    int slot;                       /* the picture's request, -1: none */
    int has_flip, has_cv, sealed;
    uint32_t flip, posted;
    struct hud_layer_canvas cv;
    char *lines;
    size_t len;
};

static int s_on;
static char s_dir[NV2A_CAP_PATH - 32];
static uint32_t s_from = 1, s_count = 1;
static const uintptr_t *s_memp;
static FILE *s_out;
static int s_finished;              /* atomic; set under s_flock */
/* atomic: held by hud_capture_frame and hud_capture_end, so a shutdown on
 * another thread (the exit hook, atexit) never meets the guest's frame in
 * the pending queue or the file. */
static int s_flock;
static int s_warned;                /* atomic: one-time lines, CAP_WARN_* bits */
static uint32_t s_tick;             /* xbox_mod_frame calls so far */
static uint32_t s_rec;              /* atomic: the frame being recorded, 0 none */

static int s_lock;                  /* atomic: the buffer's */
static char *s_buf;
static size_t s_len, s_cap;
static uint32_t s_dropped;          /* atomic: lines that did not fit */
static uint32_t s_sprites;          /* atomic */

static struct pend s_pend[CAP_PEND_MAX];
static unsigned s_npend;
static uint32_t s_frames_out, s_bmps_out;

/* ---- small things ---------------------------------------------------------- */

static void nap_ms(int ms)
{
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
#endif
}

static uint8_t *mem_base(void)
{
    return s_memp ? (uint8_t *)__atomic_load_n(s_memp, __ATOMIC_ACQUIRE) : NULL;
}

static int va_ok(uint32_t va, uint32_t len)
{
    return va < CAP_VA_TOP && len <= CAP_VA_TOP - va;
}

static uint32_t rd32(const uint8_t *b, uint32_t va)
{
    uint32_t v;
    memcpy(&v, b + va, 4);
    return v;
}

int hud_capture_is_call_ret(const uint8_t *b, uint32_t r)
{
    /* CALL rel32 (E8), CALL [abs32] (FF 15), CALL reg (FF D0+r) and
     * CALL [reg+d8] (FF 50+r): what MSVC emits for a direct, an import, a
     * register and a vtable call. */
    if (r < 6 || !va_ok(r - 6, 6))
        return 0;
    return b[r - 5] == 0xE8 || (b[r - 6] == 0xFF && b[r - 5] == 0x15)
           || (b[r - 2] == 0xFF && (b[r - 1] & 0xF8) == 0xD0)
           || (b[r - 3] == 0xFF && (b[r - 2] & 0xF8) == 0x50);
}

/* A number as JSON in the C locale, whatever the process's is. JSON has
 * no inf or nan: those are null. */
static int jnum(char *o, size_t n, double v)
{
    int k = isfinite(v) ? snprintf(o, n, "%.7g", v) : snprintf(o, n, "null");
    for (int i = 0; i < k && (size_t)i < n; i++)
        if (o[i] == ',')
            o[i] = '.';
    return k;
}

/* A JSON string, or null. */
static size_t jstr(char *o, size_t n, const char *s)
{
    size_t k = 0;
    if (!s)
        return (size_t)snprintf(o, n, "null");
    if (n < 3)
        return n;
    o[k++] = '"';
    for (; *s && k + 8 < n; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') {
            o[k++] = '\\';
            o[k++] = (char)c;
        } else if (c < 0x20) {
            k += (size_t)snprintf(o + k, n - k, "\\u%04x", c);
        } else {
            o[k++] = (char)c;
        }
    }
    o[k++] = '"';
    o[k] = 0;
    return k;
}

/* Appends to a line, keeping count; past the end it stops writing. */
struct line {
    char b[CAP_LINE];
    size_t k;
};

static void put(struct line *l, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(struct line *l, const char *fmt, ...)
{
    va_list ap;
    int r;
    if (l->k >= sizeof l->b)
        return;
    va_start(ap, fmt);
    r = vsnprintf(l->b + l->k, sizeof l->b - l->k, fmt, ap);
    va_end(ap);
    l->k = r < 0 ? sizeof l->b : l->k + (size_t)r;
}

static void put_num(struct line *l, double v)
{
    char t[32];
    jnum(t, sizeof t, v);
    put(l, "%s", t);
}

static void put_str(struct line *l, const char *s)
{
    char t[2 * 64 + 16];
    jstr(t, sizeof t, s);
    put(l, "%s", t);
}

static void put_va(struct line *l, uint32_t va)
{
    put(l, "\"0x%08X\"", (unsigned)va);
}

/* ---- the buffer ------------------------------------------------------------ */

static void lock(void)
{
    while (__atomic_exchange_n(&s_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void unlock(void)
{
    __atomic_store_n(&s_lock, 0, __ATOMIC_RELEASE);
}

static int warn_once(int bit)
{
    return !(__atomic_fetch_or(&s_warned, bit, __ATOMIC_RELAXED) & bit);
}

/* A line of frame `f`. Checked again under the lock: a call on another
 * guest thread that began in f and ends after f was sealed is dropped,
 * never written into a later frame's block. The buffer grows outside the
 * lock (the allocator can take a lock of its own or page in), and only
 * the copy into the larger buffer is made under it. */
static void emit(const struct line *l, uint32_t f)
{
    size_t n = l->k;
    if (n >= sizeof l->b) {
        __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
        return;
    }
    for (;;) {
        size_t need, nc;
        char *nb, *old = NULL;

        lock();
        if (__atomic_load_n(&s_rec, __ATOMIC_ACQUIRE) != f) {
            unlock();
            __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
            return;
        }
        need = s_len + n + 1;
        if (need <= s_cap) {
            memcpy(s_buf + s_len, l->b, n);
            s_buf[s_len + n] = '\n';
            s_len += n + 1;
            unlock();
            __atomic_add_fetch(&s_sprites, 1u, __ATOMIC_RELAXED);
            return;
        }
        nc = s_cap ? s_cap * 2 : 65536;
        unlock();
        while (nc < need)
            nc *= 2;
        if (nc > CAP_BUF_MAX || !(nb = malloc(nc))) {
            __atomic_add_fetch(&s_dropped, 1u, __ATOMIC_RELAXED);
            return;
        }
        lock();
        /* Another thread may have grown it, or the frame sealed and taken
         * it, meanwhile: install this one only if it still holds more. */
        if (nc > s_cap && nc >= s_len + n + 1) {
            if (s_len)
                memcpy(nb, s_buf, s_len);
            old = s_buf;
            s_buf = nb;
            s_cap = nc;
            nb = NULL;
        }
        unlock();
        free(old);
        free(nb);
    }
}

/* The lines so far, handed over (the caller frees them). */
static char *take_lines(size_t *len)
{
    char *b;
    lock();
    b = s_buf;
    *len = s_len;
    s_buf = NULL;
    s_len = s_cap = 0;
    unlock();
    return b;
}

/* ---- the canvas map --------------------------------------------------------- */

/* Guest pixels of the target back to canvas units: the inverse of the
 * layer's map (design A11), which the reader applies forwards. */
static void px_to_units(const struct hud_layer_canvas *cv, const float px[4], float out[4])
{
    float sx = cv->view[2] / cv->w, sy = cv->view[3] / cv->h;
    float ox = cv->top_left ? 0.0f : cv->w * 0.5f, oy = cv->top_left ? 0.0f : cv->h * 0.5f;
    float x0 = (px[0] - cv->view[0]) / sx - ox, x1 = (px[2] - cv->view[0]) / sx - ox;
    float y0 = (px[1] - cv->view[1]) / sy - oy, y1 = (px[3] - cv->view[1]) / sy - oy;

    if (cv->y_up) {
        float t = -y0;
        y0 = -y1;
        y1 = t;
    }
    out[0] = x0 < x1 ? x0 : x1;
    out[2] = x0 < x1 ? x1 : x0;
    out[1] = y0 < y1 ? y0 : y1;
    out[3] = y0 < y1 ? y1 : y0;
}

/* ---- level 1: hud_apply's calls --------------------------------------------- */

static void put_head(struct line *l, int level, uint32_t frame)
{
    put(l, "{\"type\":\"sprite\",\"level\":%d,\"frame\":%u", level, (unsigned)frame);
}

static void put_names(struct line *l, uint32_t site, const char *el, const char *grp)
{
    put(l, ",\"site\":");
    if (site)
        put_va(l, site);
    else
        put(l, "null");
    put(l, ",\"element\":");
    put_str(l, el);
    put(l, ",\"group\":");
    put_str(l, grp);
}

static void put_bbox(struct line *l, int has, const float bb[4])
{
    put(l, ",\"bbox\":");
    /* A huge finite vertex can overflow to inf in the canvas map: the
     * whole box is null then, never a half-null array. */
    for (int k = 0; has && k < 4; k++)
        if (!isfinite(bb[k]))
            has = 0;
    if (!has) {
        put(l, "null");
        return;
    }
    put(l, "[");
    for (int k = 0; k < 4; k++) {
        if (k)
            put(l, ",");
        put_num(l, bb[k]);
    }
    put(l, "]");
}

static void on_record(const struct hud_record *r)
{
    uint32_t f = __atomic_load_n(&s_rec, __ATOMIC_ACQUIRE);
    struct line l;

    if (!f)
        return;
    l.k = 0;
    put_head(&l, 1, f);
    put(&l, ",\"queue\":%u,\"chain\":[", (unsigned)r->queue);
    for (unsigned i = 0; i < r->nchain; i++) {
        if (i)
            put(&l, ",");
        put_va(&l, r->chain[i]);
    }
    put(&l, "],\"ret\":");
    put_va(&l, r->ret);
    put_names(&l, r->site, r->element, r->group);
    put(&l, ",\"n\":%u", (unsigned)r->n);
    put_bbox(&l, r->has_bbox, r->bbox);
    put(&l, ",\"prims\":%u,\"tex\":", (unsigned)r->prims);
    if (r->tex)
        put(&l, "\"0x%016llX\"", (unsigned long long)r->tex);
    else
        put(&l, "null");
    put(&l, "}");
    emit(&l, f);
}

/* ---- level 0: the draw entries ------------------------------------------------ */

static int loc_read(const uint8_t *b, const struct xbox_mod_call *c, const struct hud_loc *lc,
                    uint32_t *out)
{
    switch (lc->kind) {
    case HUD_LOC_ECX:
        *out = c->ecx;
        return 1;
    case HUD_LOC_EDX:
        *out = c->edx;
        return 1;
    case HUD_LOC_CONST:
        *out = lc->v;
        return 1;
    case HUD_LOC_STACK:
        if (!va_ok(c->esp + 4u + lc->v, 4))
            return 0;
        *out = rd32(b, c->esp + 4u + lc->v);
        return 1;
    default:
        return 0;
    }
}

static void draw_pre(struct xbox_mod_call *c, void *user)
{
    const struct hud_draw *dr = user;
    const struct nv2a_cap_kernel *k;
    uint32_t f = __atomic_load_n(&s_rec, __ATOMIC_ACQUIRE);
    uint32_t hits[HUD_CHAIN_MAX], lo = 0, hi = 0, ret, site = 0, vtx, n = 0, stride, prim;
    const char *el = NULL, *grp = NULL;
    unsigned nh = 0, i;
    struct hud_layer_canvas cv;
    float px[4], bb[4];
    int has_bb = 0, got;
    struct line l;
    uint8_t *b;

    if (!f || !(b = mem_base()) || !va_ok(c->esp, 4u * CAP_SCAN))
        return;
    if ((k = nv2a_cap_kernel()) && k->code_range)
        k->code_range(&lo, &hi);
    ret = rd32(b, c->esp);
    /* The callers above the draw: words on the stack that point just past
     * a call in the title's code, innermost first as they are found. A
     * stale word can pass; the reader names sprites by the outermost
     * listed one, so a false hit only adds a candidate. */
    for (i = 1; i < CAP_SCAN && nh < HUD_CHAIN_MAX && hi > lo; i++) {
        uint32_t v = rd32(b, c->esp + 4u * i);
        if (v >= lo && v < hi && hud_capture_is_call_ret(b, v))
            hits[nh++] = v;
    }
    for (i = nh; i-- > 0 && !site;)
        if (hud_layer_site(hits[i], &el, &grp))
            site = hits[i];
    if (!site && hud_layer_site(ret, &el, &grp))
        site = ret;
    hud_layer_layout(&stride, NULL, &prim);
    prim = dr->prim ? dr->prim : prim;
    if (dr->stride.kind != HUD_LOC_NONE && !loc_read(b, c, &dr->stride, &stride))
        stride = 0;
    got = dr->vtx.kind != HUD_LOC_NONE && loc_read(b, c, &dr->vtx, &vtx)
          && loc_read(b, c, &dr->count, &n) && n && n <= 65536u && stride >= 8u
          && stride <= 256u;
    if (got && !va_ok(vtx, (n < 4096u ? n : 4096u) * stride)) {
        /* va_ok bounds every read to retail RAM, the guest map's size. */
        if (warn_once(CAP_WARN_HIGH))
            fprintf(stderr, LOGP "draw %s: vertices at 0x%08X lie past retail RAM (64 MB):"
                    " not read, bbox null (said once)\n", dr->spec, (unsigned)vtx);
    } else if (got && hud_layer_bbox(vtx, n, stride, 0, px) && hud_layer_canvas(&cv)
               && cv.w > 0.0f && cv.h > 0.0f && cv.view[2] > 0.0f && cv.view[3] > 0.0f) {
        /* Pretransformed (x, y, z, rhw): guest pixels, back to the
         * canvas's units so both levels read alike. */
        px_to_units(&cv, px, bb);
        has_bb = 1;
    }

    l.k = 0;
    put_head(&l, 0, f);
    put(&l, ",\"queue\":null,\"draw\":");
    put_va(&l, dr->va);
    put(&l, ",\"name\":");
    put_str(&l, dr->name[0] ? dr->name : NULL);
    put(&l, ",\"chain\":[");
    for (i = nh; i-- > 0;) {
        put_va(&l, hits[i]);
        if (i)
            put(&l, ",");
    }
    put(&l, "],\"ret\":");
    put_va(&l, ret);
    put_names(&l, site, el, grp);
    put(&l, ",\"n\":%u", (unsigned)n);
    put_bbox(&l, has_bb, bb);
    put(&l, ",\"prims\":%u,\"tex\":null}", (unsigned)(prim ? n / prim : 0));
    emit(&l, f);
}

/* ---- frames and their pictures -------------------------------------------- */

static void out_open(void)
{
    char path[NV2A_CAP_PATH];

    snprintf(path, sizeof path, "%s/capture.jsonl", s_dir);
    if (!(s_out = fopen(path, "wb"))) {
#ifdef _WIN32
        _mkdir(s_dir);
#else
        mkdir(s_dir, 0755);
#endif
        s_out = fopen(path, "wb");
    }
    if (!s_out)
        fprintf(stderr, LOGP "cannot write %s: off\n", path);
}

static void frame_line(const struct pend *p, int bmp_ok)
{
    struct line l;
    l.k = 0;
    put(&l, "{\"type\":\"frame\",\"frame\":%u,\"flip\":", (unsigned)p->frame);
    if (p->has_flip)
        put(&l, "%u", (unsigned)p->flip);
    else
        put(&l, "null");
    put(&l, ",\"canvas\":");
    if (p->has_cv) {
        const struct hud_layer_canvas *c = &p->cv;
        put(&l, "{\"w\":");
        put_num(&l, c->w);
        put(&l, ",\"h\":");
        put_num(&l, c->h);
        put(&l, ",\"origin\":\"%s\",\"y\":\"%s\",\"view\":[", c->top_left ? "top-left" : "centre",
            c->y_up ? "up" : "down");
        for (int k = 0; k < 4; k++) {
            if (k)
                put(&l, ",");
            put_num(&l, c->view[k]);
        }
        put(&l, "],\"target\":[%u,%u],\"pad\":%u}", (unsigned)c->tw, (unsigned)c->th,
            (unsigned)c->pad);
    } else {
        put(&l, "null");
    }
    if (bmp_ok)
        put(&l, ",\"bmp\":\"frame_%05u.bmp\"}", (unsigned)p->frame);
    else
        put(&l, ",\"bmp\":null}");
    if (s_out && l.k < sizeof l.b)
        fprintf(s_out, "%s\n", l.b);
}

static void pend_write(struct pend *p, int bmp_ok)
{
    if (s_out && p->len)
        fwrite(p->lines, 1, p->len, s_out);
    frame_line(p, bmp_ok);
    s_frames_out++;
    s_bmps_out += bmp_ok != 0;
    free(p->lines);
    p->lines = NULL;
}

/* The oldest frames, while settled: `force` retracts what is still asked
 * for (shutdown, or the queue full). */
static void pend_drain(int force)
{
    unsigned done = 0;

    while (done < s_npend) {
        struct pend *p = &s_pend[done];
        int st = NV2A_CAP_DEAD;
        if (!p->sealed && !force)
            break;
        if (p->slot >= 0) {
            int late = s_tick - p->posted > CAP_TIMEOUT;
            st = nv2a_cap_poll(p->slot, force || late);
            if (st < 0)
                break;              /* still asked for, or being written */
            if (st == NV2A_CAP_FAILED)
                fprintf(stderr, LOGP "frame %u: the picture failed\n", (unsigned)p->frame);
            else if (st == NV2A_CAP_DEAD)
                fprintf(stderr, LOGP "frame %u: flip %u passed without a picture\n",
                        (unsigned)p->frame, (unsigned)p->flip);
        }
        pend_write(p, st == NV2A_CAP_DONE);
        done++;
    }
    if (done) {
        memmove(s_pend, s_pend + done, (s_npend - done) * sizeof *s_pend);
        s_npend -= done;
    }
}

/* The frame starting now: its canvas, and its picture asked for at the
 * flip that will present it. */
static int frame_begin(uint32_t f)
{
    const struct nv2a_cap_kernel *k = nv2a_cap_kernel();
    struct pend *p;
    char path[NV2A_CAP_PATH];
    int idle = 0;

    if (s_npend >= CAP_PEND_MAX) {
        pend_drain(1);
        if (s_npend >= CAP_PEND_MAX)
            return 0;               /* a picture still being written */
    }
    p = &s_pend[s_npend++];
    memset(p, 0, sizeof *p);
    p->frame = f;
    p->slot = -1;
    p->has_cv = hud_layer_canvas(&p->cv);
    if (!k || !k->walk_idle) {
        if (warn_once(CAP_WARN_KERN))
            fprintf(stderr, LOGP "no walker registered (the kernel's GPU thread has not"
                    " started): frames without pictures until it does\n");
        return 1;
    }
    /* The flip that shows this frame is the one after everything the
     * guest kicked so far (its last Swap included) is walked. */
    for (int ms = 0; k && k->walk_idle && ms <= CAP_IDLE_MS; ms++) {
        if (k->walk_idle()) {
            idle = 1;
            break;
        }
        nap_ms(1);
    }
    if (!idle) {
        fprintf(stderr, LOGP "frame %u: the walker did not catch up; no picture\n", (unsigned)f);
        return 1;
    }
    p->flip = nv2a_cap_flips() + 1u;
    p->has_flip = 1;
    snprintf(path, sizeof path, "%s/frame_%05u.bmp", s_dir, (unsigned)f);
    p->slot = nv2a_cap_post(p->flip, path);
    p->posted = s_tick;
    if (p->slot < 0)
        fprintf(stderr, LOGP "frame %u: no picture slot\n", (unsigned)f);
    return 1;
}

/* "<head> N lines (D dropped), P pictures of F frames, in DIR", then
 * the file closes. Under s_flock. */
static void finish(const char *head)
{
    fprintf(stderr, LOGP "%s %u lines (%u dropped), %u pictures of %u frames, in %s\n", head,
            (unsigned)__atomic_load_n(&s_sprites, __ATOMIC_RELAXED),
            (unsigned)__atomic_load_n(&s_dropped, __ATOMIC_RELAXED), (unsigned)s_bmps_out,
            (unsigned)s_frames_out, s_dir);
    __atomic_store_n(&s_finished, 1, __ATOMIC_RELEASE);
    hud_layer_set_recorder(NULL);
    /* The layer's active flag follows the mod alone again from its next
     * frame, as the owner's hooks do (hud_capture_live). */
    hud_layer_set_capture(0);
    if (s_out) {
        fclose(s_out);
        s_out = NULL;
    }
}

static void frame_locked(void)
{
    uint32_t f, last;
    int rec = 0;

    f = ++s_tick;
    last = s_from + s_count - 1u;
    /* The frame just ended: its lines join it. */
    if (s_npend && !s_pend[s_npend - 1u].sealed && s_pend[s_npend - 1u].frame == f - 1u) {
        struct pend *p = &s_pend[s_npend - 1u];
        __atomic_store_n(&s_rec, 0u, __ATOMIC_RELEASE);
        p->lines = take_lines(&p->len);
        p->sealed = 1;
    }
    __atomic_store_n(&s_rec, 0u, __ATOMIC_RELEASE);
    if (f >= s_from && f <= last && frame_begin(f)) {
        __atomic_store_n(&s_rec, f, __ATOMIC_RELEASE);
        rec = 1;
    }
    /* The layer computes a record (its bbox reads) only inside the window. */
    hud_layer_set_recorder(rec ? on_record : NULL);
    pend_drain(0);
    if (s_out)
        fflush(s_out);
    if (f > last && !s_npend) {
        char head[48];
        snprintf(head, sizeof head, "done: frames %u..%u,", (unsigned)s_from, (unsigned)last);
        finish(head);
    }
}

void hud_capture_frame(void)
{
    if (!s_on || __atomic_load_n(&s_finished, __ATOMIC_ACQUIRE))
        return;
    /* Held only by hud_capture_end: the run is ending, this frame is not
     * counted. */
    if (__atomic_exchange_n(&s_flock, 1, __ATOMIC_ACQUIRE))
        return;
    if (!__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE))
        frame_locked();
    __atomic_store_n(&s_flock, 0, __ATOMIC_RELEASE);
}

void hud_capture_end(void)
{
    int ms;

    if (!s_on || __atomic_load_n(&s_finished, __ATOMIC_ACQUIRE))
        return;
    __atomic_store_n(&s_rec, 0u, __ATOMIC_RELEASE);
    hud_layer_set_recorder(NULL);
    /* The guest's frame holds the lock for at most one frame_begin (its
     * 200 ms wait). Bounded: an exit raised on the guest's own thread
     * while it holds the lock leaves the file as it is. */
    for (ms = 0; __atomic_exchange_n(&s_flock, 1, __ATOMIC_ACQUIRE); ms++) {
        if (ms >= CAP_LOCK_MS) {
            fprintf(stderr, LOGP "end: the guest's frame holds the capture; left as it is\n");
            return;
        }
        nap_ms(1);
    }
    if (__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_flock, 0, __ATOMIC_RELEASE);
        return;
    }
    if (s_npend && !s_pend[s_npend - 1u].sealed) {
        struct pend *p = &s_pend[s_npend - 1u];
        p->lines = take_lines(&p->len);
        p->sealed = 1;
    }
    for (ms = 0; s_npend && ms < CAP_END_MS; ms += 10) {
        pend_drain(0);
        if (s_npend)
            nap_ms(10);
    }
    pend_drain(1);
    {
        char head[32];
        snprintf(head, sizeof head, "ended at frame %u:", (unsigned)s_tick);
        finish(head);
    }
    __atomic_store_n(&s_flock, 0, __ATOMIC_RELEASE);
}

/* ---- setup -------------------------------------------------------------- */

int hud_capture_init(const uintptr_t *mem)
{
    const char *dir = recomp_env(RENV_HUD_CAPTURE), *at;
    size_t n;

    if (!dir)
        return 0;
    hud_capture_reset();
    n = strlen(dir);
    while (n > 1 && (dir[n - 1] == '/' || dir[n - 1] == '\\'))
        n--;
    if (!n || n >= sizeof s_dir) {
        fprintf(stderr, LOGP "hud_capture=%s: not a usable directory; off\n", dir);
        return 0;
    }
    memcpy(s_dir, dir, n);
    s_dir[n] = 0;
    if ((at = recomp_env(RENV_HUD_CAPTURE_AT))) {
        char *end;
        unsigned long a = strtoul(at, &end, 10), c = 0;
        if (end != at && *end == ':') {
            const char *cs = end + 1;
            c = strtoul(cs, &end, 10);
            if (end == cs)
                c = 0;
        }
        if (!a || !c || *end || c > CAP_COUNT_MAX || a > 0x7FFFFFFFul) {
            fprintf(stderr, LOGP "hud_capture_at=%s is not FROM:COUNT (from >= 1, count 1..%u);"
                    " off\n", at, CAP_COUNT_MAX);
            return 0;
        }
        s_from = (uint32_t)a;
        s_count = (uint32_t)c;
    }
    out_open();
    if (!s_out)
        return 0;
    s_memp = mem;
    s_on = 1;
    hud_layer_set_mem(mem);
    hud_layer_set_capture(1);
    fprintf(stderr, LOGP "on: guest frames %u..%u into %s\n", (unsigned)s_from,
            (unsigned)(s_from + s_count - 1u), s_dir);
    return 1;
}

int hud_capture_on(void)
{
    return s_on;
}

int hud_capture_live(void)
{
    return s_on && !__atomic_load_n(&s_finished, __ATOMIC_ACQUIRE);
}

static char *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    char *t = NULL;
    long sz;

    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (sz = ftell(f)) >= 0 && sz < (1L << 24)
        && fseek(f, 0, SEEK_SET) == 0 && (t = malloc((size_t)sz + 1))) {
        *len = fread(t, 1, (size_t)sz, f);
        t[*len] = 0;
    }
    fclose(f);
    return t;
}

void hud_capture_tables(hud_hook_fn hook, int have_table)
{
    const char *path = recomp_env(RENV_HUD_CAPTURE_TABLE);
    const struct hud_draw *dr;
    int nd, hooked = 0;

    if (!s_on)
        return;
    if (path && have_table) {
        fprintf(stderr, LOGP "a mod registered a HUD table: it wins, hud_capture_table=%s"
                " is not read\n", path);
    } else if (path) {
        size_t len = 0;
        char *t = read_file(path, &len);
        if (!t)
            fprintf(stderr, LOGP "hud_capture_table=%s: cannot read it\n", path);
        else if (hud_layer_table(t, len, hook) < 0)
            fprintf(stderr, LOGP "hud_capture_table=%s: not taken (the line above says why)\n",
                    path);
        else
            hud_layer_table_is_capture();
        free(t);
    }
    if (!hud_layer_has_table()) {
        fprintf(stderr, LOGP "no HUD table: frames and pictures only\n");
        return;
    }
    nd = hud_layer_draws(&dr);
    for (int i = 0; i < nd; i++) {
        if (!dr[i].va) {
            fprintf(stderr, LOGP "draw %s: skipped, %s\n", dr[i].spec,
                    dr[i].why ? dr[i].why : "no address");
            continue;
        }
        if (hook && hook(dr[i].va, draw_pre, NULL, 0, (void *)&dr[i]))
            hooked++;
        else
            fprintf(stderr, LOGP "draw %s: no hook slot\n", dr[i].spec);
    }
    if (nd)
        fprintf(stderr, LOGP "%d of %d draws hooked (level 0)\n", hooked, nd);
}

void hud_capture_reset(void)
{
    for (unsigned i = 0; i < s_npend; i++)
        free(s_pend[i].lines);
    s_npend = 0;
    free(s_buf);
    s_buf = NULL;
    s_len = s_cap = 0;
    if (s_out)
        fclose(s_out);
    s_out = NULL;
    s_on = 0;
    __atomic_store_n(&s_finished, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_flock, 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_warned, 0, __ATOMIC_RELAXED);
    s_tick = 0;
    s_from = s_count = 1;
    s_dir[0] = 0;
    s_memp = NULL;
    s_frames_out = s_bmps_out = 0;
    __atomic_store_n(&s_rec, 0u, __ATOMIC_RELEASE);
    __atomic_store_n(&s_dropped, 0u, __ATOMIC_RELAXED);
    __atomic_store_n(&s_sprites, 0u, __ATOMIC_RELAXED);
    hud_layer_set_recorder(NULL);
    hud_layer_set_capture(0);
}
