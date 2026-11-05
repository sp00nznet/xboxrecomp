/*
 * enhance_mod test: the mods (src/enhance/mod.c) as a title runs them:
 * the table handed over before xbox_enhance_init, the inits in order with
 * one that fails and leaves nothing behind, the frame dispatch, the reverse
 * shutdown; the batch filter as a GPU backend's on_draw asks it
 * (nv2a_batch_skip), with and without one installed; and the input capture,
 * which takes the mouse's motion off the pad's look stick and gives it to
 * the mod, beside the title's last port-0 read. Runs in its working
 * directory, where it writes config/config.toml.
 */
#include "enhance.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "input_backend.h"
#include "input_names.h"
#include "pad_map.h"
#include "keyboard.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_pb_state.h"
#include "recomp_exit_hook.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir(p, 0755)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* ---- stubs for what the runtime would link ---------------------------- */

void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { (void)mode; }
/* kernel_ramfs.c's switch, which this test does not link. */
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { (void)on; }
/* kernel_language.c's setter (enhance.c), which this test does not link. */
uint32_t xbox_SetLanguage(const char *code);
uint32_t xbox_SetLanguage(const char *code) { (void)code; return 1; }
/* kernel_path.c: no save root in this test, so a chain profile cannot
 * compose (shader_profile_req.c). */
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n) { (void)out; (void)n; return 0; }
/* The overlay (tests/overlay has it); overlay.stats stays off here. */
int nv2a_overlay_arm(uint32_t layers, uint32_t out_h)
{
    (void)layers;
    (void)out_h;
    return -1;
}
void nv2a_overlay_set_stats(int on) { (void)on; }
float nv2a_overlay_set_ui_scale(float mul) { return mul; }
/* The display probe needs a window system. */
void enhance_display_start(void) {}
int nv2a_overlay_stats(void) { return 0; }
/* api_toast's overlay push: the last line, to check the mod's text. */
static char s_toast_last[256];
/* The menu (tests/enhance_menu has it): enhance.c starts it. */
void enhance_menu_start(void);
void enhance_menu_start(void) {}
void nv2a_overlay_toast(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(s_toast_last, sizeof s_toast_last, fmt, ap);
    va_end(ap);
}

/* The guest's memory: a buffer, and an offset that maps guest 0x1000 to
 * its start, as xbox_GetMemoryOffset maps the title's address space. */
static uint8_t s_guest[64];
ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return (ptrdiff_t)((uintptr_t)s_guest - 0x1000u); }
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

/* nv2a_pb_exec.c's present pick, which the batch info carries. */
static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

