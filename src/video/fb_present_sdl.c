/**
 * Show the presented frames in a window: the POSIX half of fb_present.c.
 *
 * fb_present.c is Win32 GDI and runs its window on a thread of its own. That
 * cannot work on macOS, where AppKit -- and so SDL's video and event pump --
 * only runs on the process main thread, and the guest used to own that thread
 * for the whole run. So the shape is inverted here:
 *
 *   process main thread   SDL window, the one SDL event loop, the presents
 *   "guest-main" pthread  the boot sequence and the recompiled entry point
 *
 * xbox_HostWindowMain() does the inversion: it opens the window, starts the
 * guest on a pthread with the main thread's stack size and QoS, and pumps
 * events and presents until the guest returns or the window is closed.
 *
 * Frames come from the backend's flip (cpu_on_flip calls
 * xbox_FramebufferWindowPresent on the nv2a-ack thread, with the surface the
 * walker picked). The ack thread copies the finished surface into a free
 * slot of three and publishes it with a pointer swap under a mutex held for a
 * few instructions, so the walker never waits for the window and the window
 * never reads guest memory the rasteriser may be writing. The copy is the
 * same bytes RECOMP_FB_DUMP writes (bpp from pitch / width, 2 = R5G6B5,
 * 4 = X8R8G8B8), so the window shows what the dump shows.
 *
 * With RECOMP_PB_BACKEND=metal the window is a CAMetalLayer instead
 * (fb_present_metal.m; metal_present=readback keeps the SDL renderer): the
 * presenter owns the Metal device and queue and the backend adopts them
 * (xbox_FramebufferWindowMetal), so a frame drawn on the GPU reaches the
 * window by a blit into the back slot's texture
 * (xbox_FramebufferWindowBackTexture, then PublishTexture, inside the same
 * s_in_present handshake) and never through guest memory. Guest-bytes frames are uploaded into a slot. When the
 * layer cannot be set up, the window is re-created with the SDL renderer.
 *
 * RECOMP_HEADLESS=1 (or no display, or SDL video failing to start) skips all
 * of it: the guest runs on the main thread exactly as before.
 *
 * Knobs: RECOMP_PRESENT_VSYNC=0 presents without waiting for the display;
 * RECOMP_PRESENT_STATS=1 prints a [PRESENT] line every 5 s (one is always
 * printed at exit); RECOMP_WINDOW_SCALE=<n> sizes the window (default 2).
 *
 * RECOMP_WINDOW_SHOT=<flip>[,<flip>...]: read back what the renderer drew for
 * the frame of that walker flip count (the number RECOMP_FB_DUMP_FLIPS and
 * RECOMP_FLIP_LOG print), at the window's pixel size, with
 * SDL_RenderReadPixels (the layer: a blit of the drawable), into
 * <prefix>win_flip<NNNNN>.bmp. <prefix> is RECOMP_WINDOW_SHOT_PREFIX, else
 * RECOMP_FB_DUMP, else "window_". A flip the window dropped is shot at the
 * next frame it shows, under that frame's own flip number. Only the
 * process's own pixels are read.
 *
 * RECOMP_WINDOW_QUIT_AFTER=<seconds>: post SDL_EVENT_QUIT then, the event the
 * window's close button produces, so a script can test the close path.
 *
 * Enhancements (nv2a_host_opts, set by the opt-in layer's
 * xbox_enhance_init; all zero without it): present.filter = linear sets the
 * frame texture's scale mode on the same logical-size letterbox; integer
 * draws the frame at the largest whole multiple that fits, centred
 * (nv2a_present_rect; a frame larger than the window falls back to a
 * linear fit, never a crop, which is why SDL's integer-scale logical
 * presentation is not used); present.fullscreen opens the window borderless fullscreen on the
 * desktop. The default (nearest, windowed) runs exactly the code it always
 * did.
 *
 * The window title is the title's to choose: call xbox_HostWindowSetTitle()
 * before xbox_HostWindowMain(). Unset, it is the XBE certificate's name, or
 * "Xbox Recomp" before the loader has read one. RECOMP_TRACE=title appends
 * FPS and draws, once a second (window_title.h).
 */
#if !defined(_WIN32)

#include <SDL3/SDL.h>
#include "recomp_env.h"
#include "platform/sdl_env.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sched.h>
#include <sys/resource.h>

#include "kernel/nv2a_pb_state.h"
#include "kernel/nv2a_backend_common.h"
#include "window_title.h"
#if defined(__APPLE__)
#include <strings.h>
#include "fb_present_metal.h"
#endif
#include "keyboard_sdl.h"     /* src/input: keys into the pad stand-in */
#include "mouse_sdl.h"        /* src/input: the mouse and its capture */
#include "text_input_sdl.h"   /* src/input: typed text for the menu */
#include "present_sdl_quit.h"
#ifdef XBOXRECOMP_ENHANCE
#include "kernel/nv2a_overlay.h"
#include "kernel/nv2a_stats.h"
#endif

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_log_thread_role(const char *role, uint32_t routine);
extern void xbox_InputSetMainPump(int on);   /* src/input/xinput_device.c */
extern void xbox_InputSdlPadSync(void);      /* src/input/xinput_device.c */

/* ── Handoff: three slots, one each for writer, reader and the ready frame ── */

struct fb_slot {
    uint8_t *px;
    size_t   cap;
    uint32_t w, h, bpp;
    uint32_t flip;                 /* the walker's flip count at this frame */
    /* Layer mode: the render scale and hor+ pad the frame was drawn at, for
     * display.overscan's crop (1 and 0 for an uploaded guest-size frame). */
    unsigned n;
    uint32_t pad;
};

static struct fb_slot  s_slot[3];
static int             s_back = 0, s_ready = 1, s_front = 2;
static uint64_t        s_ready_seq;          /* frames published */
static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile int    s_running;            /* window up: flips copy */
static Uint32          s_frame_event;        /* wakes the main loop */
static volatile int    s_wake_pending;
static volatile int    s_in_present;         /* ack thread inside Present */
/* Layer mode (macOS, Metal backend): the window has a CAMetalLayer and no
 * SDL_Renderer, and a slot is a GPU texture in fb_present_metal.m (indexed by
 * slot number) rather than bytes. Set once before the guest starts. */
static int             s_layer;

/* Ack-thread side counters, read by the stats line. */
static uint64_t s_flips, s_copy_ns;          /* relaxed atomics */

static uint64_t now_ns(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch)
{
    (void)fb_va;
    (void)pitch;
}

/* The AA row step (#162, fb_present.c). Unused here: the SDL window takes
 * the walker's own surface size and skips a surface that is not 2 or 4
 * bytes a pixel, so an anti-aliased one is not shown rather than shown
 * squashed. */
