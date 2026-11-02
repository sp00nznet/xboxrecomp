/*
 * mod_status test: mod API version 7 (src/enhance/mod.c, the status store
 * and glyph_name; enhance_mod_status.c, the panels; enhance_text.c, the
 * names), with the menu armed as a title arms it and a fake input backend:
 *
 *   status    set, replace, clear, the cut (7 lines, a 121-byte line, a
 *             UTF-8 sequence never split), the serial moving only on a
 *             change, an off mod's call ignored but its clear taken,
 *             switching a mod off (live and "at restart") dropping its
 *             panel and an enable bringing back no stale card
 *   panel     the tick arms NV2A_OVL_STATUS on the first panel, the cards
 *             stack in load order, the reference raster draws them at the
 *             top left inside the safe area, the menu open hides them, the
 *             last clear hides the layer, the hook's stop hides it
 *   glyphs    glyph_name for a source, an action on the pad and on the
 *             keyboard, a control, a chord, an unbound ref, each flag, a
 *             ref with its braces, a name cut to whole members, a
 *             captured control on the keyboard (its keys, a stick's four,
 *             the right stick's mouse), and a control after its rebinding
 *   device    the device the panel names follows the last press
 *   v6        a mod written against version 6 loads, runs and arms nothing
 *
 * Runs in its working directory, where it writes config/config.toml.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_menu.h"
#include "enhance_mod_status.h"
#include "enhance_text.h"
#include "host_pad.h"
#include "input_backend.h"
#include "keyboard.h"
#include "mod.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"
#include "nv2a_pb_state.h"
#include "nv2a_tex_pack.h"
#include "pad_map.h"
#include "recomp_env.h"

#include <fcntl.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */

static int s_pacing = 1;
int xbox_PacingMode(void);
int xbox_PacingMode(void) { return s_pacing; }
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing = mode; }
/* kernel_ramfs.c's switch, which this test does not link. */
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_path.c: the shader cache root a chain profile composes under
 * (shader_profile_req.c); NULL: none yet, as before the save root is up. */
static const char *s_cache_root;
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n)
{
    if (!s_cache_root)
        return 0;
    snprintf(out, n, "%s", s_cache_root);
    return 1;
}

ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
/* mod.c's v6 guest calls run guest code (recomp_guest_call.c, tested in
 * mod_guest_call); this test has none to run. */
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out);
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out)
{
    (void)who; (void)va; (void)eax; (void)ecx; (void)edx; (void)stack; (void)n_stack;
    (void)eax_out;
    return 0;
}
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align);
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align)
{
    (void)size; (void)align;
    return 0;
}

static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

/* ---- the fake pad backend --------------------------------------------- */

#define VK_F10 0x79

static uint64_t s_now = 1000;
static XBOX_INPUT_STATE f_pad[4];
static int f_conn[4] = { 1, 0, 0, 0 };
static uint32_t f_extra[4];

static uint64_t f_clock(void) { return __atomic_load_n(&s_now, __ATOMIC_ACQUIRE); }
static void advance(uint64_t ms) { __atomic_add_fetch(&s_now, ms, __ATOMIC_ACQ_REL); }

static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    *s = f_pad[p];
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    XBOX_INPUT_STATE s;
    DWORD rc = f_get(p, &s);
    if (rc == ERROR_SUCCESS) {
        xbox_PadRawFromGamepad(&s.Gamepad, extras ? f_extra[p] : 0, raw);
        *packet = s.dwPacketNumber;
    }
    return rc;
}
static DWORD f_read_raw(DWORD p, InputRaw *raw)
{
    if (p >= 4 || !f_conn[p])
        return ERROR_DEVICE_NOT_CONNECTED;
    xbox_PadRawFromGamepad(&f_pad[p].Gamepad, f_extra[p], raw);
    return ERROR_SUCCESS;
}

static const xbox_input_backend k_fake = {
    NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, f_read_raw, NULL, NULL };