static uint64_t s_now;
static uint64_t f_clock(void) { return s_now; }
static int f_plugged;
static DWORD f_get(DWORD p, XBOX_INPUT_STATE *s)
{
    if (p != 0 || !f_plugged)
        return ERROR_DEVICE_NOT_CONNECTED;
    memset(s, 0, sizeof *s);
    s->dwPacketNumber = 7;
    s->Gamepad.wButtons = XBOX_GAMEPAD_START;
    s->Gamepad.sThumbLX = 1234;
    return ERROR_SUCCESS;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
/* The backend reads raw; the fake builds its pad as the Duke would see it
 * and hands it over through the identity's inverse. */
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    XBOX_INPUT_STATE s;
    DWORD rc = f_get(p, &s);
    (void)extras;
    if (rc == ERROR_SUCCESS) {
        xbox_PadRawFromGamepad(&s.Gamepad, 0, raw);
        *packet = s.dwPacketNumber;
    }
    return rc;
}
static const xbox_input_backend k_fake = { NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, NULL, NULL, NULL };
/* input_core.c's default; never called here. */
const xbox_input_backend xbox_input_host_backend = { NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

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

/* ---- 1. the table: order, a failed init, frames, shutdown ------------- */

static char s_log[256];
static void say(const char *what)
{
    if (strlen(s_log) + strlen(what) + 2 < sizeof s_log) {
        strcat(s_log, what);
        strcat(s_log, " ");
    }
}

static const struct xbox_mod_api *s_api;
static long long s_speed;
static uint32_t s_seen;

static void alpha_frame(void *u) { (void)u; say("alpha.frame");
    s_seen = XBOX_MOD_MEM32(s_api, 0x1004); }
static void alpha_frame2(void *u) { say((const char *)u); }
static int alpha_init(const struct xbox_mod_api *api)
{
    say("alpha.init");
    s_api = api;
    s_speed = api->cfg_int("mods.alpha.speed", 1);
    api->on_frame(alpha_frame, NULL);
    api->on_frame(alpha_frame2, "alpha.frame2");
    return 0;
}
static void alpha_shutdown(void) { say("alpha.down"); }

static int s_bad_filter_asked;
static void bad_frame(void *u) { (void)u; say("bad.frame"); }
static int bad_filter(const struct nv2a_batch_info *b, void *u)
{
    (void)b; (void)u;
    s_bad_filter_asked++;
    return 1;
}
static int bad_init(const struct xbox_mod_api *api)
{
    say("bad.init");
    api->on_frame(bad_frame, NULL);
    api->batch_filter(bad_filter, NULL);
    api->input_capture(1);
    return -3;
}
static void bad_shutdown(void) { say("bad.down"); }

static void beta_frame(void *u) { (void)u; say("beta.frame"); }
static int beta_init(const struct xbox_mod_api *api)
{
    say("beta.init");
    api->on_frame(beta_frame, NULL);
    return 0;
}
static void beta_shutdown(void) { say("beta.down"); }

static const struct xbox_mod k_alpha = { "alpha", alpha_init, alpha_shutdown, NULL };
static const struct xbox_mod k_bad = { "bad", bad_init, bad_shutdown, NULL };
static const struct xbox_mod k_beta = { "beta", beta_init, beta_shutdown, NULL };
static const struct xbox_mod k_quiet = { "quiet", NULL, NULL, NULL };

static void test_table(void)
{
    static const struct xbox_mod *const mods[] = { &k_alpha, &k_bad, &k_beta, &k_quiet, NULL };
    uint32_t v = 0xC0FFEE11u;

    MKDIR("config");
    write_file("config/config.toml", "[mods]\n\"alpha.speed\" = 3\n");
    xbox_mod_register_table(mods);
    s_log[0] = 0;
    CHECK(xbox_enhance_init(".", NULL) == 0);
    CHECK(strcmp(s_log, "alpha.init bad.init beta.init ") == 0);
    CHECK(s_speed == 3);
    /* The failed init's filter and capture went with it. */
    CHECK(nv2a_batch_filter_installed == NULL);
    CHECK(!xbox_mod_input_captured());
    CHECK(s_api && s_api->version == XBOX_MOD_API_VERSION
          && s_api->size == sizeof(struct xbox_mod_api));
    /* A mod's key is a used key. */
    CHECK(enhance_cfg_report_unused() == 0);
    /* Frames: table order, each mod's in its order, the bad one's gone. */
    memcpy(s_guest + 4, &v, sizeof v);
    s_log[0] = 0;
    xbox_mod_frame();
    CHECK(strcmp(s_log, "alpha.frame alpha.frame2 beta.frame ") == 0);
    CHECK(s_seen == 0xC0FFEE11u);
    XBOX_MOD_MEMF(s_api, 0x1008) = 2.5f;
    {
        float f;
        memcpy(&f, s_guest + 8, sizeof f);
        CHECK(f == 2.5f);
    }
    /* A toast reaches the overlay's TOAST layer with the mod's text. */
    s_toast_last[0] = 0;
    s_api->toast(&k_alpha, "speed %lld", s_speed);
    CHECK(strcmp(s_toast_last, "speed 3") == 0);
    /* Registration is init only. */
    CHECK(s_api->on_frame(beta_frame, NULL) == 0);
    /* Reverse order, the bad one never, once. */
    s_log[0] = 0;
    xbox_mod_shutdown();
    xbox_mod_shutdown();
    CHECK(strcmp(s_log, "beta.down alpha.down ") == 0);
    s_log[0] = 0;
    xbox_mod_frame();
    CHECK(s_log[0] == 0);

    /* No table: nothing runs and nothing is installed. */
    xbox_mod_register_table(NULL);
    CHECK(xbox_mod_init() == 0);
    xbox_mod_frame();
    CHECK(s_log[0] == 0);
    remove("config/config.toml");
}

/* ---- 2. the batch filter through a stub backend ----------------------- */

static struct nv2a_pb_gpu s_g;
static struct nv2a_pb_vsh s_v;
static int s_drawn;
static void stub_on_draw(const struct nv2a_pb_draw *d)
{
    /* What metal_on_draw and d3d11_on_draw do first. */
    if (nv2a_batch_skip(d, &s_g, &s_v))
        return;
    s_drawn++;
}
static const struct nv2a_pb_backend k_stub = { .on_draw = stub_on_draw };

static struct nv2a_batch_info s_last;
static int s_asked;
static uint32_t s_skip_hash;
static int hud_filter(const struct nv2a_batch_info *b, void *u)
{
    s_asked++;
    s_last = *b;
    return b->program && b->program_hash == s_skip_hash && *(int *)u;
}
static int s_hud_on = 1;
static int filter_init(const struct xbox_mod_api *api)
{
    api->batch_filter(hud_filter, &s_hud_on);
    return 0;
}
static const struct xbox_mod k_filter = { "filter", filter_init, NULL, NULL };

static void test_batch_filter(void)
{
    static const struct xbox_mod *const mods[] = { &k_filter, NULL };
    struct nv2a_pb_draw d;

    memset(&s_g, 0, sizeof s_g);
    memset(&s_v, 0, sizeof s_v);
    memset(&d, 0, sizeof d);
    s_v.start = 2;
    s_v.prog[2][0] = 0x1234;
    s_v.prog[2][3] = 1;             /* FINAL */
    s_v.prog_dwords = 4;
    s_v.depth_enable = 1;
    s_g.clip_w = 640;
    s_g.clip_h = 480;
    s_g.color_offset = 0x00ABC000u;
    s_g.blend_enable = 1;
    s_g.blend_sfactor = 0x302;
    s_g.flips = 9;
    d.program = 1;
    d.prim = 5;
    d.idx_count = 6;

    /* None installed: drawn, nothing asked. */
    s_drawn = s_asked = 0;
    k_stub.on_draw(&d);
    CHECK(s_drawn == 1 && s_asked == 0);

    xbox_mod_register_table(mods);
    CHECK(xbox_mod_init() == 1);
    CHECK(nv2a_batch_filter_installed != NULL);
    s_skip_hash = nv2a_vsh_program_hash(&s_v, NULL);
    k_stub.on_draw(&d);
    CHECK(s_drawn == 1 && s_asked == 1);
    CHECK(s_last.program && s_last.program_hash == s_skip_hash);
    CHECK(s_last.prim == 5 && s_last.idx_count == 6 && s_last.draw == &d);
    CHECK(s_last.surface_w == 640 && s_last.surface_h == 480);
    CHECK(s_last.color_offset == 0x00ABC000u && s_last.flip == 9);
    CHECK(s_last.present_offset == 0x00C00000u);
    CHECK(s_last.depth_test == 1 && s_last.blend == 1 && s_last.blend_sfactor == 0x302);

    /* A new upload is a new program: the memoised hash follows it. */
    s_v.prog[2][0] = 0x5678;
    s_v.prog_dwords += 4;
    k_stub.on_draw(&d);
    CHECK(s_drawn == 2 && s_last.program_hash != s_skip_hash);
    CHECK(s_last.program_hash == nv2a_vsh_program_hash(&s_v, NULL));
    s_skip_hash = s_last.program_hash;
    s_hud_on = 0;
    k_stub.on_draw(&d);
    CHECK(s_drawn == 3);

    /* Fixed function: no program, no hash. */
    s_hud_on = 1;
    d.program = 0;
    k_stub.on_draw(&d);
    CHECK(s_drawn == 4 && !s_last.program && s_last.program_hash == 0);

    xbox_mod_shutdown();
    CHECK(nv2a_batch_filter_installed == NULL);
    s_asked = 0;
    d.program = 1;
    k_stub.on_draw(&d);
    CHECK(s_drawn == 5 && s_asked == 0);
}

/* ---- 3. the input capture --------------------------------------------- */

static const struct xbox_mod_api *s_in_api;
static int input_init(const struct xbox_mod_api *api)
{
    s_in_api = api;
    return 0;
}
static const struct xbox_mod k_input = { "input", input_init, NULL, NULL };

static void test_capture(void)
{
    static const struct xbox_mod *const mods[] = { &k_input, NULL };
    struct enhance_input_state st;
    XBOX_INPUT_STATE s;

    xbox_mod_register_table(mods);
    CHECK(xbox_mod_init() == 1);
    xbox_InputSetBackend(&k_fake);
    xbox_InputKeyboardEnable(1);
    xbox_InputMouseEnable(1);
    xbox_InputMouseFocus(0);
    f_plugged = 1;

    /* Before the title's first read: no pad. */
    s_in_api->input_state(&st);
    CHECK(!st.connected && !st.captured);

    /* Not captured: the motion is the look stick's, the mod sees none. */
    xbox_InputMouseMotion(40, 0);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS && s.Gamepad.sThumbRX > 0);
    s_in_api->input_state(&st);
    CHECK(st.connected && st.pad.Gamepad.wButtons == XBOX_GAMEPAD_START
          && st.pad.Gamepad.sThumbLX == 1234 && st.pad.Gamepad.sThumbRX == s.Gamepad.sThumbRX);
    CHECK(st.mouse_dx == 0 && st.mouse_dy == 0);

    /* Captured: the stick is still, the counts are the mod's, raw. */
    xbox_InputMouseMotion(5, 0);        /* before the edge: dropped */
    s_in_api->input_capture(1);
    CHECK(xbox_mod_input_captured());
    xbox_InputMouseMotion(40, -12);
    xbox_InputMouseButton(XBOX_MOUSE_RIGHT, 1);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS && s.Gamepad.sThumbRX == 0
          && s.Gamepad.sThumbRY == 0);
    s_in_api->input_state(&st);
    CHECK(st.captured && st.mouse_dx == 40 && st.mouse_dy == -12);
    CHECK(st.mouse_buttons & (1u << (XBOX_MOUSE_RIGHT - 1)));
    CHECK(st.connected && st.pad.Gamepad.sThumbLX == 1234);
    s_in_api->input_state(&st);
    CHECK(st.mouse_dx == 0 && st.mouse_dy == 0);
    xbox_InputMouseButton(XBOX_MOUSE_RIGHT, 0);

    /* Released: what gathered for the mod does not jump the stick. */
    xbox_InputMouseMotion(40, 0);
    s_in_api->input_capture(0);
    CHECK(!xbox_mod_input_captured());
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS && s.Gamepad.sThumbRX == 0);
    xbox_InputMouseMotion(40, 0);
    CHECK(xbox_InputGetState(0, &s) == ERROR_SUCCESS && s.Gamepad.sThumbRX > 0);
    s_in_api->input_state(&st);
    CHECK(!st.captured && st.mouse_dx == 0);

    /* Shutdown gives a held capture back. */
    s_in_api->input_capture(1);
    xbox_mod_shutdown();
    CHECK(!xbox_mod_input_captured());

    /* Unplugged and the keys and mouse off: the last read says so. */
    f_plugged = 0;
    xbox_InputKeyboardEnable(0);
    xbox_InputMouseEnable(0);
    xbox_InputSetBackend(&k_fake);
    CHECK(xbox_InputGetState(0, &s) == ERROR_DEVICE_NOT_CONNECTED);
    CHECK(xbox_InputLastState(0, &s) == ERROR_DEVICE_NOT_CONNECTED && s.Gamepad.sThumbLX == 0);
    CHECK(xbox_InputLastState(1, &s) == ERROR_DEVICE_NOT_CONNECTED);
    xbox_InputSetBackend(NULL);
}