void xbox_FramebufferWindowSetAA(uint32_t sy) { (void)sy; }

void xbox_FramebufferWindowStart(void) {}

static void present_copy(uint32_t fb_va, uint32_t pitch);
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t pitch, uint32_t bpp, uint64_t t0);

/* Whether the window is up and takes frames: a GPU backend skips its
 * readback for the window when not (headless). */
int xbox_FramebufferWindowRunning(void)
{
    return __atomic_load_n(&s_running, __ATOMIC_SEQ_CST) != 0;
}

/* A GPU backend's frame, already in host memory (Metal: the presented
 * MTLTexture read back at the flip, BGRA8 = X8R8G8B8, bpp 4), handed to the
 * window instead of the guest surface. Same thread and handshake as
 * xbox_FramebufferWindowPresent. */
void xbox_FramebufferWindowPresentPixels(const void *px, uint32_t w, uint32_t h,
                                         uint32_t pitch, uint32_t bpp)
{
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !px || !w || !h
            || (bpp != 2 && bpp != 4) || pitch < w * bpp)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (__atomic_load_n(&s_running, __ATOMIC_SEQ_CST))
        present_pixels((const uint8_t *)px, w, h, pitch, bpp, now_ns(CLOCK_MONOTONIC));
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* Called by the backend at each flip, on the nv2a-ack thread. */
void xbox_FramebufferWindowPresent(uint32_t fb_va, uint32_t pitch)
{
    /* Dekker-style handshake with the close path: each side stores its own
     * flag and then loads the other's, all seq_cst, so either the window sees
     * this present in flight or this present sees the window closing. */
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !fb_va || !pitch)
        return;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {   /* closing */
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return;
    }
    present_copy(fb_va, pitch);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

static void present_copy(uint32_t fb_va, uint32_t pitch)
{
    const struct nv2a_pb_present *p;
    const uint8_t *src;
    uint32_t w = 640, h = 480, bpp;
    uint64_t t0 = now_ns(CLOCK_MONOTONIC);

    /* The walker's pick has the surface's own size; before its first pick
     * (or for a surface it did not pick) the display's 640x480. */
    p = nv2a_pb_present_state();
    if (p && p->offset && p->w && p->h && p->pitch == pitch
            && nv2a_pb_dma_resolve(p->offset) == fb_va) {
        w = p->w;
        h = p->h;
    }
    bpp = pitch / w;
    if (bpp != 2 && bpp != 4)
        return;
    src = (const uint8_t *)((uintptr_t)fb_va + xbox_GetMemoryOffset());
    present_pixels(src, w, h, pitch, bpp, t0);
}

/* The copy and the publish, from any w x h image of `bpp` bytes a pixel,
 * `pitch` bytes a row: guest memory for present_copy, a GPU backend's own
 * readback for xbox_FramebufferWindowPresentPixels. */
/* The back slot is filled: make it the ready one. An unshown ready frame is
 * overwritten (dropped), never waited for. */
static void publish(uint32_t w, uint32_t h, uint32_t bpp, uint64_t t0)
{
    struct fb_slot *s = &s_slot[s_back];   /* only this thread touches s_back */

    s->w = w;
    s->h = h;
    s->bpp = bpp;
    s->flip = nv2a_pb_gpu_state()->flips;
    pthread_mutex_lock(&s_lock);
    {
        int t = s_ready;
        s_ready = s_back;
        s_back = t;
        s_ready_seq++;
    }
    pthread_mutex_unlock(&s_lock);

    /* Wake the main loop, once per batch of frames it has not looked at. */
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        SDL_PushEvent(&ev);
    }

    __atomic_add_fetch(&s_flips, 1, __ATOMIC_RELAXED);
    __atomic_add_fetch(&s_copy_ns, now_ns(CLOCK_MONOTONIC) - t0, __ATOMIC_RELAXED);
}

/* The copy and the publish, from any w x h image of `bpp` bytes a pixel,
 * `pitch` bytes a row: guest memory for present_copy, a GPU backend's own
 * readback for xbox_FramebufferWindowPresentPixels. In layer mode the image
 * is uploaded into the back slot's texture instead. */
static void present_pixels(const uint8_t *src, uint32_t w, uint32_t h,
                           uint32_t pitch, uint32_t bpp, uint64_t t0)
{
    struct fb_slot *s;
    size_t need;
    uint32_t y;

#if defined(__APPLE__)
    if (s_layer) {
        if (fbm_upload(s_back, src, w, h, pitch, bpp)) {
            s_slot[s_back].n = 1;
            s_slot[s_back].pad = 0;
            publish(w, h, 4, t0);
        }
        return;
    }
#endif
    s = &s_slot[s_back];               /* only this thread touches s_back */
    need = (size_t)w * h * bpp;
    if (s->cap < need) {
        uint8_t *n = (uint8_t *)realloc(s->px, need);
        if (!n)
            return;
        s->px = n;
        s->cap = need;
    }
    for (y = 0; y < h; y++)
        memcpy(s->px + (size_t)y * w * bpp, src + (size_t)y * pitch,
               (size_t)w * bpp);
    publish(w, h, bpp, t0);
}

/* ── Layer mode: the Metal backend's side (nv2a-ack thread) ─────────────── */

/* 1 with the layer's device and queue when the window presents GPU slot
 * textures; the Metal backend adopts them at its init. 0 otherwise
 * (headless, the renderer window, not macOS). */
int xbox_FramebufferWindowMetal(void **device, void **queue)
{
#if defined(__APPLE__)
    if (s_layer) {
        fbm_get(device, queue);
        return 1;
    }
#endif
    (void)device;
    (void)queue;
    return 0;
}

/* The back slot's id<MTLTexture> at w x h, for the backend to blit its frame
 * into, or NULL (no layer, window closing). n and pad are the render scale
 * and hor+ pad the frame was drawn at. A non-NULL return must be
 * followed by xbox_FramebufferWindowPublishTexture: the ack thread counts as
 * inside a present from here to there (the close path's handshake). */