const xbox_input_backend xbox_input_host_backend = {
    NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

static void pad_clear(void)
{
    memset(f_pad, 0, sizeof f_pad);
    memset(f_extra, 0, sizeof f_extra);
    f_conn[0] = 1;
}
static void hold_btn(WORD w, int on)
{
    if (on)
        f_pad[0].Gamepad.wButtons |= w;
    else
        f_pad[0].Gamepad.wButtons &= (WORD)~w;
}

static XBOX_INPUT_STATE poll0(void)
{
    XBOX_INPUT_STATE st;
    memset(&st, 0, sizeof st);
    xbox_InputGetState(0, &st);
    return st;
}

/* The flip's part: the live options requested apply there. */
static void flip(void)
{
    nv2a_host_opts_apply();
    nv2a_tex_pack_snapshot();
}

/* A key press and release, each seen by a poll a little apart, a flip
 * after each. */
static void tap(int vk)
{
    xbox_InputKeyEvent(vk, 1);
    advance(16);
    poll0();
    flip();
    xbox_InputKeyEvent(vk, 0);
    advance(16);
    poll0();
    flip();
}

/* ---- stderr capture ---------------------------------------------------- */

static int s_saved_fd = -1;
static char s_cap[32768];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("status_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr\n"); exit(2); }
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
    s_cap[0] = 0;
    f = fopen("status_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    {
        char *r = s_cap, *w = s_cap;
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
    }
    return s_cap;
}

/* ---- setup -------------------------------------------------------------- */

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
    recomp_env_reload();
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "cannot write %s\n", path);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static const char *start_with(const char *toml, const struct xbox_mod *const *mods)
{
    const char *out;
    xbox_mod_shutdown();
    enhance_menu_reset_for_test();
    nv2a_overlay_reset_for_test();
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeysClear();
    pad_clear();
    if (toml)
        write_file("config/config.toml", toml);
    else
        remove("config/config.toml");
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    out = capture_end();
    nv2a_overlay_set_placement(0, 0, 1920, 1080);
    poll0();
    return out;
}

#define ARMED "[menu]\nenabled = true\n"

static void open_menu(void) { tap(VK_F10); }
static void close_menu(void) { tap(0x1B); }

/* ---- the mods ------------------------------------------------------------ */

/* probe: version 7, a live switch, an action control and two captured
 * ones. */
enum { PC_TOGGLE, PC_MOVE, PC_RISE, PC_LOOK, PC_N };
static const char *const k_toggle_def[] = { "F6", "guide+y", NULL };
static const char *const k_move_def[] = { "left_stick", NULL };
static const char *const k_rise_def[] = { "rt", "x", NULL };
static const char *const k_look_def[] = { "right_stick", NULL };
static const struct xbox_mod_control k_probe_ctl[PC_N] = {
    [PC_TOGGLE] = { "toggle", "Toggle", NULL, NULL, XBOX_CTL_ACTION, k_toggle_def, NULL, NULL },
    [PC_MOVE] = { "move", "Move", NULL, NULL, XBOX_CTL_STICK, k_move_def, NULL, NULL },
    [PC_RISE] = { "rise", "Rise", NULL, NULL, XBOX_CTL_BUTTON, k_rise_def, NULL, NULL },
    [PC_LOOK] = { "look", "Look", NULL, NULL, XBOX_CTL_STICK, k_look_def, NULL, NULL },
};
static const struct xbox_mod_desc k_probe_desc = {
    XBOX_MOD_DESC_VERSION, sizeof k_probe_desc, "Probe", "The status test's mod", NULL, "1",
    NULL, 0, k_probe_ctl, PC_N
};
static const struct xbox_mod_api *s_papi;
static const struct xbox_mod k_probe;
static void probe_enable(int on, void *u) { (void)on; (void)u; }
static int probe_init(const struct xbox_mod_api *api)
{
    if (api->size < offsetof(struct xbox_mod_api, status) + sizeof api->status)
        return -1;
    s_papi = api;
    return api->on_enable(probe_enable, NULL) ? 0 : -1;
}
static const struct xbox_mod k_probe = { .name = "probe", .init = probe_init, .desc = &k_probe_desc };

