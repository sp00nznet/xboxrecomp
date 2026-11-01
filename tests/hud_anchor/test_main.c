/* The HUD layer (hud_anchor.h), driven as a mod drives it: a table, the
 * wrappers' pre and post as the hook service would call them, and
 * hud_layer_apply on quads in a fake guest memory. The numbers are the
 * design's (openspec change hud-anchors, A3 and 1.9) at a 640x480 target. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <unistd.h>
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

#include "hud_anchor.h"
#include "nv2a_backend_common.h"
#include "recomp_env.h"

static int s_checks, s_failed;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        s_checks++;                                                                                \
        if (!(x)) {                                                                                \
            s_failed++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)
#define NEAR(a, b) (fabsf((float)(a) - (float)(b)) < 1e-3f)

static uint8_t g_mem[1u << 16];
static uintptr_t g_base;

/* stderr into a file and back, for the trace lines' text. */
static int s_saved_fd = -1;
static char s_cap[8192];
static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("hud_anchor_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) {
        fprintf(stderr, "cannot capture stderr\n");
        exit(2);
    }
    DUP2(fd, 2);
    CLOSE(fd);
}
static const char *capture_end(void)
{
    FILE *f;
    size_t n = 0;
    fflush(stderr);
    DUP2(s_saved_fd, 2);
    CLOSE(s_saved_fd);
    f = fopen("hud_anchor_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    return s_cap;
}

/* The hook service: records the wrappers; s_hook_refuse makes it full. */
static struct {
    uint32_t va;
    xbox_mod_hook_fn pre, post;
} s_hooks[8];
static int s_nhooks, s_hook_refuse;

static int fake_hook(uint32_t va, xbox_mod_hook_fn pre, xbox_mod_hook_fn post, uint32_t flags,
                     void *user)
{
    (void)flags;
    (void)user;
    if (s_hook_refuse || s_nhooks >= 8)
        return 0;
    s_hooks[s_nhooks].va = va;
    s_hooks[s_nhooks].pre = pre;
    s_hooks[s_nhooks].post = post;
    s_nhooks++;
    return 1;
}

static void wr32(uint32_t va, uint32_t v) { memcpy(g_mem + va, &v, 4); }
static void wrf(uint32_t va, float v) { memcpy(g_mem + va, &v, 4); }
static float rdf(uint32_t va)
{
    float v;
    memcpy(&v, g_mem + va, 4);
    return v;
}

/* Vertices: x, y, u, v; 16 bytes. */
#define VTX 0x4000u
static float vx(uint32_t i) { return rdf(VTX + i * 16u); }
static float vu(uint32_t i) { return rdf(VTX + i * 16u + 8u); }

static void quad_at(uint32_t first, float x0, float x1, float u0, float u1)
{
    const float xs[4] = { x0, x1, x1, x0 }, us[4] = { u0, u1, u1, u0 };
    const float ys[4] = { 10, 10, 30, 30 };
    for (uint32_t k = 0; k < 4; k++) {
        uint32_t va = VTX + (first + k) * 16u;
        wrf(va, xs[k]);
        wrf(va + 4, ys[k]);
        wrf(va + 8, us[k]);
        wrf(va + 12, (float)k);
    }
}

static void quad(float x0, float x1) { quad_at(0, x0, x1, 0.0f, 1.0f); }

static struct xbox_mod_call at(uint32_t esp, uint32_t ret)
{
    struct xbox_mod_call c;
    memset(&c, 0, sizeof c);
    c.esp = esp;
    wr32(esp, ret);
    return c;
}

static int apply_n(uint32_t esp, uint32_t ret, uint32_t n, uint32_t flags)
{
    struct xbox_mod_call c = at(esp, ret);
    struct xbox_hud_call d = { VTX, n, flags, 0, 0 };
    return hud_layer_apply(&c, &d);
}

static int apply(uint32_t ret) { return apply_n(0x8000, ret, 4, 0); }

static void set_aspect(unsigned w, unsigned h)
{
    struct nv2a_host_opts o;
    memset(&o, 0, sizeof o);
    o.render_scale = 1;
    o.aspect_w = w;
    o.aspect_h = h;
    nv2a_host_opts_set(&o);
}

static int load(const char *t)
{
    hud_layer_reset();
    s_nhooks = 0;
    return hud_layer_table(t, strlen(t), fake_hook);
}

#define HEAD                                                                                       \
    "version = 1\n[canvas]\nw = 640\nh = 480\norigin = \"top-left\"\n"                             \
    "[layout]\nstride = 16\nx_off = 0\nu_off = 8\nprim = 4\n"

static const char k_table[] = HEAD
    "[[wrapper]]\nva = 0x1000\nname = \"text\"\n"
    "[[group]]\nname = \"score\"\nkind = \"left\"\n"
    "[[group]]\nname = \"timer\"\nkind = \"right\"\n"
    "[[group]]\nname = \"meter\"\nkind = \"proportional\"\n"
    "[[group]]\nname = \"bg\"\nkind = \"stretch\"\n"
    "[[group]]\nname = \"marks\"\nkind = \"world\"\npark = 200\n"
    "[[group]]\nname = \"title\"\nkind = \"centre\"\n"
    "[[group]]\nname = \"dim\"\nkind = \"centre\"\nfull = \"stretch\"\n"
    "[[site]]\nret_va = 0x2000\nelement = \"score\"\ngroup = \"score\"\n"
    "[[site]]\nret_va = 0x2010\nelement = \"timer\"\ngroup = \"timer\"\n"
    "[[site]]\nret_va = 0x2020\nelement = \"meter a\"\ngroup = \"meter\"\n"
    "[[site]]\nret_va = 0x2024\nelement = \"meter b\"\ngroup = \"meter\"\n"
    "[[site]]\nret_va = 0x2030\nelement = \"bg\"\ngroup = \"bg\"\n"
    "[[site]]\nret_va = 0x2040\nelement = \"marker\"\ngroup = \"marks\"\n"
    "[[site]]\nret_va = 0x2050\nelement = \"title\"\ngroup = \"title\"\n"
    "[[site]]\nret_va = 0x2060\nelement = \"gauge\"\nkind = \"proportional\"\n"
    "[[site]]\nret_va = 0x2070\nelement = \"dim\"\ngroup = \"dim\"\n";

static void test_errors(void)
{
    static const char *const bad[] = {
        /* no version, a wrong one */
        "[canvas]\nw = 640\nh = 480\n[layout]\nstride = 16\nx_off = 0\n",
        "version = 2\n[canvas]\nw = 640\nh = 480\n[layout]\nstride = 16\nx_off = 0\n",
        /* unknown key, unknown table */
        HEAD "[canvas2]\nw = 1\n",
        HEAD "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\ncolour = 1\n",
        /* unknown group, unknown kind, a site with both, with neither */
        HEAD "[[site]]\nret_va = 0x2000\nelement = \"a\"\ngroup = \"nope\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"sideways\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"left\"\n"
             "[[site]]\nret_va = 0x2000\nelement = \"a\"\ngroup = \"g\"\nkind = \"left\"\n",
        HEAD "[[site]]\nret_va = 0x2000\nelement = \"a\"\n",
        /* duplicate site, duplicate group */
        HEAD "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n"
             "[[site]]\nret_va = 0x2000\nelement = \"b\"\nkind = \"right\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"left\"\n[[group]]\nname = \"g\"\nkind = \"right\"\n",
        /* park off a world group */
        HEAD "[[group]]\nname = \"g\"\nkind = \"left\"\npark = 10\n",
        /* full on a stretch or world group, or not stretch or keep */
        HEAD "[[group]]\nname = \"g\"\nkind = \"stretch\"\nfull = \"stretch\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"world\"\nfull = \"keep\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"centre\"\nfull = \"wide\"\n",
        HEAD "[[group]]\nname = \"g\"\nkind = \"centre\"\nfull = 1\n",
        /* bad canvas and layout */
        "version = 1\n[canvas]\nw = 0\nh = 480\n[layout]\nstride = 16\nx_off = 0\n",
        "version = 1\n[canvas]\nw = 640\nh = 480\nview = [0, 0, 640]\n[layout]\nstride = 16\n"
        "x_off = 0\n",
        "version = 1\n[canvas]\nw = 640\nh = 480\n[layout]\nstride = 16\nx_off = 14\n",
        "version = 1\n[canvas]\nw = 640\nh = 480\nsource = \"memory\"\n[layout]\nstride = 16\n"
        "x_off = 0\n",
        /* not TOML */
        "version = 1\n[canvas\n",
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        int rc = load(bad[i]);
        if (rc >= 0)
            printf("accepted bad table %zu\n", i);
        CHECK(rc < 0);
        CHECK(!hud_layer_has_table());
    }
    /* A wrapper with no hook slot: nothing kept. */
    s_hook_refuse = 1;
    CHECK(load(HEAD "[[wrapper]]\nva = 0x1000\n") < 0);
    CHECK(!hud_layer_has_table());
    s_hook_refuse = 0;
    /* One table per run; [capture] takes only its reserved draws (2.1),
     * unused, and refuses any other key. */
    CHECK(load(k_table) == 9);
    CHECK(hud_layer_has_table() && s_nhooks == 1 && s_hooks[0].va == 0x1000);
    CHECK(hud_layer_table(k_table, strlen(k_table), fake_hook) < 0);
    CHECK(load(HEAD "[capture]\ndraws = [1, 2]\n") == 0);
    CHECK(load(HEAD "[capture.draws]\nqueue = 1\n") == 0);
    CHECK(load(HEAD "[capture]\nqueues = [1, 2]\n") < 0 && !hud_layer_has_table());
}

/* The edge pad (A3) at each ratio, and the keys' cap and margin. */
static void test_offsets(void)
{
    static const struct {
        unsigned w, h, aw, ah;
        int margin, pad, pad_hud;
    } t[] = {
        { 16, 10, 0, 0, 0, 64, 64 },   { 16, 9, 0, 0, 0, 107, 107 },
        { 21, 9, 0, 0, 0, 240, 240 },  { 21, 9, 16, 9, 0, 240, 107 },
        { 16, 9, 0, 0, 20, 107, 87 },  { 16, 9, 21, 9, 0, 107, 107 },
        { 16, 9, 4, 3, 0, 107, 0 },    { 16, 9, 0, 0, 200, 107, 43 },
        { 4, 3, 0, 0, 0, 0, 0 },
    };
    CHECK(load(k_table) == 9);
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) {
        set_aspect(t[i].w == 4 ? 0 : t[i].w, t[i].w == 4 ? 0 : t[i].h);
        hud_layer_set_keys(t[i].aw, t[i].ah, t[i].margin);
        CHECK(hud_layer_frame(1) == (t[i].pad > 0));
        if (hud_layer_pad() != t[i].pad || hud_layer_pad_hud() != t[i].pad_hud)
            printf("ratio %zu: pad %d hud %d\n", i, hud_layer_pad(), hud_layer_pad_hud());
        CHECK(hud_layer_pad() == t[i].pad);
        CHECK(hud_layer_pad_hud() == t[i].pad_hud);
    }
    /* 200 is clamped to 64; narrower than 4:3 reads as 4:3. */
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 200);
    CHECK(hud_layer_margin() == 64);
    hud_layer_set_keys(1, 1, 0);
    hud_layer_frame(1);
    CHECK(hud_layer_pad_hud() == 0);
    hud_layer_set_keys(0, 0, 0);
    hud_layer_set_margin(-5);
    CHECK(hud_layer_margin() == 0);
}