static uint64_t s_tex_t0;
void *xbox_FramebufferWindowBackTexture(uint32_t w, uint32_t h, unsigned n, uint32_t pad)
{
#if defined(__APPLE__)
    void *t;
    if (!s_layer || !__atomic_load_n(&s_running, __ATOMIC_SEQ_CST) || !w || !h)
        return NULL;
    __atomic_add_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
    if (!__atomic_load_n(&s_running, __ATOMIC_SEQ_CST)) {
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    s_tex_t0 = now_ns(CLOCK_MONOTONIC);
    t = fbm_slot(s_back, w, h);
    if (!t) {
        __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
        return NULL;
    }
    s_slot[s_back].w = w;
    s_slot[s_back].h = h;
    s_slot[s_back].n = n;
    s_slot[s_back].pad = pad;
    return t;
#else
    (void)w;
    (void)h;
    (void)n;
    (void)pad;
    return NULL;
#endif
}

/* The back slot's blit is committed: publish it. */
void xbox_FramebufferWindowPublishTexture(void)
{
    publish(s_slot[s_back].w, s_slot[s_back].h, 4, s_tex_t0);
    __atomic_sub_fetch(&s_in_present, 1, __ATOMIC_SEQ_CST);
}

/* ── The guest thread ─────────────────────────────────────────────────── */

static int (*s_guest_main)(void);
static volatile int s_guest_rc;
static Uint32 s_guest_done_event;

static void *guest_thread(void *arg)
{
    (void)arg;
    s_guest_rc = s_guest_main();
    {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_guest_done_event;
        SDL_PushEvent(&ev);
    }
    return NULL;
}

/* Title bar (window_title.h). A title's own xbox_HostWindowSetTitle wins;
 * otherwise the XBE certificate's name, which the loader hands
 * xbox_FramebufferWindowSetTitle. RECOMP_TRACE=title adds FPS and, once the
 * executor reports them, draws per frame, as the Win32 window does
 * (upstream 331d95b). Written from guest threads, read by the main thread:
 * a torn read shows a stale name until the next write, and s_name_gen,
 * bumped after each name change, makes sure there is one. */
static char s_title[128] = "Xbox Recomp";
static char s_xbe_title[128];
static int  s_host_title_set;
static int  s_have_draws;
static uint32_t s_frame_draws;
static unsigned s_name_gen;

void xbox_HostWindowSetTitle(const char *title)
{
    if (title && *title) {
        snprintf(s_title, sizeof s_title, "%s", title);
        __atomic_store_n(&s_host_title_set, 1, __ATOMIC_RELEASE);
        __atomic_add_fetch(&s_name_gen, 1, __ATOMIC_RELEASE);
    }
}

void xbox_FramebufferWindowSetTitle(const uint16_t *name, int max_chars)
{
    char buf[sizeof s_xbe_title];

    if (xbox_title_utf16_to_utf8(buf, sizeof buf, name, max_chars)) {
        memcpy(s_xbe_title, buf, strlen(buf) + 1);
        __atomic_add_fetch(&s_name_gen, 1, __ATOMIC_RELEASE);
    }
}

/* Draws in the frame just flipped; the executor calls it at the flip. */
void xbox_FramebufferWindowFrameStats(uint32_t draws)
{
    __atomic_store_n(&s_frame_draws, draws, __ATOMIC_RELAXED);
    __atomic_store_n(&s_have_draws, 1, __ATOMIC_RELAXED);
}

/* The flips the title's FPS counts: in enhancements builds the stats
 * counters, so the title and the stats strip agree (RECOMP_TRACE=title
 * holds NV2A_STATS_TITLE for the run); otherwise this file's own. */
static uint64_t title_flips(void)
{
#ifdef XBOXRECOMP_ENHANCE
    return nv2a_stats_count(NV2A_STAT_FLIPS);
#else
    return __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
#endif
}

static const char *window_name(void)
{
    if (__atomic_load_n(&s_host_title_set, __ATOMIC_ACQUIRE) || !s_xbe_title[0])
        return s_title;
    return s_xbe_title;
}

static int start_guest(void)
{
    pthread_attr_t attr;
    pthread_t th;
    struct rlimit rl;
    size_t stack = 64u << 20;
    int rc;

    /* At least the main thread's stack (8 MB by default), and generously
     * more: lifted code recurses on the native stack. */
    if (getrlimit(RLIMIT_STACK, &rl) == 0 && rl.rlim_cur != RLIM_INFINITY
            && rl.rlim_cur > stack)
        stack = (size_t)rl.rlim_cur;

    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, stack);
#if defined(__APPLE__)
    /* Same scheduling class as the thread it replaces, so moving the guest
     * off the main thread does not move it to the efficiency cores. */
    {
        qos_class_t qos;
        int rel;
        if (pthread_get_qos_class_np(pthread_self(), &qos, &rel) == 0
                && qos != QOS_CLASS_UNSPECIFIED)
            pthread_attr_set_qos_class_np(&attr, qos, rel);
    }
#endif
    rc = pthread_create(&th, &attr, guest_thread, NULL);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return 0;
    pthread_detach(th);
    return 1;
}

/* ── The window, on the process main thread ───────────────────────────── */

#define SHOT_MAX 32
static uint32_t s_shot[SHOT_MAX];
static int      s_shot_n;

static void shot_parse(void)
{
    const char *e = recomp_env(RENV_WINDOW_SHOT);
    while (e && *e && s_shot_n < SHOT_MAX) {
        char *end;
        unsigned long v = strtoul(e, &end, 0);
        if (end == e)
            break;
        s_shot[s_shot_n++] = (uint32_t)v;
        e = *end == ',' ? end + 1 : end;
    }
}

/* The first pending target this frame reaches, removed; -1 if none. */
static int shot_due(uint32_t flip)
{
    int i;
    for (i = 0; i < s_shot_n; i++)
        if (flip >= s_shot[i]) {
            s_shot[i] = s_shot[--s_shot_n];
            return 1;
        }
    return 0;
}

static void shot_path(char *path, size_t n, uint32_t flip)
{
    const char *prefix = recomp_env(RENV_WINDOW_SHOT_PREFIX);

    if (!prefix)
        prefix = recomp_env(RENV_FB_DUMP);
    if (!prefix)
        prefix = "window_";
    snprintf(path, n, "%swin_flip%05u.bmp", prefix, flip);
}

/* Between RenderTexture and RenderPresent: the back buffer is what is shown.
 * The read is of the whole window, letterbox bars included: logical
 * presentation is set aside for it (SDL3 reads only the logical viewport
 * otherwise) and restored, which leaves the drawn pixels as they are.
 * The read comes back 32-bit, which SDL_SaveBMP writes with a V5 header;
 * BGR24 keeps the 24-bit file the layer's shots and the SDL2 build write. */