/* ---- 4. a mod with only a filter: api.mem before any frame ----------- */

static const struct xbox_mod_api *s_fo_api;
static uintptr_t s_fo_mem;
static uint32_t s_fo_read;
static int fo_filter(const struct nv2a_batch_info *b, void *u)
{
    (void)b; (void)u;
    s_fo_mem = s_fo_api->mem;
    s_fo_read = XBOX_MOD_MEM32(s_fo_api, 0x100C);
    return 0;
}
static int fo_init(const struct xbox_mod_api *api)
{
    s_fo_api = api;
    api->batch_filter(fo_filter, NULL);
    return 0;
}
static const struct xbox_mod k_filter_only = { "filteronly", fo_init, NULL, NULL };

static void test_filter_only(void)
{
    static const struct xbox_mod *const mods[] = { &k_filter_only, NULL };
    struct nv2a_pb_draw d;
    uint32_t v = 0x5EED1234u;

    memset(&d, 0, sizeof d);
    memcpy(s_guest + 12, &v, sizeof v);
    xbox_mod_register_table(mods);
    CHECK(xbox_mod_init() == 1);
    CHECK(s_fo_api->mem == 0);      /* not during init */
    s_drawn = 0;
    k_stub.on_draw(&d);             /* no xbox_mod_frame yet */
    CHECK(s_drawn == 1);
    CHECK(s_fo_mem == (uintptr_t)xbox_GetMemoryOffset() && s_fo_read == 0x5EED1234u);
    xbox_mod_shutdown();
}