/* second: version 7 too, no on_enable (a switch applies at restart); its
 * init sets its panel, before its state is decided. */
static const struct xbox_mod_api *s_sapi;
static const struct xbox_mod k_second;
static int second_init(const struct xbox_mod_api *api)
{
    s_sapi = api;
    return api->status(&k_second, "Second\nline") ? -1 : 0;
}
static const struct xbox_mod k_second = { .name = "second", .init = second_init };

/* old: written against version 6 (its size check names guest_alloc, the
 * last field it knew); it calls no version 7 field. */
static int s_old_frames;
static void old_frame(void *u) { (void)u; s_old_frames++; }
static int old_init(const struct xbox_mod_api *api)
{
    if (api->version < 6u
        || api->size < offsetof(struct xbox_mod_api, guest_alloc) + sizeof api->guest_alloc)
        return -1;
    return api->on_frame(old_frame, NULL) ? 0 : -1;
}
static const struct xbox_mod k_old = { .name = "old", .init = old_init };

static const struct xbox_mod *const k_mods[] = { &k_probe, &k_second, NULL };
static const struct xbox_mod *const k_old_mods[] = { &k_old, NULL };

static int mod_index(const char *name)
{
    for (int i = 0; i < xbox_mod_count(); i++)
        if (!strcmp(xbox_mod_name(i), name))
            return i;
    return -1;
}

static const char *panel(int i)
{
    static char t[XBOX_MOD_STATUS_LINES * (XBOX_MOD_STATUS_LINE + 1)];
    xbox_mod_status_text(i, t, sizeof t);
    return t;
}

static int status_shown(void)
{
    return (nv2a_overlay_visible() >> NV2A_OVL_STATUS) & 1u;
}

/* ---- 1. the store ---------------------------------------------------------- */

static void test_status(void)
{
    char big[1024], want[200];
    int p, s;
    uint32_t serial;

    start_with(ARMED, k_mods);
    p = mod_index("probe");
    s = mod_index("second");
    CHECK(p == 0 && s == 1);
    CHECK(xbox_mod_state(p) == XBOX_MOD_ON && xbox_mod_state(s) == XBOX_MOD_ON);
    CHECK(s_papi && s_papi->version == 7u && s_papi->glyph_name && s_papi->status);
    /* The one second's init set, kept once it started. */
    CHECK(!strcmp(panel(s), "Second\nline"));

    CHECK(s_papi->status(&k_probe, "Free camera\nline two") == 0);
    CHECK(!strcmp(panel(p), "Free camera\nline two"));
    serial = xbox_mod_status_serial();
    /* The same text again: nothing moves. */
    CHECK(s_papi->status(&k_probe, "Free camera\nline two") == 0);
    CHECK(xbox_mod_status_serial() == serial);
    /* Replace. */
    CHECK(s_papi->status(&k_probe, "Free camera") == 0);
    CHECK(!strcmp(panel(p), "Free camera"));
    CHECK(xbox_mod_status_serial() != serial);
    /* Clear: NULL and "" both. */
    CHECK(s_papi->status(&k_probe, NULL) == 0);
    CHECK(!panel(p)[0]);
    CHECK(s_papi->status(&k_probe, "x") == 0);
    CHECK(s_papi->status(&k_probe, "") == 0);
    CHECK(!panel(p)[0]);

    /* Seven lines: six kept, -1. */
    CHECK(s_papi->status(&k_probe, "1\n2\n3\n4\n5\n6\n7") == -1);
    CHECK(!strcmp(panel(p), "1\n2\n3\n4\n5\n6"));
    /* A 121-byte line: 120 kept, -1. */
    memset(big, 'a', 121);
    big[121] = 0;
    CHECK(s_papi->status(&k_probe, big) == -1);
    CHECK(strlen(panel(p)) == 120);
    /* 119 bytes, then a 2-byte sequence: cut before it, never inside. */
    memset(big, 'b', 119);
    memcpy(big + 119, "\xc3\xa9", 3);
    CHECK(s_papi->status(&k_probe, big) == -1);
    memset(want, 'b', 119);
    want[119] = 0;
    CHECK(!strcmp(panel(p), want));
    /* Exactly at the limits: no cut. */
    memset(big, 'c', 120);
    big[120] = 0;
    CHECK(s_papi->status(&k_probe, big) == 0);
    CHECK(s_papi->status(&k_probe, "1\n2\n3\n4\n5\n6") == 0);
    /* An unknown mod. */
    {
        static const struct xbox_mod k_stranger = { .name = "stranger" };
        CHECK(s_papi->status(&k_stranger, "hi") == -1);
    }

    /* Off live (on_enable): dropped now, and a late call is ignored. */
    CHECK(s_papi->status(&k_probe, "Free camera") == 0);
    CHECK(xbox_mod_set_enabled(p, 0) == XBOX_MOD_OFF);
    CHECK(!panel(p)[0]);
    CHECK(s_papi->status(&k_probe, "late") == 0);
    CHECK(!panel(p)[0]);
    /* A clear while off is taken: nothing to come back on the enable. */
    serial = xbox_mod_status_serial();
    CHECK(s_papi->status(&k_probe, NULL) == 0);
    CHECK(s_papi->status(&k_probe, "") == 0);
    CHECK(!panel(p)[0]);
    CHECK(xbox_mod_status_serial() == serial);
    CHECK(xbox_mod_set_enabled(p, 1) == XBOX_MOD_ON);
    CHECK(!panel(p)[0]);
    CHECK(s_papi->status(&k_probe, "back") == 0);
    CHECK(!strcmp(panel(p), "back"));

    /* Off at restart: dropped, and not back on the switch back. */
    CHECK(xbox_mod_set_enabled(s, 0) == XBOX_MOD_RESTART);
    CHECK(!panel(s)[0]);
    CHECK(xbox_mod_set_enabled(s, 1) == XBOX_MOD_ON);
    CHECK(!panel(s)[0]);
}