static void test_kinds(void)
{
    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    CHECK(hud_layer_frame(1) == 1);

    /* left, right, centre */
    quad(10, 50);
    CHECK(apply(0x2000) == XBOX_HUD_LEFT);
    CHECK(NEAR(vx(0), -97) && NEAR(vx(1), -57) && NEAR(vu(1), 1.0f));
    quad(590, 630);
    CHECK(apply(0x2010) == XBOX_HUD_RIGHT);
    CHECK(NEAR(vx(0), 697) && NEAR(vx(1), 737));
    quad(300, 340);
    CHECK(apply(0x2050) == XBOX_HUD_CENTRE);
    CHECK(NEAR(vx(0), 300) && NEAR(vx(1), 340));
    /* Full width: left alone by an edge kind. */
    quad(0, 640);
    CHECK(apply(0x2000) == XBOX_HUD_LEFT);
    CHECK(NEAR(vx(0), 0) && NEAR(vx(1), 640));

    /* proportional: centre 160 px right of the frame's -> 54. The group's
     * reference is its first primitive this frame, so a second one far
     * away moves by the same 54. */
    quad(460, 500);
    CHECK(apply(0x2020) == XBOX_HUD_PROPORTIONAL);
    CHECK(NEAR(vx(0), 514) && NEAR(vx(1), 554));
    quad(100, 140);
    apply(0x2024);
    CHECK(NEAR(vx(0), 154));
    /* A new frame takes a new reference. */
    hud_layer_frame(1);
    quad(100, 140);
    apply(0x2024);
    CHECK(NEAR(vx(0), 100 - 67));      /* centre 120: -200/320 * 107 = -66.9 */
    /* a site with its own kind: per primitive */
    quad(460, 500);
    apply(0x2060);
    CHECK(NEAR(vx(0), 514));
    quad(100, 140);
    apply(0x2060);
    CHECK(NEAR(vx(0), 100 - 67));

    /* full = "stretch": the full-width primitive is stretched (0..640 ->
     * -107..747), a smaller one of the same group keeps its kind, and a
     * full-width one of a plain centre group is left for Hor+. */
    quad(0, 640);
    CHECK(apply(0x2070) == XBOX_HUD_CENTRE);
    CHECK(NEAR(vx(0), -107) && NEAR(vx(1), 747) && NEAR(vu(0), 0) && NEAR(vu(1), 1));
    quad(110, 530);
    apply(0x2070);
    CHECK(NEAR(vx(0), 110) && NEAR(vx(1), 530));
    quad(0, 640);
    apply(0x2050);
    CHECK(NEAR(vx(0), 0) && NEAR(vx(1), 640));

    /* stretch: 0..320 -> -107..320 */
    quad(0, 320);
    CHECK(apply(0x2030) == XBOX_HUD_STRETCH);
    CHECK(NEAR(vx(0), -107) && NEAR(vx(1), 320));
    /* Stretched to full width yet inside the already-wide band: pushed to
     * -9 and 649, u kept. At 16:10 a quad of 50..590 stretches to -4..644. */
    set_aspect(16, 10);
    hud_layer_frame(1);
    quad(50, 590);
    apply(0x2030);
    CHECK(NEAR(vx(0), -9) && NEAR(vx(1), 649) && NEAR(vu(0), 0) && NEAR(vu(1), 1));
    /* One edge inside the band only: at pad 4 (648/480 = 81:60) a quad of
     * 0..680 stretches to -4..684.5, past the right edge alone. H5 would
     * take it as full width, so its left edge goes past the band too and
     * the right one stays. */
    set_aspect(81, 60);
    hud_layer_frame(1);
    quad(0, 680);
    apply(0x2030);
    CHECK(vx(0) < -8.0f && vx(1) > 648.0f && NEAR(vx(1), 684.5f) && NEAR(vu(0), 0)
          && NEAR(vu(1), 1));
    set_aspect(16, 9);
    hud_layer_frame(1);

    /* world: untouched, but past park (200 canvas units from the centre)
     * moved past the wide edge. */
    quad(400, 440);
    CHECK(apply(0x2040) == XBOX_HUD_WORLD);
    CHECK(NEAR(vx(0), 400));
    quad(530, 560);
    apply(0x2040);
    CHECK(NEAR(vx(0), 748));
    quad(60, 100);
    apply(0x2040);
    CHECK(NEAR(vx(1), -108));
}