/* ---- 5. a failed init gives the capture back as it found it ----------- */

static int hold_init(const struct xbox_mod_api *api) { api->input_capture(1); return 0; }
static int letgo_fail_init(const struct xbox_mod_api *api) { api->input_capture(0); return 1; }
static int grab_fail_init(const struct xbox_mod_api *api) { api->input_capture(1); return 1; }
static const struct xbox_mod k_hold = { "hold", hold_init, NULL, NULL };
static const struct xbox_mod k_letgo_fail = { "letgo", letgo_fail_init, NULL, NULL };
static const struct xbox_mod k_grab_fail = { "grab", grab_fail_init, NULL, NULL };

static void test_failed_init_capture(void)
{
    static const struct xbox_mod *const held[] = { &k_hold, &k_letgo_fail, NULL };
    static const struct xbox_mod *const free_[] = { &k_grab_fail, NULL };

    /* An earlier mod holds port 0; a failing one that let go is undone. */
    xbox_mod_register_table(held);
    CHECK(xbox_mod_init() == 1);
    CHECK(xbox_mod_input_captured());
    xbox_mod_shutdown();
    CHECK(!xbox_mod_input_captured());

    /* Nobody held it; a failing one that took it gives it back. */
    xbox_mod_register_table(free_);
    CHECK(xbox_mod_init() == 0);
    CHECK(!xbox_mod_input_captured());
}