/* ---- 2. the panels on the overlay ------------------------------------------ */

/* Pixels the raster covered in [x0,x1) x [y0,y1). */
static long covered(const uint32_t *px, int w, int x0, int y0, int x1, int y1)
{
    long n = 0;
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++)
            if (px[y * w + x] >> 24)
                n++;
    return n;
}

static void test_panel(void)
{
    const struct nv2a_ovl_list *l;
    uint32_t *px;
    int p, s, w = 1920, h = 1080;

    start_with(ARMED, k_mods);
    p = mod_index("probe");
    s = mod_index("second");
    /* second's init panel made the toolkit start tracking. */
    CHECK(xbox_mod_glyphs_used());
    CHECK(!nv2a_overlay_layer_armed(NV2A_OVL_STATUS) || !status_shown());
    CHECK(s_papi->status(&k_probe, "Free camera\n{act:probe.toggle} Exit\n"
                                   "{ctl:move} Move \xc2\xb7 {ctl:rise} Up") == 0);
    enhance_mod_status_tick(0);
    CHECK(nv2a_overlay_layer_armed(NV2A_OVL_STATUS));
    CHECK(status_shown());
    /* Load order: probe's card, then second's. */
    CHECK(enhance_mod_status_compose(NULL, (uint32_t)w, (uint32_t)h) == 2);

    l = nv2a_overlay_acquire(NV2A_OVL_STATUS);
    CHECK(l != NULL);
    px = calloc((size_t)w * h, 4);
    if (l && px) {
        CHECK(l->nvtx > 0 && l->ncmd > 0);
        nv2a_ovl_ref_draw(px, w, h, l, nv2a_overlay_atlas());
        /* The cards sit in the top left, inside the 5% title-safe inset:
         * nothing above it or left of it, nothing on the right half. */
        CHECK(covered(px, w, 96, 54, 600, 400) > 2000);
        CHECK(covered(px, w, 0, 0, w, 50) == 0);
        CHECK(covered(px, w, 0, 0, 90, h) == 0);
        CHECK(covered(px, w, w / 2, 0, w, h) == 0);
    }
    free(px);

    /* Under the menu: hidden, back when it closes. */
    open_menu();
    CHECK(enhance_menu_is_open());
    enhance_mod_status_tick(0);
    CHECK(!status_shown());
    close_menu();
    CHECK(!enhance_menu_is_open());
    enhance_mod_status_tick(0);
    CHECK(status_shown());

    /* The flip tick stopped (a disarm): hidden at once, from the stop
     * itself; the next tick after a re-arm draws anew. */
    enhance_mod_status_tick(1);
    CHECK(!status_shown());
    enhance_mod_status_tick(0);
    CHECK(status_shown());
    /* The hook removed: its stop runs, then a fresh start draws again. */
    enhance_actions_set_panel_hook(NULL);
    CHECK(!status_shown());
    enhance_mod_status_start();
    enhance_mod_status_tick(0);
    CHECK(status_shown());

    /* Both cleared: the layer hides. */
    CHECK(s_papi->status(&k_probe, NULL) == 0);
    CHECK(s_sapi->status(&k_second, NULL) == 0);
    enhance_mod_status_tick(0);
    CHECK(!status_shown());
    CHECK(enhance_mod_status_compose(NULL, (uint32_t)w, (uint32_t)h) == 0);

    /* A card at the bottom edge is cut, never squeezed: six lines each,
     * at 1080 both fit; on some shorter output only the first does, and
     * a taller one never shows fewer. */
    CHECK(s_papi->status(&k_probe, "A\n1\n2\n3\n4\n5") == 0);
    CHECK(s_sapi->status(&k_second, "B\n1\n2\n3\n4\n5") == 0);
    CHECK(enhance_mod_status_compose(NULL, 1920, 1080) == 2);
    {
        int one = 0, grew = 1, last = 0;
        for (int hh = 80; hh <= 1080; hh += 20) {
            int n = enhance_mod_status_compose(NULL, 1920, (uint32_t)hh);
            if (n < last)
                grew = 0;
            if (n == 1)
                one = 1;
            last = n;
        }
        CHECK(one && grew);
    }
    /* A mod switched off takes its card with it. */
    CHECK(xbox_mod_set_enabled(p, 0) == XBOX_MOD_OFF);
    CHECK(enhance_mod_status_compose(NULL, 1920, 1080) == 1);
    (void)s;
}