static void test_clip(void)
{
    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    hud_layer_frame(1);

    /* Unlisted: centre, clipped to the 4:3 columns with u along x. */
    quad(600, 700);
    CHECK(apply(0x3000) == XBOX_HUD_CENTRE);
    CHECK(NEAR(vx(0), 600) && NEAR(vx(1), 640) && NEAR(vu(1), 0.4f) && NEAR(vu(0), 0));
    /* wholly outside: parked past the right edge, u kept */
    quad(700, 740);
    apply(0x3000);
    CHECK(NEAR(vx(0), 748) && NEAR(vx(1), 788) && NEAR(vu(1), 1));
    /* already past: stays */
    quad(800, 840);
    apply(0x3000);
    CHECK(NEAR(vx(0), 800));
    /* left of the window */
    quad(-60, -20);
    apply(0x3000);
    CHECK(NEAR(vx(1), -108));
    /* A left element crossing its own window [-107, 533] after the
     * shift: -20..20 -> -127..-87, clipped to -107..-87. */
    quad(-20, 20);
    apply(0x2000);
    CHECK(NEAR(vx(0), -107) && NEAR(vx(1), -87) && NEAR(vu(0), 0.5f) && NEAR(vu(1), 1));

    /* Skewed and crossing: left as is. */
    quad(600, 700);
    wrf(VTX + 2 * 16u, 680);       /* the third vertex off the sides */
    apply(0x3000);
    CHECK(NEAR(vx(1), 700) && NEAR(vx(2), 680));

    /* Skipped: not a multiple of prim, or over the restore cap; one line
     * each per site. */
    {
        const char *out;
        quad(10, 50);
        capture_begin();
        CHECK(apply_n(0x8000, 0x2000, 6, 0) == -1);
        CHECK(apply_n(0x8000, 0x2000, 6, 0) == -1);
        CHECK(NEAR(vx(0), 10));
        CHECK(apply_n(0x8000, 0x2000, HUD_RESTORE_MAX + 4, XBOX_HUD_RESTORE) == -1);
        CHECK(apply_n(0x8000, 0x2000, HUD_RESTORE_MAX + 4, XBOX_HUD_RESTORE) == -1);
        out = capture_end();
        CHECK(NEAR(vx(0), 10));
        CHECK(strcmp(out, "[hud.skip] 0x00002000 vertex count not a multiple of prim\n"
                          "[hud.skip] 0x00002000 over the restore cap\n") == 0);
    }
    CHECK(apply_n(0x8000, 0x2000, 0, 0) == XBOX_HUD_LEFT);
}