static void shot_save(SDL_Renderer *ren, uint32_t flip)
{
    SDL_Surface *read, *surf = NULL;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    char path[512];
    int lw = 0, lh = 0;

    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED)
        SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    read = SDL_RenderReadPixels(ren, NULL);
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED)
        SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
    if (read) {
        surf = SDL_ConvertSurface(read, SDL_PIXELFORMAT_BGR24);
        SDL_DestroySurface(read);
    }
    shot_path(path, sizeof path, flip);
    if (surf && SDL_SaveBMP(surf, path))
        fprintf(stderr, "[PRESENT] window shot: %s (%dx%d, flip %u)\n",
                path, surf->w, surf->h, flip);
    else
        fprintf(stderr, "[PRESENT] window shot at flip %u failed: %s\n",
                flip, SDL_GetError());
    SDL_DestroySurface(surf);
}

#ifdef XBOXRECOMP_ENHANCE
/* ── The overlay (nv2a_overlay.h) on the renderer ─────────────────────────
 *
 * This loop is the overlay's one reader on SDL and on the CAMetalLayer: it
 * polls (the strip and toasts compose here), takes the visible lists and
 * draws them, or hands them to fbm_present. The overlay's wake posts the
 * frame event, so a change shows within one loop turn even while the guest
 * does not flip. */

static void ovl_wake(void)
{
    if (!__atomic_load_n(&s_running, __ATOMIC_ACQUIRE) || !s_frame_event)
        return;
    if (!__atomic_exchange_n(&s_wake_pending, 1, __ATOMIC_ACQ_REL)) {
        SDL_Event ev;
        memset(&ev, 0, sizeof ev);
        ev.type = s_frame_event;
        SDL_PushEvent(&ev);
    }
}

/* Texture 0 is the atlas, 1.. the image slots; each re-made when its gen
 * moves. Premultiplied, linear: a list built for another size is scaled. */
static SDL_Texture *s_ovl_tex[1 + NV2A_OVL_IMAGES];
static uint64_t     s_ovl_tex_gen[1 + NV2A_OVL_IMAGES];
/* Each layer's list in SDL's vertex form, converted again only when its
 * gen or the placement moves. */
static struct ovl_conv {
    SDL_Vertex *v;
    uint32_t cap;
    uint64_t gen;
    SDL_FRect place;
} s_ovl_conv[NV2A_OVL_COUNT];

static SDL_Texture *ovl_texture(SDL_Renderer *ren, uint16_t k)
{
    const uint32_t *px;
    uint32_t w, h;
    uint64_t gen;
    if (k == 0) {
        const struct nv2a_ovl_atlas *a = nv2a_overlay_atlas();
        if (!a)
            return NULL;
        px = a->px, w = a->w, h = a->h, gen = a->gen;
    } else {
        const struct nv2a_ovl_image *im = k <= NV2A_OVL_IMAGES ? nv2a_overlay_image(k) : NULL;
        if (!im || !im->px)
            return NULL;
        px = im->px, w = im->w, h = im->h, gen = im->gen;
    }
    if (s_ovl_tex[k] && s_ovl_tex_gen[k] == gen)
        return s_ovl_tex[k];
    if (s_ovl_tex[k])
        SDL_DestroyTexture(s_ovl_tex[k]);
    s_ovl_tex[k] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC,
                                     (int)w, (int)h);
    s_ovl_tex_gen[k] = gen;
    if (s_ovl_tex[k]) {
        SDL_SetTextureBlendMode(s_ovl_tex[k], SDL_BLENDMODE_BLEND_PREMULTIPLIED);
        SDL_SetTextureScaleMode(s_ovl_tex[k], SDL_SCALEMODE_LINEAR);
        SDL_UpdateTexture(s_ovl_tex[k], NULL, px, (int)(w * 4));
    }
    return s_ovl_tex[k];
}

/* The visible lists over the frame at place (output pixels), after the
 * frame and before the shot, so a window shot has them. */
static void ovl_draw(SDL_Renderer *ren, const SDL_FRect *place)
{
    const struct nv2a_ovl_list *ls[NV2A_OVL_COUNT];
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    int n, i, lw = 0, lh = 0;

    nv2a_overlay_set_placement((int)place->x, (int)place->y, (int)place->w, (int)place->h);
    n = nv2a_overlay_lists(ls);
    if (!n)
        return;
    /* The lists are in output pixels, not the frame's logical size. SDL
     * 3.2 maps each draw by the logical presentation as it is queued (no
     * intermediate target), so turning it off around these draws and back
     * on before the present leaves the frame's draw as it was. Re-check
     * after an SDL bump. */
    SDL_GetRenderLogicalPresentation(ren, &lw, &lh, &mode);
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED)
        SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    for (i = 0; i < n; i++) {
        const struct nv2a_ovl_list *l = ls[i];
        float sx = l->out_w ? place->w / (float)l->out_w : 1.0f;
        float sy = l->out_h ? place->h / (float)l->out_h : 1.0f;
        struct ovl_conv *cv = &s_ovl_conv[l->layer < NV2A_OVL_COUNT ? l->layer : 0];
        uint32_t v, c;
        if (l->nvtx > cv->cap) {
            SDL_Vertex *nv = (SDL_Vertex *)realloc(cv->v, l->nvtx * sizeof *nv);
            if (!nv)
                continue;
            cv->v = nv;
            cv->cap = l->nvtx;
            cv->gen = 0;
        }
        /* Converted already unless the list or the placement moved. */
        v = cv->gen == l->gen && !memcmp(&cv->place, place, sizeof *place) ? l->nvtx : 0;
        cv->gen = l->gen;
        cv->place = *place;
        for (; v < l->nvtx; v++) {
            const struct nv2a_ovl_vtx *p = &l->vtx[v];
            SDL_Vertex *q = &cv->v[v];
            q->position.x = place->x + p->x * sx;
            q->position.y = place->y + p->y * sy;
            q->tex_coord.x = p->u;
            q->tex_coord.y = p->v;
            q->color.r = (float)(p->rgba & 0xff) / 255.0f;
            q->color.g = (float)((p->rgba >> 8) & 0xff) / 255.0f;
            q->color.b = (float)((p->rgba >> 16) & 0xff) / 255.0f;
            q->color.a = (float)(p->rgba >> 24) / 255.0f;
        }
        for (c = 0; c < l->ncmd; c++) {
            const struct nv2a_ovl_cmd *k = &l->cmd[c];
            SDL_Texture *t = ovl_texture(ren, k->tex);
            SDL_Rect clip;
            if (!t || !k->count)
                continue;
            clip.x = (int)(place->x + k->clip[0] * sx);
            clip.y = (int)(place->y + k->clip[1] * sy);
            clip.w = (int)(k->clip[2] * sx + 0.999f);
            clip.h = (int)(k->clip[3] * sy + 0.999f);
            SDL_SetRenderClipRect(ren, &clip);
            SDL_RenderGeometryRaw(ren, t, &cv->v[0].position.x, sizeof(SDL_Vertex),
                                  &cv->v[0].color, sizeof(SDL_Vertex),
                                  &cv->v[0].tex_coord.x, sizeof(SDL_Vertex),
                                  (int)l->nvtx, l->idx + k->first, (int)k->count, 2);
        }
    }
    SDL_SetRenderClipRect(ren, NULL);
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED)
        SDL_SetRenderLogicalPresentation(ren, lw, lh, mode);
}