/* ---- 3. glyph_name ----------------------------------------------------------- */

static void test_glyphs(void)
{
    char out[96];
    int p;
    static const char *const rebind[] = { "lb", NULL };

    start_with(ARMED, k_mods);
    p = mod_index("probe");
    xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_XBOX);
    enhance_mod_status_set_device(ENHANCE_DEV_PAD);
#define GN(ref, fl) s_papi->glyph_name(&k_probe, ref, out, sizeof out, fl)

    /* A source. */
    CHECK(GN("pad:south", XBOX_GLYPH_TOKENS) == 11 && !strcmp(out, "{pad:south}"));
    CHECK(GN("pad:south", XBOX_GLYPH_TEXT) == 1 && !strcmp(out, "A"));
    CHECK(GN("{pad:south}", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "A"));
    CHECK(GN("key:F5", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "F5"));
    CHECK(GN("key:F5", XBOX_GLYPH_TOKENS) > 0 && !strcmp(out, "{key:F5}"));
    xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_PLAYSTATION);
    CHECK(GN("pad:south", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "Cross"));
    xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_XBOX);

    /* An action on the pad: its chord. */
    CHECK(GN("act:probe.toggle", XBOX_GLYPH_TOKENS) > 0
          && !strcmp(out, "{pad:guide}+{pad:y}"));
    CHECK(GN("act:probe.toggle", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "Guide+Y"));
    /* ...and on the keyboard: its key. */
    enhance_mod_status_set_device(ENHANCE_DEV_KEYBOARD);
    CHECK(GN("act:probe.toggle", XBOX_GLYPH_TOKENS) > 0 && !strcmp(out, "{key:F6}"));
    CHECK(GN("act:probe.toggle", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "F6"));
    enhance_mod_status_set_device(ENHANCE_DEV_PAD);

    /* The mod's own controls. */
    CHECK(GN("ctl:move", XBOX_GLYPH_TOKENS) > 0 && !strcmp(out, "{pad:left_stick}"));
    CHECK(GN("ctl:move", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "Left stick"));
    CHECK(GN("ctl:rise", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "RT"));
    CHECK(GN("ctl:toggle", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "Guide+Y"));

    /* Unbound or naming nothing: -1, and out holds the ref. */
    CHECK(GN("ctl:nosuch", XBOX_GLYPH_TEXT) == -1 && !strcmp(out, "ctl:nosuch"));
    CHECK(GN("act:nobody.nothing", XBOX_GLYPH_TOKENS) == -1
          && !strcmp(out, "act:nobody.nothing"));
    CHECK(GN("pad:bogus", XBOX_GLYPH_TEXT) == -1 && !strcmp(out, "pad:bogus"));
    CHECK(s_papi->glyph_name(&k_probe, "pad:south", out, 0, XBOX_GLYPH_TEXT) == -1);
    /* Cut to cap, always ended. */
    CHECK(s_papi->glyph_name(&k_probe, "ctl:nosuch", out, 4, XBOX_GLYPH_TEXT) == -1
          && !strcmp(out, "ctl"));
    /* A bound name too long for cap: whole members only, -1. */
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 20, XBOX_GLYPH_TOKENS) == 19);
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 19, XBOX_GLYPH_TOKENS) == -1
          && !strcmp(out, "{pad:guide}"));
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 12, XBOX_GLYPH_TOKENS) == -1
          && !strcmp(out, "{pad:guide}"));
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 11, XBOX_GLYPH_TOKENS) == -1
          && !strcmp(out, ""));
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 8, XBOX_GLYPH_TEXT) == 7);
    CHECK(s_papi->glyph_name(&k_probe, "act:probe.toggle", out, 7, XBOX_GLYPH_TEXT) == -1
          && !strcmp(out, "Guide"));

    /* Captured controls on the keyboard: the keys the keyboard map gives
     * their guest controls, which drive them. */
    xbox_InputBindingsClear(INPUT_RIGHT_TRIGGER);
    CHECK(xbox_InputBind(INPUT_RIGHT_TRIGGER, 'E', 0) == 0);
    {
        static const struct { int ctl, vk; } wasd[] = {
            { INPUT_LSTICK_UP, 'W' }, { INPUT_LSTICK_LEFT, 'A' },
            { INPUT_LSTICK_DOWN, 'S' }, { INPUT_LSTICK_RIGHT, 'D' },
            { INPUT_RSTICK_UP, 'I' }, { INPUT_RSTICK_LEFT, 'J' },
            { INPUT_RSTICK_DOWN, 'K' }, { INPUT_RSTICK_RIGHT, 'L' },
        };
        for (size_t i = 0; i < sizeof wasd / sizeof wasd[0]; i++) {
            xbox_InputBindingsClear(wasd[i].ctl);
            CHECK(xbox_InputBind(wasd[i].ctl, wasd[i].vk, 0) == 0);
        }
    }
    enhance_mod_status_set_device(ENHANCE_DEV_KEYBOARD);
    CHECK(GN("ctl:rise", XBOX_GLYPH_TOKENS) > 0 && !strcmp(out, "{key:E}"));
    CHECK(GN("ctl:rise", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "E"));
    CHECK(GN("ctl:move", XBOX_GLYPH_TOKENS) > 0
          && !strcmp(out, "{key:W}/{key:A}/{key:S}/{key:D}"));
    CHECK(GN("ctl:move", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "W/A/S/D"));
    CHECK(GN("ctl:look", XBOX_GLYPH_TOKENS) > 0
          && !strcmp(out, "{key:I}/{key:J}/{key:K}/{key:L}/{mouse:move}"));
    CHECK(GN("ctl:look", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "I/J/K/L/Mouse move"));
    /* The mouse in use names the same keys. */
    enhance_mod_status_set_device(ENHANCE_DEV_MOUSE);
    CHECK(GN("ctl:rise", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "E"));
    /* A key the map lacks: the pad's name stays. */
    xbox_InputBindingsClear(INPUT_RIGHT_TRIGGER);
    xbox_InputBindingsClear(INPUT_X);
    CHECK(GN("ctl:rise", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "RT"));
    /* The panel draws the keys too: wider than the pad's glyph. */
    {
        mu_Font f = OVL_FONT(OVL_FONT_BODY, 24, 0);
        int kw, pw;
        enhance_mod_status_set_device(ENHANCE_DEV_KEYBOARD);
        kw = enhance_text_width_owned(f, "{ctl:move}", XBOX_PAD_FAMILY_XBOX,
                                      ENHANCE_DEV_KEYBOARD, p);
        pw = enhance_text_width_owned(f, "{ctl:move}", XBOX_PAD_FAMILY_XBOX, ENHANCE_DEV_PAD,
                                      p);
        CHECK(kw > pw && pw > 0);
    }
    xbox_InputBindingsReset();
    enhance_mod_status_set_device(ENHANCE_DEV_PAD);

    /* After a rebinding in the menu. */
    CHECK(xbox_mod_control_set_bindings(p, PC_RISE, rebind, 1) >= 0);
    CHECK(GN("ctl:rise", XBOX_GLYPH_TOKENS) > 0 && !strcmp(out, "{pad:lb}"));
    CHECK(GN("ctl:rise", XBOX_GLYPH_TEXT) > 0 && !strcmp(out, "LB"));