/* The wrappers: the outermost listed caller names the element. */
static void test_chain(void)
{
    struct xbox_mod_call w, w2;
    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_frame(1);

    w = at(0x7000, 0x2010);        /* the text wrapper, called from timer */
    s_hooks[0].pre(&w, NULL);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_RIGHT);
    CHECK(NEAR(vx(0), 407));
    /* nested: the wrapper again, from an unlisted caller; the outer wins */
    w2 = at(0x6800, 0x3334);
    s_hooks[0].pre(&w2, NULL);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x2000, 4, 0) == XBOX_HUD_RIGHT);
    s_hooks[0].post(&w2, NULL);
    /* a post at another esp pops nothing */
    {
        struct xbox_mod_call o = at(0x6900, 0);
        s_hooks[0].post(&o, NULL);
    }
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_RIGHT);
    s_hooks[0].post(&w, NULL);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_CENTRE);

    /* An entry whose post never came is ignored two frames on. */
    w = at(0x7000, 0x2010);
    s_hooks[0].pre(&w, NULL);
    hud_layer_frame(1);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_RIGHT);
    hud_layer_frame(1);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_CENTRE);
    /* and is dropped by the next push */
    w2 = at(0x6800, 0x2000);
    s_hooks[0].pre(&w2, NULL);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_LEFT);
    s_hooks[0].post(&w2, NULL);

    /* Deeper than HUD_CHAIN_MAX: the push dropped, the pops still match. */
    {
        struct xbox_mod_call d[HUD_CHAIN_MAX + 1];
        for (unsigned i = 0; i <= HUD_CHAIN_MAX; i++) {
            d[i] = at(0x7000 - 16u * i, i ? 0x3000u + i : 0x2000u);
            s_hooks[0].pre(&d[i], NULL);
        }
        for (unsigned i = HUD_CHAIN_MAX + 1; i-- > 0;)
            s_hooks[0].post(&d[i], NULL);
        quad(300, 340);
        CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_CENTRE);
    }
}