/* ---- 6. shutdown runs once, whatever calls it ------------------------- */

static int s_once_down, s_once_frames, s_once_asks;
static void once_frame(void *u) { (void)u; s_once_frames++; }
static int once_filter(const struct nv2a_batch_info *b, void *u) { (void)b; (void)u; s_once_asks++; return 1; }
static int once_init(const struct xbox_mod_api *api)
{
    api->on_frame(once_frame, NULL);
    api->batch_filter(once_filter, NULL);
    api->input_capture(1);
    return 0;
}
static void once_shutdown(void) { s_once_down++; }
static const struct xbox_mod k_once = { "once", once_init, once_shutdown, NULL };

static void test_shutdown_once(void)
{
    static const struct xbox_mod *const mods[] = { &k_once, NULL };
    struct nv2a_pb_draw d;

    memset(&d, 0, sizeof d);
    xbox_mod_register_table(mods);
    CHECK(xbox_mod_init() == 1);
    xbox_mod_frame();
    s_drawn = 0;
    k_stub.on_draw(&d);
    CHECK(s_once_frames == 1 && s_once_asks == 1 && s_drawn == 0);
    /* xbox_mod_shutdown, then the windows' exit hook, then atexit's. */
    xbox_mod_shutdown();
    recomp_exit_hook_run("test");
    xbox_mod_shutdown();
    CHECK(s_once_down == 1);
    CHECK(nv2a_batch_filter_installed == NULL && !xbox_mod_input_captured());
    xbox_mod_frame();
    k_stub.on_draw(&d);
    CHECK(s_once_frames == 1 && s_once_asks == 1 && s_drawn == 1);
}

/* ---- 7. the exit hook table: eight, and a full one says so ----------- */

static int s_hooks_run;
#define HOOK(n) static void hook##n(const char *w) { (void)w; s_hooks_run++; }
HOOK(0) HOOK(1) HOOK(2) HOOK(3) HOOK(4) HOOK(5) HOOK(6) HOOK(7)

static void test_exit_hooks(void)
{
    static const recomp_exit_hook_fn hooks[] = { hook0, hook1, hook2, hook3,
                                                 hook4, hook5, hook6, hook7 };
    int i, added = 0;

    /* mod.c's hook is in already (test_table ran a mod). */
    for (i = 0; i < 8; i++)
        added += recomp_exit_hook_add(hooks[i]);
    CHECK(added == 7);
    CHECK(recomp_exit_hook_add(hook0) == 1);    /* twice keeps one */
    recomp_exit_hook_run("test");
    CHECK(s_hooks_run == 7);
}

/* ---- 8. a mod off by mods.<name> takes its keys' warnings with it ---- */

static void test_off_keys(void)
{
    static const struct xbox_mod *const mods[] = { &k_beta, NULL };

    /* beta never inits, so nothing reads beta.speed, beta.hud or
     * beta_lives; they are the mod's, not typos. A key outside its prefix
     * still warns, and so does one that only shares its letters (betamax). */
    write_file("config/config.toml",
               "beta_lives = 3\nbetamax = 1\n\n[mods]\nbeta = false\n\n[beta]\nspeed = 2\nhud = 1\n"
               "\n[render]\nscael = 2\n");
    enhance_cfg_shutdown();
    CHECK(enhance_cfg_init(".", NULL) >= 1);
    xbox_mod_register_table(mods);
    s_log[0] = 0;
    CHECK(xbox_mod_init() == 0);
    CHECK(s_log[0] == 0);
    CHECK(xbox_mod_state(0) == XBOX_MOD_OFF);
    CHECK(enhance_cfg_report_unused() == 2);    /* betamax and render.scael */
    xbox_mod_shutdown();
    enhance_cfg_shutdown();
}

int main(void)
{
    test_table();
    test_batch_filter();
    test_capture();
    test_filter_only();
    test_failed_init_capture();
    test_shutdown_once();
    test_exit_hooks();
    test_off_keys();
    printf("enhance_mod: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