static void ovl_close(void)
{
    unsigned k;
    nv2a_overlay_set_wake(NULL);
    for (k = 0; k <= NV2A_OVL_IMAGES; k++) {
        if (s_ovl_tex[k])
            SDL_DestroyTexture(s_ovl_tex[k]);
        s_ovl_tex[k] = NULL;
    }
    for (k = 0; k < NV2A_OVL_COUNT; k++)
        free(s_ovl_conv[k].v);
    memset(s_ovl_conv, 0, sizeof s_ovl_conv);
}
#endif /* XBOXRECOMP_ENHANCE */

/* The renderer, or the layer's resources; the window goes after. */
static void close_present(SDL_Renderer *ren)
{
#ifdef XBOXRECOMP_ENHANCE
    ovl_close();
#endif
#if defined(__APPLE__)
    if (s_layer) {
        fbm_destroy();
        s_layer = 0;
        return;
    }
#endif
    if (ren)
        SDL_DestroyRenderer(ren);
}

/* present.filter changed live on the renderer path (layer mode passes the
 * filter to fbm_present at each present): the texture's scale mode, and the
 * logical presentation that integer placement does without. */
static void present_filter_set(SDL_Renderer *ren, SDL_Texture *tex, uint32_t w, uint32_t h,
                               int filter)
{
    if (!ren)
        return;
    if (tex)
        SDL_SetTextureScaleMode(tex, filter == NV2A_PRESENT_LINEAR ? SDL_SCALEMODE_LINEAR
                                                                   : SDL_SCALEMODE_NEAREST);
    if (filter == NV2A_PRESENT_INTEGER)
        SDL_SetRenderLogicalPresentation(ren, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    else if (tex)
        SDL_SetRenderLogicalPresentation(ren, (int)w, (int)h,
                                         SDL_LOGICAL_PRESENTATION_LETTERBOX);
}

#if defined(__APPLE__)
/* Whether present.fullscreen is a borderless window over the display (as
 * the D3D11 popup is) rather than a Spaces fullscreen. SDL3 defaults
 * SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES on, which makes every fullscreen
 * a new Space: a slide animation at start and at every switch, and a
 * switch macOS skips for an app that is not frontmost while SDL reports
 * success, so a live switch (the menu, opts_at) did nothing.
 * SDL_RaiseWindow before the switch is not the fix: whether a background
 * app may come forward is macOS's call (it may only bounce the Dock
 * icon), and a menu switch should not steal focus. Without Spaces SDL
 * makes the same window borderless at the display's size in place, above
 * the menu bar and the Dock, but the title bar's green button then zooms
 * instead. So stock runs keep Spaces, and the green button and
 * Ctrl-Cmd-F behave as in any Mac app; a run that asks for fullscreen
 * itself (fullscreen configured, the in-game menu armed, or opts_at)
 * goes borderless. SDL_VIDEO_MAC_FULLSCREEN_SPACES in the environment
 * wins over either (SDL reads the hint's environment variable first).
 * SDL caches the hint when Cocoa video starts, so it has to be set
 * before SDL_Init(SDL_INIT_VIDEO). Returns why the run goes borderless,
 * for the log, or NULL. */
static const char *mac_spaces_off(void)
{
    if (nv2a_host_opts()->fullscreen)
        return "present.fullscreen";
    if (recomp_env(RENV_OPTS_AT))
        return "opts_at";
#ifdef XBOXRECOMP_ENHANCE
    if (nv2a_overlay_layer_armed(NV2A_OVL_MENU))
        return "the menu can switch it";
#endif
    return NULL;
}

/* SDL puts a fullscreen window without Spaces above the menu bar and the
 * Dock and leaves it there when the app loses focus, so after Cmd-Tab the
 * game would still cover the display while keys go to an app nobody can
 * see. The D3D11 popup is not topmost, and Alt-Tab uncovers the desktop;
 * this does the same here: the normal level while the window has no
 * focus, back above the menu bar when it has. Checked on every pass (the
 * level is read, not remembered), so SDL raising it again on its own is
 * undone at once. SDL_HINT_ALLOW_TOPMOST=0 is not the fix: the menu bar
 * and the Dock would then draw over the focused game. */
static void mac_fullscreen_level(SDL_Window *win, int full, int spaces_off)
{
    int top;

    if (!full || !spaces_off)
        return;
    top = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    if (fbm_window_level(win, top))
        fprintf(stderr, "[PRESENT] macOS fullscreen: %s\n",
                top ? "focused, above the menu bar" : "no focus, normal window level");
}
#endif

int xbox_HostWindowMain(int (*guest_main)(void))
{
    SDL_Window *win;
    SDL_Renderer *ren = NULL;
    SDL_Texture *tex = NULL;
    uint32_t tex_w = 0, tex_h = 0, tex_bpp = 0;
    uint64_t shown_seq = 0, presents = 0, dropped_base = 0;
    uint64_t t_start, cpu_start, t_stat, cpu_stat, pres_stat = 0, flips_stat = 0;
    struct xbox_title_clock title_clock = {0};
    int title_stats = recomp_env_on(RENV_TITLE_STATS);
    uint64_t upload_ns = 0;
    int vsync = !recomp_env(RENV_PRESENT_VSYNC) || recomp_env_on(RENV_PRESENT_VSYNC);
    int stats = recomp_env_on(RENV_PRESENT_STATS);
    int scale = recomp_env(RENV_WINDOW_SCALE) ? atoi(recomp_env(RENV_WINDOW_SCALE)) : 2;
    int quit = 0, guest_done = 0, dirty = 0, shoot = 0;
    double quit_after = recomp_env(RENV_WINDOW_QUIT_AFTER)
                      ? atof(recomp_env(RENV_WINDOW_QUIT_AFTER)) : 0.0;
    uint32_t shown_flip = 0;
    const struct nv2a_host_opts *eo = nv2a_host_opts();
    int filter = eo->present_filter;
    int full = eo->fullscreen;   /* present.fullscreen as last applied here */
#if defined(__APPLE__)
    int spaces_off = 0;          /* fullscreen is borderless, not a Space */
#endif
    uint32_t opts_gen = nv2a_host_opts_gen();
    SDL_FRect dst;               /* integer: where the frame goes */
    int dst_ok = 0, out_w = 0, out_h = 0;
#ifdef XBOXRECOMP_ENHANCE
    uint64_t ovl_seen = 0;       /* nv2a_overlay_poll's value at the last draw */
#endif

    /* Ctrl-C and SIGTERM keep their default meaning (benches rely on it);
     * SDL would turn them into SDL_EVENT_QUIT. Set before the headless
     * return: audio and input start SDL subsystems in headless runs too. */
    xbox_sdl_env_compat();
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
#ifdef XBOXRECOMP_ENHANCE
    if (title_stats)
        nv2a_stats_use(NV2A_STATS_TITLE, 1);
#endif
    if (recomp_env_on(RENV_HEADLESS))
        return guest_main();
    shot_parse();

#if defined(__APPLE__)
    {
        const char *why = mac_spaces_off();
        /* The environment's value outranks a normal-priority hint, and
         * SDL_SetHint then fails: say what the run really gets. */
        if (why && SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, "0"))
            fprintf(stderr, "[PRESENT] macOS fullscreen: borderless, no Spaces (%s)\n",
                    why);
        else if (why)
            fprintf(stderr, "[PRESENT] macOS fullscreen: Spaces kept"
                            " (SDL_VIDEO_MAC_FULLSCREEN_SPACES in the environment)\n");
        spaces_off = !SDL_GetHintBoolean(SDL_HINT_VIDEO_MAC_FULLSCREEN_SPACES, true);
    }
#endif
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "[PRESENT] SDL video unavailable (%s); running headless\n",
                SDL_GetError());
        return guest_main();
    }
    if (scale < 1 || scale > 8)
        scale = 2;
    {
        /* Fullscreen with a NULL display mode is SDL3's borderless desktop
         * fullscreen; set after creation. */
        SDL_WindowFlags wflags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY
                               | (eo->fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        /* display.aspect: the padded frame's shape (854x480 at 16:9). */
        int win_w = (int)nv2a_host_window_w(640, 480, (unsigned)scale);
#if defined(__APPLE__)
        /* The Metal backend's frames stay on the GPU: a CAMetalLayer window
         * the backend blits into (fb_present_metal.m), unless
         * metal_present=readback asks for the renderer and readback. */
        const char *be = recomp_env(RENV_PB_BACKEND);
        const char *mp = recomp_env(RENV_METAL_PRESENT);
        if (be && !strcasecmp(be, "metal") && !(mp && !strcasecmp(mp, "readback"))) {
            char why[256] = "";
            win = SDL_CreateWindow(window_name(), win_w, 480 * scale,
                                   wflags | SDL_WINDOW_METAL);
            if (win && eo->fullscreen)
                SDL_SetWindowFullscreenMode(win, NULL);
            if (!win)
                snprintf(why, sizeof why, "%s", SDL_GetError());
            else if (fbm_create(win, vsync, s_shot_n > 0, why, sizeof why))
                s_layer = dirty = 1;   /* paint black before the first frame */
            if (!s_layer) {
                fbm_destroy();
                if (win)
                    SDL_DestroyWindow(win);
                fprintf(stderr, "[PRESENT] window up: CAMetalLayer unavailable (%s);"
                        " SDL renderer, readback present\n", why);
            }
        }
#endif
        if (!s_layer) {
            win = SDL_CreateWindow(window_name(), win_w, 480 * scale, wflags);
            if (win && eo->fullscreen)
                SDL_SetWindowFullscreenMode(win, NULL);
            ren = win ? SDL_CreateRenderer(win, NULL) : NULL;
            if (ren && vsync && !SDL_SetRenderVSync(ren, 1))
                fprintf(stderr, "[PRESENT] renderer vsync unavailable (%s)\n",
                        SDL_GetError());
            if (!ren) {
                fprintf(stderr, "[PRESENT] SDL window/renderer failed (%s); running headless\n",
                        SDL_GetError());
                if (win)
                    SDL_DestroyWindow(win);
                SDL_QuitSubSystem(SDL_INIT_VIDEO);
                return guest_main();
            }
        }
    }
    if (s_layer) {
        fprintf(stderr, "[PRESENT] window up: CAMetalLayer (zero-copy), vsync %s;"
                " guest runs on a pthread\n", vsync ? "on" : "off");
    } else {
        const char *rn = SDL_GetRendererName(ren);
        fprintf(stderr, "[PRESENT] window up: SDL %s renderer, vsync %s;"
                " guest runs on a pthread\n", rn ? rn : "?", vsync ? "on" : "off");
        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        SDL_RenderPresent(ren);
        /* The renderer scales into the window itself; present.surface is
         * for the D3D11 swap chain and the CAMetalLayer. */
        if (eo->present_surface == NV2A_PRESENT_SURFACE_INTERNAL)
            fprintf(stderr, "[PRESENT] present.surface=internal ignored (SDL presenter)\n");
    }

    s_guest_done_event = SDL_RegisterEvents(2);
    if (s_guest_done_event == 0) {
        fprintf(stderr, "[PRESENT] SDL_RegisterEvents failed; running headless\n");
        close_present(ren);
        SDL_DestroyWindow(win);
        SDL_QuitSubSystem(SDL_INIT_VIDEO);
        return guest_main();
    }
    s_frame_event = s_guest_done_event + 1;
#ifdef XBOXRECOMP_ENHANCE
    nv2a_overlay_set_presenter(s_layer ? "Metal" : "SDL");
    nv2a_overlay_set_wake(ovl_wake);
#endif
    s_guest_main = guest_main;
    xbox_InputSetMainPump(1);
    xbox_InputSdlInit(win);
    xbox_log_thread_role("sdl-main", 0);
    s_running = 1;
    if (!start_guest()) {
        fprintf(stderr, "[PRESENT] cannot start the guest thread; running headless\n");
        s_running = 0;
        xbox_InputSetMainPump(0);
        close_present(ren);
        SDL_DestroyWindow(win);
        return guest_main();
    }

    t_start = t_stat = now_ns(CLOCK_MONOTONIC);
    cpu_start = cpu_stat = now_ns(CLOCK_THREAD_CPUTIME_ID);

    while (!quit && !guest_done) {
        SDL_Event ev;
        uint64_t seq;

        /* One event loop for the window and the controllers: SDL_PumpEvents
         * (inside the wait) also updates the game controllers, which
         * xinput_device.c then only reads. */
        if (SDL_WaitEventTimeout(&ev, 100)) {
            do {
                /* Keys and the mouse for port 0, and the window events
                 * that release them; both only read the event. */
                xbox_InputSdlEvent(&ev);
                xbox_InputSdlMouseEvent(&ev);
                xbox_InputSdlTextEvent(&ev);
                if (ev.type == SDL_EVENT_QUIT)
                    quit = 1;
                else if (ev.type == s_guest_done_event)
                    guest_done = 1;
                else if (ev.type == s_frame_event)
                    __atomic_store_n(&s_wake_pending, 0, __ATOMIC_RELEASE);
                else if (ev.type == SDL_EVENT_WINDOW_EXPOSED
                         || ev.type == SDL_EVENT_WINDOW_RESIZED
                         || ev.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    dirty = 1;
            } while (SDL_PollEvent(&ev));
        }
        /* The capture can change on the poll thread (the release key, a
         * script); relative mode has to be set here, on the window's. */
        xbox_InputSdlMouseSync(win);
        /* Text input for a menu field, asked for on the guest's thread;
         * SDL wants it started and stopped on this one. */
        xbox_InputSdlTextSync(win);
#if defined(__APPLE__)
        mac_fullscreen_level(win, full, spaces_off);
#endif
        /* Pads come and go here, on the thread that pumps their events
         * (xinput_device.c says why it cannot be a poll thread). */
        xbox_InputSdlPadSync();
        /* Live present options (nv2a_host_opts_request): the walker applies
         * them at a flip and bumps the gen; the window and its renderer are
         * this thread's, so the change is made here. A new frame wakes the
         * loop, so it shows within a flip of the apply. */
        if (nv2a_host_opts_gen() != opts_gen) {
            opts_gen = nv2a_host_opts_gen();   /* acquire: the fields below are new */
            if (eo->present_filter != filter) {
                filter = eo->present_filter;
                present_filter_set(ren, tex, tex_w, tex_h, filter);
                dst_ok = 0;
                dirty = 1;
            }
            if (eo->fullscreen != full) {
                int want = eo->fullscreen;
                /* NULL mode: borderless desktop, as at creation. A failed
                 * switch keeps `full`, so the next gen change tries again. */
                if (want)
                    SDL_SetWindowFullscreenMode(win, NULL);
                if (!SDL_SetWindowFullscreen(win, want)) {
                    fprintf(stderr, "[PRESENT] present.fullscreen=%s failed (%s)\n",
                            want ? "on" : "off", SDL_GetError());
                } else {
                    full = want;
                    fprintf(stderr, "[PRESENT] present.fullscreen=%s (live)\n",
                            full ? "on" : "off");
                }
                dirty = 1;
            }
        }

        pthread_mutex_lock(&s_lock);
        seq = s_ready_seq;
        if (seq != shown_seq) {
            int t = s_front;
            s_front = s_ready;
            s_ready = t;
        }
        pthread_mutex_unlock(&s_lock);

        if (seq != shown_seq && s_layer) {
            /* Layer mode: the slot is already a GPU texture. */
            dropped_base += seq - shown_seq - 1;
            shown_flip = s_slot[s_front].flip;
            /* A shot still pending (no drawable yet) is taken of this frame,
             * under its own flip number, as for a dropped flip. */
            shoot |= s_shot_n && shot_due(shown_flip);
            shown_seq = seq;
            dirty = 1;
        } else if (seq != shown_seq) {
            struct fb_slot *f = &s_slot[s_front];
            uint64_t t0 = now_ns(CLOCK_MONOTONIC);

            dropped_base += seq - shown_seq - 1;
            shown_flip = f->flip;
            shoot = s_shot_n && shot_due(shown_flip);
            shown_seq = seq;
            if (!tex || f->w != tex_w || f->h != tex_h || f->bpp != tex_bpp) {
                if (tex)
                    SDL_DestroyTexture(tex);
                tex = SDL_CreateTexture(ren, f->bpp == 4 ? SDL_PIXELFORMAT_XRGB8888
                                                         : SDL_PIXELFORMAT_RGB565,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        (int)f->w, (int)f->h);
                tex_w = f->w;
                tex_h = f->h;
                tex_bpp = f->bpp;
                /* SDL3 textures start linear; the stock look is nearest. */
                if (tex)
                    SDL_SetTextureScaleMode(tex, filter == NV2A_PRESENT_LINEAR
                                                 ? SDL_SCALEMODE_LINEAR
                                                 : SDL_SCALEMODE_NEAREST);
                if (filter == NV2A_PRESENT_INTEGER)
                    dst_ok = 0;          /* new frame size: place it again */
                else
                    SDL_SetRenderLogicalPresentation(ren, (int)f->w, (int)f->h,
                                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);
            }
            if (tex)
                SDL_UpdateTexture(tex, NULL, f->px, (int)(f->w * f->bpp));
            upload_ns += now_ns(CLOCK_MONOTONIC) - t0;
            dirty = 1;
        }
#ifdef XBOXRECOMP_ENHANCE
        /* Disarmed (every golden): one load. Armed, the poll composes the
         * strip and toasts when due; a move in what it draws redraws. */
        if (nv2a_overlay_armed()) {
            uint64_t e = nv2a_overlay_poll(now_ns(CLOCK_MONOTONIC) / 1000000ull);
            if (e != ovl_seen) {
                ovl_seen = e;
                dirty = 1;
            }
        }
#endif
#if defined(__APPLE__)
        /* Not while minimised or hidden: nextDrawable then blocks for up to
         * its 1 s timeout, and the event pump and pad poll with it. */
        if (dirty && s_layer
                && !(SDL_GetWindowFlags(win) & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN))) {
            /* Before the first frame this paints the window black. */
            struct fb_slot *f = &s_slot[s_front];
            uint64_t t0 = now_ns(CLOCK_MONOTONIC);
            char path[512];
#ifdef XBOXRECOMP_ENHANCE
            const struct nv2a_ovl_list *ls[NV2A_OVL_COUNT];
            int nls = nv2a_overlay_visible() ? nv2a_overlay_lists(ls) : 0;
#else
            const struct nv2a_ovl_list *const *ls = NULL;
            int nls = 0;
#endif
            if (shoot)
                shot_path(path, sizeof path, shown_flip);
            if (fbm_present(win, s_front, shown_seq ? f->w : 0, shown_seq ? f->h : 0,
                            f->n, f->pad, filter, shoot ? path : NULL, shown_flip,
                            nls ? ls : NULL, nls)) {
                dirty = 0;
                shoot = 0;
                presents++;
#ifdef XBOXRECOMP_ENHANCE
                nv2a_stats_add(NV2A_STAT_PRESENTS, 1);
#endif
            }
            /* else no drawable (display asleep, window occluded): try again
             * at the next event or the 100 ms wake. */
            upload_ns += now_ns(CLOCK_MONOTONIC) - t0;
        }
#endif
        if (dirty && !s_layer) {
            dirty = 0;
            SDL_RenderClear(ren);
            if (tex && filter == NV2A_PRESENT_INTEGER) {
                int ow = 0, oh = 0;
                if (SDL_GetCurrentRenderOutputSize(ren, &ow, &oh)
                        && (!dst_ok || ow != out_w || oh != out_h)) {
                    struct nv2a_rect r;
                    int whole = nv2a_present_rect(tex_w, tex_h, (uint32_t)ow, (uint32_t)oh,
                                                  NV2A_PRESENT_INTEGER, &r);
                    dst.x = (float)r.x;
                    dst.y = (float)r.y;
                    dst.w = (float)r.w;
                    dst.h = (float)r.h;
                    out_w = ow;
                    out_h = oh;
                    dst_ok = 1;
                    /* A frame that does not fit even at 1x is shrunk:
                     * linear there, nearest for whole multiples. Set on
                     * every placement: a new texture starts nearest. */
                    SDL_SetTextureScaleMode(tex, whole ? SDL_SCALEMODE_NEAREST
                                                       : SDL_SCALEMODE_LINEAR);
                }
                SDL_RenderTexture(ren, tex, NULL, &dst);
            } else if (tex)
                SDL_RenderTexture(ren, tex, NULL, NULL);
#ifdef XBOXRECOMP_ENHANCE
            if (nv2a_overlay_visible()) {
                SDL_FRect pl = {0, 0, 0, 0};
                int ow = 0, oh = 0;
                if (tex && filter == NV2A_PRESENT_INTEGER && dst_ok)
                    pl = dst;
                else if (!tex || !SDL_GetRenderLogicalPresentationRect(ren, &pl)
                         || pl.w <= 0) {
                    SDL_GetCurrentRenderOutputSize(ren, &ow, &oh);
                    pl.w = (float)ow;
                    pl.h = (float)oh;
                }
                ovl_draw(ren, &pl);
            }
#endif
            if (shoot) {
                shoot = 0;
                shot_save(ren, shown_flip);
            }
            SDL_RenderPresent(ren);
            presents++;
#ifdef XBOXRECOMP_ENHANCE
            nv2a_stats_add(NV2A_STAT_PRESENTS, 1);
#endif
        }

        if (win) {                     /* title bar: name, or 1 Hz stats */
            double fps;
            if (xbox_title_due(&title_clock, title_stats,
                               now_ns(CLOCK_MONOTONIC) / 1000000ull, title_flips(),
                               __atomic_load_n(&s_name_gen, __ATOMIC_ACQUIRE),
                               &fps)) {
                char tb[192];
                xbox_title_format(tb, sizeof tb, window_name(), title_stats, fps,
                                  __atomic_load_n(&s_have_draws, __ATOMIC_RELAXED),
                                  __atomic_load_n(&s_frame_draws, __ATOMIC_RELAXED));
                SDL_SetWindowTitle(win, tb);
            }
        }
        if (quit_after > 0 && (double)(now_ns(CLOCK_MONOTONIC) - t_start) / 1e9
                                  >= quit_after) {
            SDL_Event q;
            memset(&q, 0, sizeof q);
            q.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&q);
            quit_after = 0;
        }
        if (stats) {
            uint64_t t = now_ns(CLOCK_MONOTONIC);
            if (t - t_stat >= 5000000000ull) {
                uint64_t cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
                double dt = (double)(t - t_stat) / 1e9;
                uint64_t fl = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
                fprintf(stderr, "[PRESENT] %.1f flips/s copied, %.1f presents/s,"
                        " main thread %.1f%% of a core, ack copy %.3f ms/flip\n",
                        (double)(fl - flips_stat) / dt,
                        (double)(presents - pres_stat) / dt,
                        100.0 * (double)(cpu - cpu_stat) / (double)(t - t_stat),
                        fl ? (double)__atomic_load_n(&s_copy_ns, __ATOMIC_RELAXED) / (double)fl / 1e6 : 0.0);
                t_stat = t;
                cpu_stat = cpu;
                pres_stat = presents;
                flips_stat = fl;
            }
        }
    }

    /* No present may be mid-publish (it pushes an SDL event) once SDL goes. */
    __atomic_store_n(&s_running, 0, __ATOMIC_SEQ_CST);
    while (__atomic_load_n(&s_in_present, __ATOMIC_SEQ_CST))
        sched_yield();
    {
        uint64_t t = now_ns(CLOCK_MONOTONIC), cpu = now_ns(CLOCK_THREAD_CPUTIME_ID);
        double dt = (double)(t - t_start) / 1e9;
        uint64_t fl = __atomic_load_n(&s_flips, __ATOMIC_RELAXED);
        fprintf(stderr, "[PRESENT] %s after %.1f s (%s): %llu flips copied (%.1f/s),"
                " %llu presents (%.1f/s), %llu dropped, main thread %.1f%% of a"
                " core, upload %.3f ms/frame, ack copy %.3f ms/flip\n",
                quit ? "window closed" : "guest returned", dt,
                s_layer ? "CAMetalLayer" : "SDL renderer",
                (unsigned long long)fl, dt > 0 ? (double)fl / dt : 0.0,
                (unsigned long long)presents, dt > 0 ? (double)presents / dt : 0.0,
                (unsigned long long)dropped_base,
                t > t_start ? 100.0 * (double)(cpu - cpu_start) / (double)(t - t_start) : 0.0,
                shown_seq ? (double)upload_ns / (double)shown_seq / 1e6 : 0.0,
                fl ? (double)__atomic_load_n(&s_copy_ns, __ATOMIC_RELAXED) / (double)fl / 1e6 : 0.0);
    }
    fflush(stdout);
    fflush(stderr);

    /* The guest is still running when the window closes, and there is no way
     * to stop lifted code mid-flight. exit() ends every thread; atexit
     * handlers (ICALL feedback, profiles) still run.
     *
     * Window and renderer go first, on this thread as AppKit wants, then the
     * video subsystem and nothing else (present_sdl_quit.h): the audio
     * stream and the game controllers are still in use on other threads.
     * Cmd-Q with sound on crashed in SDL_GetAudioStreamQueued when this was
     * SDL_Quit; the dummy driver's virtual clock never touches the stream. */
    if (tex)
        SDL_DestroyTexture(tex);
    close_present(ren);
    SDL_DestroyWindow(win);
    /* The main pump flag stays set: guest and OHCI threads keep running
     * until exit(), and must not start calling SDL_UpdateGamepads
     * while SDL is torn down under them. */
    SDL_QuitSubSystem(XBOX_PRESENT_SDL_QUIT);
    if (quit)
        exit(0);
    return s_guest_rc;
}

#endif /* !_WIN32 */