static void test_canvas(void)
{
    /* A 320x240 top-left canvas drawn over the whole 640x480 target: one
     * unit is two guest px, so a left element moves by 107/2 units. */
    static const char half[] =
        "version = 1\n[canvas]\nw = 320\nh = 240\norigin = \"top-left\"\n"
        "view = [0, 0, 640, 480]\n"
        "[layout]\nstride = 16\nx_off = 0\nu_off = 8\n"
        "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n"
        "[[site]]\nret_va = 0x2010\nelement = \"b\"\nkind = \"right\"\n";
    /* A centred canvas: x from -320 to 320. */
    static const char centre[] =
        "version = 1\n[canvas]\nw = 640\nh = 480\n"
        "[layout]\nstride = 16\nx_off = 0\nu_off = 8\n"
        "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n";
    /* Sized from guest memory; 0 there falls back to the declared size. */
    static const char mem[] =
        "version = 1\n[canvas]\nw = 640\nh = 480\norigin = \"top-left\"\nsource = \"memory\"\n"
        "w_va = 0x100\nh_va = 0x104\ntype = \"u32\"\n"
        "[layout]\nstride = 16\nx_off = 0\nu_off = 8\n"
        "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n";
    /* A fixed 1280x720 target: the pad comes from it, not the frame. */
    static const char fixed[] =
        "version = 1\n[canvas]\nw = 640\nh = 480\norigin = \"top-left\"\n"
        "view = [0, 0, 640, 480]\ntarget = [960, 720]\n"
        "[layout]\nstride = 16\nx_off = 0\nu_off = 8\n"
        "[[site]]\nret_va = 0x2000\nelement = \"a\"\nkind = \"left\"\n";

    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    CHECK(load(half) == 2);
    hud_layer_frame(1);
    quad(10, 30);
    apply(0x2000);
    CHECK(NEAR(vx(0), -43.5f) && NEAR(vx(1), -23.5f));
    quad(290, 310);
    apply(0x2010);
    CHECK(NEAR(vx(0), 343.5f));
    /* The 4:3 window in units: 0..320 shifted; past it, clipped. */
    quad(300, 330);
    apply(0x3000);
    CHECK(NEAR(vx(1), 320) && NEAR(vu(1), 20.0f / 30.0f));

    CHECK(load(centre) == 1);
    hud_layer_frame(1);
    quad(-310, -270);
    apply(0x2000);
    CHECK(NEAR(vx(0), -417) && NEAR(vx(1), -377));

    CHECK(load(mem) == 1);
    wr32(0x100, 0);
    wr32(0x104, 0);
    hud_layer_frame(1);
    quad(10, 50);
    apply(0x2000);
    CHECK(NEAR(vx(0), -97));
    /* 320x240 from memory over the 640-wide target: two px a unit */
    wr32(0x100, 320);
    wr32(0x104, 240);
    hud_layer_frame(1);
    quad(10, 50);
    apply(0x2000);
    CHECK(NEAR(vx(0), 10 - 53.5f));

    CHECK(load(fixed) == 1);
    hud_layer_frame(1);
    CHECK(hud_layer_pad() == (int)nv2a_host_pad(960, 720));
    CHECK(hud_layer_pad() == 160);
}