#undef GN
    xbox_InputSetGlyphFamily(XBOX_PAD_FAMILY_AUTO);
}

/* ---- 4. the device the names follow --------------------------------------- */

/* The tick reads the pads as the guest's last poll left them. */
static void test_device(void)
{
    start_with(ARMED, k_mods);
    enhance_mod_status_set_device(ENHANCE_DEV_PAD);
    xbox_InputKeyEvent('W', 1);
    poll0();
    enhance_mod_status_tick(0);
    CHECK(enhance_mod_status_device() == ENHANCE_DEV_KEYBOARD);
    /* Held: the pad's press still wins. */
    hold_btn(XBOX_GAMEPAD_DPAD_UP, 1);
    poll0();
    enhance_mod_status_tick(0);
    CHECK(enhance_mod_status_device() == ENHANCE_DEV_PAD);
    hold_btn(XBOX_GAMEPAD_DPAD_UP, 0);
    xbox_InputKeyEvent('W', 0);
    poll0();
    enhance_mod_status_tick(0);
    CHECK(enhance_mod_status_device() == ENHANCE_DEV_PAD);
    /* A stick pushed out. */
    enhance_mod_status_set_device(ENHANCE_DEV_KEYBOARD);
    f_pad[0].Gamepad.sThumbLX = 30000;
    poll0();
    enhance_mod_status_tick(0);
    CHECK(enhance_mod_status_device() == ENHANCE_DEV_PAD);
    f_pad[0].Gamepad.sThumbLX = 0;
}

/* ---- 5. a version 6 mod ------------------------------------------------------ */

static void test_v6(void)
{
    start_with(ARMED, k_old_mods);
    CHECK(xbox_mod_count() == 1);
    CHECK(xbox_mod_state(0) == XBOX_MOD_ON);
    s_old_frames = 0;
    xbox_mod_frame();
    CHECK(s_old_frames == 1);
    /* Nothing of version 7 used: no tracking, no layer. */
    CHECK(!xbox_mod_glyphs_used());
    enhance_mod_status_tick(0);
    CHECK(!nv2a_overlay_layer_armed(NV2A_OVL_STATUS));
}

int main(void)
{
    MKDIR("config");
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_MENU", NULL);
    set_env("RECOMP_TRACE", NULL);
    test_status();
    test_panel();
    test_glyphs();
    test_device();
    test_v6();
    xbox_mod_shutdown();
    printf("mod_status: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