static void test_restore_and_state(void)
{
    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    hud_layer_frame(1);

    /* Restore puts back x and u at the same esp only. */
    {
        struct xbox_mod_call c = at(0x8000, 0x2000), o = at(0x8100, 0x2000);
        struct xbox_hud_call d = { VTX, 4, XBOX_HUD_RESTORE, 0, 0 };
        quad(-20, 20);
        CHECK(hud_layer_apply(&c, &d) == XBOX_HUD_LEFT);
        CHECK(NEAR(vx(0), -107) && NEAR(vu(0), 0.5f));
        hud_layer_restore(&o);
        CHECK(NEAR(vx(0), -107));
        hud_layer_restore(&c);
        CHECK(NEAR(vx(0), -20) && NEAR(vx(1), 20) && NEAR(vu(0), 0) && NEAR(vu(1), 1));
        /* once */
        wrf(VTX, 5);
        hud_layer_restore(&c);
        CHECK(NEAR(vx(0), 5));
    }

    /* The backends' flag follows the layer. */
    nv2a_aspect_frame_publish(640, 480);
    CHECK(hud_layer_frame(1) == 1);
    CHECK(nv2a_aspect_hud_on(640, 480));
    CHECK(!nv2a_aspect_hud_on(1280, 720));
    /* owner off: inactive, nothing moved */
    CHECK(hud_layer_frame(0) == 0);
    CHECK(!hud_layer_active() && !nv2a_aspect_hud_on(640, 480));
    quad(10, 50);
    CHECK(apply(0x2000) == -1);
    CHECK(NEAR(vx(0), 10));
    /* 4:3: inactive */
    set_aspect(0, 0);
    CHECK(hud_layer_frame(1) == 0);
    CHECK(apply(0x2000) == -1);
    /* no table: inactive whatever the owner */
    set_aspect(16, 9);
    hud_layer_reset();
    CHECK(hud_layer_frame(1) == 0 && !hud_layer_has_table());
    CHECK(apply(0x2000) == -1);
    CHECK(NEAR(vx(0), 10));
}

/* Restore is byte-exact over the whole buffer, x needing no change and
 * u written back included; nested RESTORE calls unwind in order. */
static void test_restore_exact(void)
{
    enum { LEN = 3u * 4u * 16u + 32u };     /* three quads and a tail */
    uint8_t want[LEN], mid[LEN];
    const char *out;

    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    hud_layer_frame(1);
    for (uint32_t i = 0; i < LEN; i++)
        g_mem[VTX + i] = (uint8_t)(i * 7u + 3u);
    quad_at(0, 100, 140, 0.0f, 1.0f);       /* inside: no change */
    quad_at(4, 600, 700, 0.25f, 0.75f);     /* crossing: x and u cut */
    quad_at(8, 300, 340, 0.0f, 1.0f);
    memcpy(want, g_mem + VTX, LEN);
    {
        struct xbox_mod_call c = at(0x8000, 0x3000);
        struct xbox_hud_call d = { VTX, 12, XBOX_HUD_RESTORE, 0, 0 };
        CHECK(hud_layer_apply(&c, &d) == XBOX_HUD_CENTRE);
        CHECK(memcmp(want, g_mem + VTX, LEN) != 0);
        CHECK(NEAR(vx(5), 640) && NEAR(vu(5), 0.45f));
        hud_layer_restore(&c);
        CHECK(memcmp(want, g_mem + VTX, LEN) == 0);
    }

    /* Nested: the inner call's save does not clobber the outer one. */
    {
        struct xbox_mod_call o = at(0x8000, 0x2000), in = at(0x7F00, 0x2000);
        struct xbox_hud_call d = { VTX, 12, XBOX_HUD_RESTORE, 0, 0 };
        CHECK(hud_layer_apply(&o, &d) == XBOX_HUD_LEFT);
        memcpy(mid, g_mem + VTX, LEN);
        CHECK(hud_layer_apply(&in, &d) == XBOX_HUD_LEFT);
        CHECK(memcmp(mid, g_mem + VTX, LEN) != 0);
        hud_layer_restore(&in);
        CHECK(memcmp(mid, g_mem + VTX, LEN) == 0);
        hud_layer_restore(&o);
        CHECK(memcmp(want, g_mem + VTX, LEN) == 0);
    }

    /* Deeper than HUD_SAVE_MAX: skipped untouched, one line; the others
     * still unwind. */
    {
        struct xbox_mod_call c[HUD_SAVE_MAX + 1];
        struct xbox_hud_call d = { VTX, 4, XBOX_HUD_RESTORE, 0, 0 };
        for (unsigned i = 0; i < HUD_SAVE_MAX; i++) {
            c[i] = at(0x8000 - 0x100u * i, 0x3000);
            CHECK(hud_layer_apply(&c[i], &d) == XBOX_HUD_CENTRE);
        }
        c[HUD_SAVE_MAX] = at(0x8000 - 0x100u * HUD_SAVE_MAX, 0x3000);
        memcpy(mid, g_mem + VTX, LEN);
        capture_begin();
        CHECK(hud_layer_apply(&c[HUD_SAVE_MAX], &d) == -1);
        out = capture_end();
        CHECK(strcmp(out, "[hud.skip] 0x00003000 restore calls nested too deep\n") == 0);
        CHECK(memcmp(mid, g_mem + VTX, LEN) == 0);
        hud_layer_restore(&c[HUD_SAVE_MAX]);
        for (unsigned i = HUD_SAVE_MAX; i-- > 0;)
            hud_layer_restore(&c[i]);
        CHECK(memcmp(want, g_mem + VTX, LEN) == 0);
    }

    /* A save whose post never came is dropped by the next apply at or
     * below its esp, so a later post there writes nothing back. */
    {
        struct xbox_mod_call c = at(0x8000, 0x2000), lone;
        struct xbox_hud_call d = { VTX, 4, XBOX_HUD_RESTORE, 0, 0 }, plain = d;
        CHECK(hud_layer_apply(&c, &d) == XBOX_HUD_LEFT);
        memcpy(g_mem + VTX, want, LEN);
        lone = at(0x8000, 0x3000);         /* the same slot, another call */
        plain.flags = 0;
        CHECK(hud_layer_apply(&lone, &plain) == XBOX_HUD_CENTRE);
        memcpy(mid, g_mem + VTX, LEN);
        hud_layer_restore(&c);
        CHECK(memcmp(mid, g_mem + VTX, LEN) == 0);
    }

    /* The owner off between the pre and the post (mod.c): post_off
     * restores too. */
    {
        struct xbox_mod_call c = at(0x8000, 0x2000);
        struct xbox_hud_call d = { VTX, 12, XBOX_HUD_RESTORE, 0, 0 };
        memcpy(g_mem + VTX, want, LEN);
        CHECK(hud_layer_apply(&c, &d) == XBOX_HUD_LEFT);
        CHECK(hud_layer_frame(0) == 0);
        hud_layer_post_off(&c);
        CHECK(memcmp(want, g_mem + VTX, LEN) == 0);
        hud_layer_frame(1);
    }
}

#ifndef _WIN32
/* Two guest threads: one group's reference, and each thread's own chain. */
enum { ROUNDS = 400 };
static int s_round, s_done;
static int s_dx[2][ROUNDS];

static void wait_for(int *v, int want)
{
    while (__atomic_load_n(v, __ATOMIC_ACQUIRE) < want)
        sched_yield();
}

/* Each thread its own stack slot and vertices; a proportional group of
 * two members, so whichever thread adds first sets the reference. */
static void *group_thread(void *arg)
{
    int id = (int)(intptr_t)arg;
    uint32_t vtx = 0x6000u + 0x100u * (uint32_t)id, esp = 0x9000u + 0x100u * (uint32_t)id;
    for (int r = 1; r <= ROUNDS; r++) {
        struct xbox_mod_call c;
        struct xbox_hud_call d = { vtx, 4, 0, 0, 0 };
        float x0 = id ? 400.0f : 100.0f;
        wait_for(&s_round, r);
        for (uint32_t k = 0; k < 4; k++)
            wrf(vtx + k * 16u, k == 1 || k == 2 ? x0 + 40.0f : x0);
        c = at(esp, id ? 0x2024u : 0x2020u);
        hud_layer_apply(&c, &d);
        s_dx[id][r - 1] = (int)lroundf(rdf(vtx) - x0);
        __atomic_add_fetch(&s_done, 1, __ATOMIC_ACQ_REL);
    }
    return NULL;
}

static void test_threads_group(void)
{
    pthread_t th[2];
    int same = 1, left = 0, right = 0;

    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_set_keys(0, 0, 0);
    hud_layer_frame(1);
    s_round = s_done = 0;
    for (int i = 0; i < 2; i++)
        pthread_create(&th[i], NULL, group_thread, (void *)(intptr_t)i);
    for (int r = 1; r <= ROUNDS; r++) {
        hud_layer_frame(1);                 /* a new epoch: a new reference */
        __atomic_store_n(&s_round, r, __ATOMIC_RELEASE);
        wait_for(&s_done, 2 * r);
    }
    for (int i = 0; i < 2; i++)
        pthread_join(th[i], NULL);
    for (int r = 0; r < ROUNDS; r++) {
        same &= s_dx[0][r] == s_dx[1][r];
        left += s_dx[0][r] < 0;             /* member a (x 100) set it */
        right += s_dx[0][r] > 0;            /* member b (x 400) did */
    }
    CHECK(same);
    CHECK(left + right == ROUNDS);
}

static struct xbox_mod_call s_main_w;
static int s_stray_kind[3];

/* Another thread's stray pushes and pops: at the main thread's esp, a pop
 * with no push, and a push it never pops. */
static void *stray_thread(void *arg)
{
    struct xbox_mod_call w = at(0x7000, 0x2000), o = at(0x6F00, 0x2000);
    struct xbox_hud_call d = { 0x6400u, 4, 0, 0, 0 };
    (void)arg;
    for (uint32_t k = 0; k < 4; k++)
        wrf(0x6400u + k * 16u, k == 1 || k == 2 ? 340.0f : 300.0f);
    s_hooks[0].post(&s_main_w, NULL);
    {
        struct xbox_mod_call c = at(0x9800, 0x3333);
        s_stray_kind[0] = hud_layer_apply(&c, &d);  /* the main thread's chain unseen */
    }
    s_hooks[0].pre(&w, NULL);
    s_hooks[0].post(&w, NULL);
    s_hooks[0].pre(&o, NULL);
    {
        struct xbox_mod_call c = at(0x9800, 0x3333);
        s_stray_kind[1] = hud_layer_apply(&c, &d);  /* its own entry: score */
    }
    return NULL;
}

static void test_threads_chain(void)
{
    pthread_t th;

    CHECK(load(k_table) == 9);
    set_aspect(16, 9);
    hud_layer_frame(1);
    s_main_w = at(0x7000, 0x2010);          /* the text wrapper from timer */
    s_hooks[0].pre(&s_main_w, NULL);
    pthread_create(&th, NULL, stray_thread, NULL);
    pthread_join(th, NULL);
    CHECK(s_stray_kind[0] == XBOX_HUD_CENTRE);
    CHECK(s_stray_kind[1] == XBOX_HUD_LEFT);
    wr32(0x7000, 0x2010);                   /* the other thread wrote there */
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_RIGHT);
    s_hooks[0].post(&s_main_w, NULL);
    quad(300, 340);
    CHECK(apply_n(0x6000, 0x3333, 4, 0) == XBOX_HUD_CENTRE);
}
#endif

int main(void)
{
    g_base = (uintptr_t)g_mem;
    hud_layer_set_mem(&g_base);
    /* The trace lines' paths run too (their text goes to stderr). */
    recomp_env_set(RENV_HUD_TRACE, "1");

    test_errors();
    test_offsets();
    test_kinds();
    test_clip();
    test_chain();
    test_canvas();
    test_restore_and_state();
    test_restore_exact();
#ifndef _WIN32
    test_threads_group();
    test_threads_chain();
#endif
    hud_layer_reset();

    printf("hud_anchor: %d checks, %d failed\n", s_checks, s_failed);
    return s_failed != 0;
}
