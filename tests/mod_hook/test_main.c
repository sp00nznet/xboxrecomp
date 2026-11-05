/*
 * mod_hook test: the mod API v4 as a title runs it through
 * xbox_enhance_init (mod-connectivity-layer 4.1, 4.9 and 5.4).
 *
 * The table: v1 and v3 entries (no needs or follows) load beside v4 ones.
 * The [ENHANCE] mods= token: none without a table and at stock; a follows
 * key at its default keeps a mod out and a changed one lists it, from the
 * user file or the environment; a descriptor's enable switch turned off
 * live takes it out, and a mod inited dormant is not listed; a mod that
 * reports an active state is listed only while it is set.
 *
 * The hooks, through slots declared as the generated code declares them
 * (RECOMP_HOOK_SLOTS, RECOMP_HOOK_CALL) and bodies that do what a lifted
 * body does to the guest registers: pre in table order and post in
 * reverse; ecx and edx at entry; the post's esp the entry's, so it reads
 * the same arguments; a MAY_SKIP skip on a `ret 0x14` function leaving the
 * stack as the body's ret would, and one on an st0 function pushing the
 * float; a second MAY_SKIP on one VA refused; a skip without the flag
 * ignored and logged once, and a later pre sees a skip and cannot clear
 * it; the x87 stack touched only on a slot that returns in st0, whatever
 * a hook writes to st0_valid or st0; a VA without a slot, or an unknown
 * flag, refused; a failed init's hooks dropped; a recursive hooked call;
 * a mod's hooks silent while it is dormant or switched off, and every
 * hook silent after shutdown. Runs in its working directory, where it writes
 * config/config.toml, and writes config.default.toml beside the
 * executable.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "input_backend.h"
#include "input_names.h"
#include "mod.h"
#include "hud_anchor.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_pb_state.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"
#include "xbox_memory_layout.h"

#include <fcntl.h>
#include <stdarg.h>
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
#include <sys/wait.h>
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
/* A line the start printed, or one it must not have. */
#define HAS(out, line) CHECK(strstr((out), (line)) != NULL)
#define LACKS(out, line) CHECK(strstr((out), (line)) == NULL)

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
void nv2a_overlay_toast(const char *fmt, ...) { (void)fmt; }
void enhance_menu_start(void);
void enhance_menu_start(void) {}

/* Guest 0x1000..0x103F: the hooked calls' stacks live here; 0x1040 on,
 * the HUD test's vertices. */
static uint8_t s_guest[256];
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
static struct nv2a_pb_present s_present = { 0x00C00000u, 640, 480, 2560, 0 };
const struct nv2a_pb_present *nv2a_pb_present_state(void) { return &s_present; }

static uint64_t f_clock(void) { return 1000; }
static DWORD f_get_raw(DWORD p, InputRaw *raw, DWORD *packet, int extras)
{
    (void)p; (void)raw; (void)packet; (void)extras;
    return ERROR_DEVICE_NOT_CONNECTED;
}
static DWORD f_set(DWORD p, const XBOX_VIBRATION *v) { (void)p; (void)v; return ERROR_SUCCESS; }
static const xbox_input_backend k_fake = { NULL, f_get_raw, f_set, xbox_InputKbmPoll, f_clock, NULL, NULL, NULL };
const xbox_input_backend xbox_input_host_backend = { NULL, f_get_raw, f_set, NULL, f_clock, NULL, NULL, NULL };

/* ---- files, env and stderr --------------------------------------------- */

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

static const char *defaults_file(void)
{
    static char p[4096];
    snprintf(p, sizeof p, "%s/config.default.toml", recomp_exe_dir());
    return p;
}

static int s_saved_fd = -1;
static char s_cap[65536];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("mod_hook_stderr.txt");
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
    f = fopen("mod_hook_stderr.txt", "rb");
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

/* A fresh title start with this user file and defaults file (NULL: none);
 * the startup lines are returned. */
static const char *start(const char *user, const char *defaults,
                         const struct xbox_mod *const *mods)
{
    xbox_mod_shutdown();
    xbox_InputSetBackend(&k_fake);
    if (user)
        write_file("config/config.toml", user);
    else
        remove("config/config.toml");
    if (defaults)
        write_file(defaults_file(), defaults);
    else
        remove(defaults_file());
    xbox_mod_register_table(mods);
    capture_begin();
    xbox_enhance_init(".", NULL);
    return capture_end();
}

static int index_of(const char *name)
{
    for (int i = 0; i < xbox_mod_count(); i++)
        if (!strcmp(xbox_mod_name(i), name))
            return i;
    return -1;
}

/* The token line as the start or a frame printed it ("" when none). */
static const char *token_of(const char *out, char *buf, size_t n)
{
    const char *p = NULL, *q = out;
    size_t k = 0;
    while ((q = strstr(q, "[ENHANCE] mods=")) != NULL) {
        p = q;
        q++;
    }
    buf[0] = 0;
    if (!p)
        return buf;
    p += strlen("[ENHANCE] mods=");
    while (p[k] && p[k] != '\n' && k + 1 < n) {
        buf[k] = p[k];
        k++;
    }
    buf[k] = 0;
    return buf;
}

static int count_of(const char *out, const char *needle)
{
    int n = 0;
    for (const char *p = out; (p = strstr(p, needle)) != NULL; p++)
        n++;
    return n;
}

/* One frame's output. */
static const char *frame_out(void)
{
    capture_begin();
    xbox_mod_frame();
    return capture_end();
}

/* ---- 1. versions and the mods= token (4.1, 4.9) ----------------------- */

static int ok_init(const struct xbox_mod_api *api) { (void)api; return 0; }

/* A v1 entry: three fields, as tables were written before v3. */
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static const struct xbox_mod k_v1 = { "v1", ok_init, NULL };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* A v3 entry: name, init, shutdown and a descriptor, no needs or follows. */
static const struct xbox_mod_desc k_v3_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "V3", NULL, NULL, NULL,
    NULL, 0, NULL, 0
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#endif
static const struct xbox_mod k_v3 = { "v3", ok_init, NULL, &k_v3_desc };
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif

/* A v4 descriptor mod that needs hooks, its switch on by default and live. */
static void hud_enable(int on, void *u) { (void)on; (void)u; }
static const struct xbox_mod_api *s_hud_api;
static int s_hud_report = 1;
static int hud_init(const struct xbox_mod_api *api)
{
    s_hud_api = api;
    api->on_enable(hud_enable, NULL);
    /* Default on, but nothing to do at 4:3: out of the token. */
    if (s_hud_report)
        api->active(NULL, 0); /* a NULL mod is ignored */
    return 0;
}
static const struct xbox_mod_setting k_hud_set[] = {
    { "enabled", NULL, NULL, NULL, "Enabled", NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 1, 0, 0, 0, NULL },
};
static const struct xbox_mod_desc k_hud_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "HUD", NULL, NULL, NULL,
    k_hud_set, 1, NULL, 0
};
static const struct xbox_mod k_hud = { "hud", hud_init, NULL, &k_hud_desc, XBOX_MOD_NEED_HOOK,
                                       NULL };
static int hud_init_report(const struct xbox_mod_api *api);
static const struct xbox_mod k_hud_r = { "hud", hud_init_report, NULL, &k_hud_desc,
                                         XBOX_MOD_NEED_HOOK, NULL };
static int hud_init_report(const struct xbox_mod_api *api)
{
    s_hud_api = api;
    api->on_enable(hud_enable, NULL);
    api->active(&k_hud_r, 0);
    return 0;
}

/* A v4 mod that follows a key, as cat's free camera follows camera.free:
 * its init runs at stock and does nothing there. */
static const struct xbox_mod k_cam = { "camera.free", ok_init, NULL, NULL, 0, "camera.free" };
/* One that follows a key the defaults file lacks. */
static const struct xbox_mod k_wide = { "wide", ok_init, NULL, NULL, 0, "mods.wide.on" };
/* One that follows a key the environment can set (RECOMP_DISPLAY_ASPECT). */
static const struct xbox_mod k_asp = { "asp", ok_init, NULL, NULL, 0, "display.aspect" };

static void test_token_stock(void)
{
    static const struct xbox_mod *const plain[] = { &k_v1, &k_v3, NULL };
    static const struct xbox_mod *const stock[] = { &k_hud_r, &k_cam, &k_wide, NULL };
    const char *out;
    char tok[256];

    /* No table: the one line, none. */
    out = start(NULL, NULL, NULL);
    CHECK(count_of(out, "[ENHANCE] mods=") == 1);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);

    /* v1 and v3 entries load under API v4 and, running, are listed. */
    out = start(NULL, NULL, plain);
    CHECK(xbox_mod_state(0) == XBOX_MOD_ON && xbox_mod_state(1) == XBOX_MOD_ON);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1,v3") == 0);

    /* Stock: the hud mod on but not active, the free camera's key at its
     * default ("off" spelled as the bool false here), wide unset. */
    out = start(NULL, "[camera]\nfree = false\n", stock);
    CHECK(xbox_mod_state(0) == XBOX_MOD_ON && xbox_mod_state(1) == XBOX_MOD_ON
          && xbox_mod_state(2) == XBOX_MOD_ON);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);
    out = frame_out();
    CHECK(count_of(out, "[ENHANCE] mods=") == 0);

    /* The user sets the key to the same value in another spelling: still
     * the default. */
    out = start("[camera]\nfree = \"off\"\n", "[camera]\nfree = false\n", stock);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);

    /* Turned on: listed. wide too, once anything sets its key. */
    out = start("[camera]\nfree = \"on\"\n[mods]\n\"wide.on\" = 1\n",
                "[camera]\nfree = \"off\"\n", stock);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "camera.free,wide") == 0);

    /* A follows key the environment sets. */
    {
        static const struct xbox_mod *const asp[] = { &k_asp, NULL };
        out = start(NULL, "[display]\naspect = \"4:3\"\n", asp);
        CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);
        recomp_env_set(RENV_DISPLAY_ASPECT, "16:9");
        out = start(NULL, "[display]\naspect = \"4:3\"\n", asp);
        CHECK(strcmp(token_of(out, tok, sizeof tok), "asp") == 0);
        recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    }
    remove(defaults_file());
}

static void test_token_live(void)
{
    static const struct xbox_mod *const mods[] = { &k_hud_r, &k_v1, NULL };
    const char *out;
    char tok[256];
    int hud;

    out = start(NULL, NULL, mods);
    hud = index_of("hud");
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1") == 0);

    /* The hud mod becomes active (a 16:9 frame, say): the next frame says so. */
    s_hud_api->active(&k_hud_r, 1);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "hud,v1") == 0);
    out = frame_out();
    CHECK(count_of(out, "[ENHANCE] mods=") == 0);    /* only on a change */

    /* Its switch turned off live: out, whatever it reports. */
    CHECK(xbox_mod_set_enabled(hud, 0) == XBOX_MOD_OFF);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1") == 0);
    CHECK(xbox_mod_set_enabled(hud, 1) == XBOX_MOD_ON);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "hud,v1") == 0);
    s_hud_api->active(&k_hud_r, 0);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1") == 0);

    /* A descriptor mod that never reports is listed while on. */
    {
        static const struct xbox_mod *const plain[] = { &k_hud, NULL };
        s_hud_report = 0;
        out = start(NULL, NULL, plain);
        CHECK(strcmp(token_of(out, tok, sizeof tok), "hud") == 0);
        s_hud_report = 1;
        /* Switched off by the user file: not inited, not listed. */
        out = start("[mods]\n\"hud.enabled\" = false\n", NULL, plain);
        CHECK(xbox_mod_state(0) == XBOX_MOD_OFF);
        CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);
    }
}

/* ---- 2. the hook slots (5.4) ------------------------------------------ */

/* The guest registers, as xbox_memory_layout.c defines them. */
RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
RECOMP_TLS double g_fp_stack[8];
RECOMP_TLS int g_fp_top;

#define VA_PLAIN 0x00010000u    /* cdecl, returns eax */
#define VA_STD   0x00020000u    /* ret 0x14 */
#define VA_ST0   0x00030000u    /* cdecl, returns on the x87 stack */
#define VA_NONE  0x00040000u    /* no slot */
#define VA_REC   0x00050000u    /* cdecl, calls itself once */

static struct recomp_hook_slot s_slots[] = {
    { VA_PLAIN, 0, 0, 0, NULL },
    { VA_STD, 0x14, 0, 0, NULL },
    { VA_ST0, 0, RECOMP_HOOK_SLOT_ST0, 0, NULL },
    { VA_REC, 0, 0, 0, NULL },
};
RECOMP_HOOK_SLOTS(s_slots, 4)

static char s_log[512];
static void say(const char *what)
{
    if (strlen(s_log) + strlen(what) + 2 < sizeof s_log) {
        strcat(s_log, what);
        strcat(s_log, " ");
    }
}

/* The lifted bodies: what each one's own code does to the registers. */
static void plain_body(void)
{
    say("body");
    g_eax = 7;
    g_edx = 0;
    g_esp += 4;
}
static void std_body(void)
{
    say("std.body");
    g_eax = 1;
    g_esp += 4 + 0x14;
}
static void st0_body(void)
{
    say("st0.body");
    g_fp_top = (g_fp_top + 7) & 7;
    g_fp_stack[g_fp_top] = 1.0 / 3.0;
    g_esp += 4;
}
static void sub_plain(void) { RECOMP_HOOK_CALL(&s_slots[0], plain_body); }
static void sub_std(void) { RECOMP_HOOK_CALL(&s_slots[1], std_body); }
static void sub_st0(void) { RECOMP_HOOK_CALL(&s_slots[2], st0_body); }
static void sub_rec(void);
/* Calls itself once (a pushed return address, the call, the callee's ret),
 * then returns 10 more than the inner call returned. */
static int s_rec_depth;
static void rec_body(void)
{
    say("rec.body");
    if (s_rec_depth++ == 0) {
        g_esp -= 4;
        sub_rec();
        g_eax += 10;
    } else {
        g_eax = 1;
    }
    g_esp += 4;
}
static void sub_rec(void) { RECOMP_HOOK_CALL(&s_slots[3], rec_body); }

/* A call as the caller makes it: arguments pushed right to left, then the
 * return address. Returns the esp the callee was entered with. */
static uint32_t guest_call(void (*fn)(void), const uint32_t *args, int nargs)
{
    g_esp = 0x1038;
    for (int k = nargs - 1; k >= 0; k--) {
        g_esp -= 4;
        memcpy(s_guest + (g_esp - 0x1000), &args[k], 4);
    }
    g_esp -= 4;
    {
        uint32_t ret = 0x00ABCDEFu;
        memcpy(s_guest + (g_esp - 0x1000), &ret, 4);
    }
    uint32_t entry = g_esp;
    fn();
    return entry;
}

static const struct xbox_mod_api *s_hapi;
static uint32_t s_pre_ecx, s_pre_edx, s_pre_esp, s_post_esp, s_pre_arg1, s_post_arg1;
static int s_post_skip, s_post_st0v;

static void a_pre(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("a.pre");
    s_pre_ecx = c->ecx;
    s_pre_edx = c->edx;
    s_pre_esp = c->esp;
    s_pre_arg1 = XBOX_MOD_ARG32(s_hapi, c, 1);
}
static void a_post(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("a.post");
    s_post_esp = c->esp;
    s_post_arg1 = XBOX_MOD_ARG32(s_hapi, c, 1);
    s_post_skip = c->skip;
    s_post_st0v = c->st0_valid;
    c->eax += 100;
}
static void b_pre(struct xbox_mod_call *c, void *u) { (void)c; say((const char *)u); }
static void b_post(struct xbox_mod_call *c, void *u)
{
    (void)c;
    (void)u;
    say("b.post");
}
/* Asks to skip without the flag. */
static void rogue_pre(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("rogue.pre");
    c->skip = 1;
}
static void skip_pre(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("skip.pre");
    c->skip = 1;
    c->eax = 0x55;
    c->edx_ret = 0x66;
    /* Claims an x87 return the function does not have: nothing pushed. */
    c->st0_valid = 1;
    c->st0 = 9.0f;
}
/* A later pre on the skipped VA: sees the skip and the skipper's return,
 * and cannot clear the skip. */
static int s_seen_skip;
static uint32_t s_seen_eax;
static void seen_pre(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("seen.pre");
    s_seen_skip = c->skip;
    s_seen_eax = c->eax;
    c->skip = 0;
}
static int s_st0_skip;
static float s_st0_seen, s_st0_set;
static void st0_pre(struct xbox_mod_call *c, void *u)
{
    (void)u;
    /* Junk in st0 and st0_valid cleared: neither decides anything. */
    c->st0_valid = 0;
    if (!s_st0_skip) {
        c->st0 = 99.0f;
        return;
    }
    c->skip = 1;
    c->st0 = 2.5f;
}
static void st0_post(struct xbox_mod_call *c, void *u)
{
    (void)u;
    s_st0_seen = c->st0_valid ? c->st0 : -1.0f;
    if (s_st0_set != 0)
        c->st0 = s_st0_set;
}
static void r_pre(struct xbox_mod_call *c, void *u)
{
    (void)c;
    (void)u;
    say("r.pre");
}
static void r_post(struct xbox_mod_call *c, void *u)
{
    (void)u;
    say("r.post");
    c->eax += 1;
}
static void never(struct xbox_mod_call *c, void *u)
{
    (void)c;
    (void)u;
    say("never");
}

static int s_rc_second, s_rc_none, s_rc_nopre, s_rc_flags;
static int ha_init(const struct xbox_mod_api *api)
{
    s_hapi = api;
    CHECK(api->version >= 4u && api->size >= offsetof(struct xbox_mod_api, active));
    CHECK(api->hook(VA_PLAIN, a_pre, a_post, 0, NULL) == 1);
    CHECK(api->hook(VA_STD, NULL, a_post, 0, NULL) == 1);
    CHECK(api->hook(VA_REC, r_pre, r_post, 0, NULL) == 1);
    return 0;
}
static int hb_init(const struct xbox_mod_api *api)
{
    CHECK(api->hook(VA_PLAIN, b_pre, b_post, 0, "b.pre") == 1);
    CHECK(api->hook(VA_PLAIN, rogue_pre, NULL, 0, NULL) == 1);
    CHECK(api->hook(VA_STD, skip_pre, NULL, XBOX_MOD_HOOK_MAY_SKIP, NULL) == 1);
    CHECK(api->hook(VA_ST0, st0_pre, st0_post, XBOX_MOD_HOOK_MAY_SKIP, NULL) == 1);
    return 0;
}
static int hc_init(const struct xbox_mod_api *api)
{
    /* A second replacement of VA_STD, and a VA the game gave no slot. */
    s_rc_second = api->hook(VA_STD, skip_pre, NULL, XBOX_MOD_HOOK_MAY_SKIP, NULL);
    s_rc_none = api->hook(VA_NONE, b_pre, NULL, 0, "none");
    s_rc_nopre = api->hook(VA_PLAIN, NULL, NULL, 0, NULL);
    s_rc_flags = api->hook(VA_PLAIN, b_pre, NULL, 0x2u, "flags");
    CHECK(api->hook(VA_STD, seen_pre, NULL, 0, NULL) == 1);
    return 0;
}
/* Hooks, then fails: none of them runs. */
static int hfail_init(const struct xbox_mod_api *api)
{
    CHECK(api->hook(VA_PLAIN, never, never, 0, NULL) == 1);
    return 1;
}
static const struct xbox_mod k_ha = { "ha", ha_init, NULL, NULL, XBOX_MOD_NEED_HOOK, NULL };
static const struct xbox_mod k_hb = { "hb", hb_init, NULL, NULL, XBOX_MOD_NEED_HOOK, NULL };
static const struct xbox_mod k_hc = { "hc", hc_init, NULL, NULL, XBOX_MOD_NEED_HOOK, NULL };
static const struct xbox_mod k_hfail = { "hfail", hfail_init, NULL, NULL, XBOX_MOD_NEED_HOOK,
                                         NULL };

static void test_hooks(void)
{
    static const struct xbox_mod *const mods[] = { &k_ha, &k_hfail, &k_hb, &k_hc, NULL };
    const char *out;
    uint32_t args[5] = { 0x11111111u, 0x22222222u, 0x33333333u, 0x44444444u, 0x55555555u };
    uint32_t entry;

    /* Unhooked: the body alone, nothing in between. */
    out = start(NULL, NULL, NULL);
    CHECK(s_slots[0].chain == NULL && s_slots[1].chain == NULL);
    s_log[0] = 0;
    guest_call(sub_plain, args, 2);
    CHECK(strcmp(s_log, "body ") == 0 && g_eax == 7);

    out = start(NULL, NULL, mods);
    HAS(out, "[MOD hc] hook 0x00020000: refused, hb already replaces this function");
    HAS(out, "[MOD hc] hook 0x00040000: no hook slot");
    HAS(out, "[MOD] hfail: init returned 1; left out");
    HAS(out, "[MOD hc] hook 0x00010000: refused, unknown flags 0x2");
    HAS(out, "[MOD] 4 hooked functions");
    CHECK(s_rc_second == 0 && s_rc_none == 0 && s_rc_nopre == 0 && s_rc_flags == 0);
    CHECK(s_slots[0].chain && s_slots[1].chain && s_slots[2].chain && s_slots[3].chain);
    /* Init only. */
    CHECK(s_hapi->hook(VA_PLAIN, b_pre, NULL, 0, NULL) == 0);

    /* Pre in table order, post in reverse; the rogue skip ignored, the
     * body run; the entry ecx, edx and esp; the post reads the same
     * argument and changes the return. */
    s_log[0] = 0;
    g_ecx = 0xCCCCu;
    g_edx = 0xDDDDu;
    capture_begin();
    entry = guest_call(sub_plain, args, 2);
    out = capture_end();
    CHECK(strcmp(s_log, "a.pre b.pre rogue.pre body b.post a.post ") == 0);
    CHECK(s_pre_ecx == 0xCCCCu && s_pre_edx == 0xDDDDu);
    CHECK(s_pre_esp == entry && s_post_esp == entry);
    CHECK(s_pre_arg1 == 0x22222222u && s_post_arg1 == 0x22222222u);
    CHECK(g_eax == 107 && g_esp == entry + 4 && s_post_skip == 0);
    CHECK(count_of(out, "[MOD hb] hook 0x00010000: skip ignored") == 1);
    /* Logged once. */
    capture_begin();
    guest_call(sub_plain, args, 2);
    out = capture_end();
    CHECK(count_of(out, "skip ignored") == 0);

    /* A skip on `ret 0x14`: the body never runs, the stack is as its ret
     * leaves it, the later pre and the post see the skip and the eax set;
     * the later pre's clearing of skip is ignored. The function returns
     * nothing in st0, so the skipper's st0_valid pushes nothing and the
     * post sees st0_valid reset. */
    s_log[0] = 0;
    g_fp_top = 3;
    entry = guest_call(sub_std, args, 5);
    CHECK(strcmp(s_log, "skip.pre seen.pre a.post ") == 0);
    CHECK(s_seen_skip == 1 && s_seen_eax == 0x55);
    CHECK(g_esp == entry + 4 + 0x14 && g_esp == 0x1038);
    CHECK(g_eax == 0x55 + 100 && g_edx == 0x66 && s_post_skip == 1);
    CHECK(g_fp_top == 3 && s_post_st0v == 0);

    /* Recursion: each level runs its own chain, the inner one inside the
     * outer body; the stack balances. */
    s_log[0] = 0;
    s_rec_depth = 0;
    entry = guest_call(sub_rec, NULL, 0);
    CHECK(strcmp(s_log, "r.pre rec.body r.pre rec.body r.post r.post ") == 0);
    CHECK(g_eax == (1 + 1) + 10 + 1 && g_esp == entry + 4);

    /* st0: unhooked return values pass through as the body left them,
     * double precision kept, though the pre left junk in st0 and cleared
     * st0_valid; a post's change is written back. */
    g_fp_top = 0;
    s_st0_skip = 0;
    s_st0_set = 0;
    guest_call(sub_st0, NULL, 0);
    CHECK(g_fp_top == 7 && g_fp_stack[7] == 1.0 / 3.0);
    CHECK(s_st0_seen == (float)(1.0 / 3.0));
    s_st0_set = 4.0f;
    guest_call(sub_st0, NULL, 0);
    CHECK(g_fp_top == 6 && g_fp_stack[6] == 4.0);
    /* Skipped: the float pushed, as the body's return would be. */
    s_st0_set = 0;
    s_st0_skip = 1;
    s_log[0] = 0;
    entry = guest_call(sub_st0, NULL, 0);
    CHECK(s_log[0] == 0);
    CHECK(g_fp_top == 5 && g_fp_stack[5] == 2.5 && g_esp == entry + 4);
    CHECK(s_st0_seen == 2.5f);

    /* Shutdown: no callback from then on, the bodies still run. */
    xbox_mod_shutdown();
    CHECK(s_slots[0].chain == NULL && s_slots[3].chain == NULL);
    s_log[0] = 0;
    guest_call(sub_plain, args, 2);
    CHECK(strcmp(s_log, "body ") == 0 && g_eax == 7);

    /* A restart drops every hook. */
    out = start(NULL, NULL, NULL);
    CHECK(s_slots[0].chain == NULL && s_slots[1].chain == NULL && s_slots[2].chain == NULL);
}

/* ---- 3. hooks follow the mod's state ---------------------------------- */

static void hd_enable(int on, void *u) { (void)on; (void)u; }
static void hd_pre(struct xbox_mod_call *c, void *u) { (void)c; (void)u; say("hd.pre"); }
static void hd_post(struct xbox_mod_call *c, void *u) { (void)c; (void)u; say("hd.post"); }
static int hd_init(const struct xbox_mod_api *api)
{
    api->on_enable(hd_enable, NULL);
    CHECK(api->hook(VA_PLAIN, hd_pre, hd_post, 0, NULL) == 1);
    return 0;
}
/* Off by default and live, so with the menu on it inits dormant. */
static const struct xbox_mod_setting k_hd_set[] = {
    { "enabled", NULL, NULL, NULL, "Enabled", NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_desc k_hd_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "Hooked", NULL, NULL, NULL,
    k_hd_set, 1, NULL, 0
};
static const struct xbox_mod k_hd = { "hd", hd_init, NULL, &k_hd_desc, XBOX_MOD_NEED_HOOK,
                                      NULL };

static void test_hook_state(void)
{
    static const struct xbox_mod *const mods[] = { &k_hd, NULL };
    uint32_t args[2] = { 1, 2 };
    const char *out;
    char tok[256];
    int hd;

    /* Inited dormant: hooked, but silent and not listed. */
    out = start("[menu]\nenabled = true\n", NULL, mods);
    hd = index_of("hd");
    CHECK(xbox_mod_state(hd) == XBOX_MOD_OFF);
    CHECK(s_slots[0].chain != NULL);
    CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);
    s_log[0] = 0;
    guest_call(sub_plain, args, 2);
    CHECK(strcmp(s_log, "body ") == 0);

    /* Turned on live: the hooks run. */
    CHECK(xbox_mod_set_enabled(hd, 1) == XBOX_MOD_ON);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "hd") == 0);
    s_log[0] = 0;
    guest_call(sub_plain, args, 2);
    CHECK(strcmp(s_log, "hd.pre body hd.post ") == 0);

    /* Switched off live: silent again. */
    CHECK(xbox_mod_set_enabled(hd, 0) == XBOX_MOD_OFF);
    s_log[0] = 0;
    guest_call(sub_plain, args, 2);
    CHECK(strcmp(s_log, "body ") == 0);
    out = frame_out();
    CHECK(strcmp(token_of(out, tok, sizeof tok), "none") == 0);
}

/* ---- 3. the HUD table (hud-anchors 1.1, 1.8) ----------------------- */

static const char k_hud_toml[] =
    "version = 1\n[canvas]\nw = 640\nh = 480\norigin = \"top-left\"\n"
    "[layout]\nstride = 16\nx_off = 0\n"
    "[[wrapper]]\nva = 0x00010000\n"
    "[[site]]\nret_va = 0x2000\nelement = \"score\"\nkind = \"left\"\n";
static int s_hud_rc, s_hud_rc2, s_hud_fail;
static int s_hud_off_in_pre, s_hud_flip_in_pre, s_hud_moved, s_hud_restored;
static const struct xbox_mod k_hudt;
static const struct xbox_mod_api *s_hud_api;

/* The mod's hook on its vertex call (VA_STD): four 16-byte vertices at
 * 0x1040, applied with RESTORE and put back at the post. */
static int s_hud_pres, s_hud_cap_seen;

static void hudt_pre(struct xbox_mod_call *c, void *u)
{
    struct xbox_hud_call d = { 0x1040u, 4, XBOX_HUD_RESTORE, 0, 0 };
    uint8_t before[64];
    (void)u;
    s_hud_pres++;
    s_hud_cap_seen = s_hud_api->hud_capturing(&k_hudt);
    memcpy(before, s_guest + 0x40, sizeof before);
    s_hud_api->hud_apply(c, &d);
    s_hud_moved = memcmp(before, s_guest + 0x40, sizeof before) != 0;
    if (s_hud_off_in_pre)       /* the Mods page, between the pre and the post */
        CHECK(xbox_mod_set_enabled(index_of("hudt"), 0) == XBOX_MOD_OFF);
    if (s_hud_flip_in_pre)      /* and a frame ends before the post */
        frame_out();
}
static void hudt_post(struct xbox_mod_call *c, void *u)
{
    (void)u;
    s_hud_restored++;
    s_hud_api->hud_restore(c);
}

static int hudt_init(const struct xbox_mod_api *api)
{
    CHECK(api->size >= offsetof(struct xbox_mod_api, hud_capturing) + sizeof api->hud_capturing);
    s_hud_api = api;
    s_hud_rc = api->hud_table(k_hud_toml, sizeof k_hud_toml - 1);
    CHECK(api->hook(VA_STD, hudt_pre, hudt_post, 0, NULL) == 1);
    api->on_enable(hud_enable, NULL);     /* switched live */
    return s_hud_fail;
}
static int hudt2_init(const struct xbox_mod_api *api)
{
    s_hud_rc2 = api->hud_table(k_hud_toml, sizeof k_hud_toml - 1);
    return 0;
}
static const struct xbox_mod k_hudt = { "hudt", hudt_init, NULL, NULL, 0, NULL };
static const struct xbox_mod k_hudt2 = { "hudt2", hudt2_init, NULL, NULL, 0, NULL };

static void test_hud_table(void)
{
    static const struct xbox_mod *const one[] = { &k_hudt, &k_v1, NULL };
    static const struct xbox_mod *const two[] = { &k_hudt, &k_hudt2, NULL };
    const char *out;
    char tok[256];

    /* 4:3: registered, the keys named, the owner not listed. */
    out = start(NULL, NULL, one);
    CHECK(s_hud_rc == 1 && hud_layer_has_table() && !hud_layer_active());
    CHECK(strstr(out, "[hud] table: 1 sites, 1 groups, 1 wrappers\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] display.hud_aspect") == NULL);     /* no pad: no line */
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1") == 0);

    /* 16:9: listed from the start line, with no call to active. */
    out = start("[display]\naspect = \"16:9\"\nhud_margin = 7\nhud_aspect = \"3:4\"\n", NULL,
                one);
    CHECK(hud_layer_active() && hud_layer_pad() == 107 && hud_layer_pad_hud() == 0);
    CHECK(strstr(out, "display.hud_aspect=3:4 is narrower than 4:3; using 4:3\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] display.hud_aspect=4:3 display.hud_margin=7 hud_pad=0\n"));
    CHECK(strcmp(token_of(out, tok, sizeof tok), "hudt,v1") == 0);
    out = start("[display]\naspect = \"16:9\"\nhud_margin = 99\n", NULL, one);
    CHECK(strstr(out, "display.hud_margin=99 out of range 0..64; using 64\n") != NULL);
    CHECK(hud_layer_pad_hud() == 107 - 64);
    /* The owner switched off live: out at the next frame, the lift off. */
    CHECK(xbox_mod_set_enabled(index_of("hudt"), 0) == XBOX_MOD_OFF);
    out = frame_out();
    CHECK(!hud_layer_active());
    CHECK(strcmp(token_of(out, tok, sizeof tok), "v1") == 0);
    CHECK(xbox_mod_set_enabled(index_of("hudt"), 1) == XBOX_MOD_ON);
    out = frame_out();
    CHECK(hud_layer_active());
    CHECK(strcmp(token_of(out, tok, sizeof tok), "hudt,v1") == 0);

    /* RESTORE through the hook: the bytes come back whole, whether the
     * owner is switched off between the pre and the post (its hooks follow
     * the switch at the next frame, so the post still runs) or a frame
     * ends there too (the layer undoes the pre itself). */
    {
        static const float q[16] = { 600, 10, 0, 0, 700, 10, 1, 0,
                                     700, 30, 1, 1, 600, 30, 0, 1 };
        const uint32_t args[5] = { 1, 2, 3, 4, 5 };
        uint8_t want[64];
        memcpy(s_guest + 0x40, q, sizeof q);
        memcpy(want, s_guest + 0x40, sizeof want);
        s_hud_restored = 0;
        guest_call(sub_std, args, 5);
        CHECK(s_hud_moved && s_hud_restored == 1);
        CHECK(memcmp(want, s_guest + 0x40, sizeof want) == 0);
        s_hud_off_in_pre = 1;
        s_hud_restored = 0;
        guest_call(sub_std, args, 5);
        s_hud_off_in_pre = 0;
        CHECK(s_hud_moved && s_hud_restored == 1);
        CHECK(memcmp(want, s_guest + 0x40, sizeof want) == 0);
        frame_out();
        CHECK(xbox_mod_set_enabled(index_of("hudt"), 1) == XBOX_MOD_ON);
        frame_out();
        s_hud_off_in_pre = s_hud_flip_in_pre = 1;
        s_hud_restored = 0;
        guest_call(sub_std, args, 5);
        s_hud_off_in_pre = s_hud_flip_in_pre = 0;
        CHECK(s_hud_moved && s_hud_restored == 0);
        CHECK(memcmp(want, s_guest + 0x40, sizeof want) == 0);
        CHECK(xbox_mod_set_enabled(index_of("hudt"), 1) == XBOX_MOD_ON);
        frame_out();
    }

    /* A live switch lands at the flip, not mid-frame: the rest of
     * the frame's calls keep the state the frame began with, as the layer
     * does, so no frame is drawn half moved. */
    {
        const uint32_t args[5] = { 1, 2, 3, 4, 5 };
        CHECK(hud_layer_active());
        CHECK(xbox_mod_set_enabled(index_of("hudt"), 0) == XBOX_MOD_OFF);
        s_hud_pres = 0;
        guest_call(sub_std, args, 5);
        CHECK(s_hud_pres == 1 && s_hud_moved && hud_layer_active());
        frame_out();
        s_hud_pres = 0;
        guest_call(sub_std, args, 5);
        CHECK(s_hud_pres == 0 && !hud_layer_active());
        CHECK(xbox_mod_set_enabled(index_of("hudt"), 1) == XBOX_MOD_ON);
        guest_call(sub_std, args, 5);
        CHECK(s_hud_pres == 0 && !hud_layer_active());
        frame_out();
        guest_call(sub_std, args, 5);
        CHECK(s_hud_pres == 1 && s_hud_moved && hud_layer_active());
    }

    /* One table per run: a second mod's is refused. */
    out = start(NULL, NULL, two);
    CHECK(s_hud_rc == 1 && s_hud_rc2 < 0);
    CHECK(strstr(out, "[hud] table: a table is already registered\n") != NULL);

    /* An owner whose init fails takes its table with it. */
    s_hud_fail = 3;
    out = start("[display]\naspect = \"16:9\"\n", NULL, one);
    CHECK(s_hud_rc == 1 && !hud_layer_has_table() && !hud_layer_active());
    CHECK(strstr(out, "[ENHANCE] display.hud_aspect") == NULL);
    s_hud_fail = 0;
}

/* The owner's hooks under a mapping capture (hud_capture.h): they run with
 * the mod off too, and hud_capturing says so only then. */
static void test_hud_capturing(void)
{
    static const struct xbox_mod *const one[] = { &k_hudt, NULL };
    static const float q[16] = { 600, 10, 0, 0, 700, 10, 1, 0,
                                 700, 30, 1, 1, 600, 30, 0, 1 };
    const uint32_t args[5] = { 1, 2, 3, 4, 5 };
    uint8_t want[64];

    MKDIR("hud_capture_out");
    recomp_env_set(RENV_HUD_CAPTURE, "hud_capture_out");
    recomp_env_set(RENV_HUD_CAPTURE_AT, "1:2");
    start("[display]\naspect = \"16:9\"\n", NULL, one);
    memcpy(s_guest + 0x40, q, sizeof q);
    memcpy(want, s_guest + 0x40, sizeof want);
    frame_out();

    /* On: its hooks run as always, and not for the capture. */
    s_hud_pres = 0;
    s_hud_cap_seen = -1;
    guest_call(sub_std, args, 5);
    CHECK(s_hud_pres == 1 && s_hud_cap_seen == 0);

    /* Off: the capture alone runs them, and they are told. Nothing moves. */
    CHECK(xbox_mod_set_enabled(index_of("hudt"), 0) == XBOX_MOD_OFF);
    frame_out();
    s_hud_pres = 0;
    s_hud_cap_seen = -1;
    guest_call(sub_std, args, 5);
    CHECK(s_hud_pres == 1 && s_hud_cap_seen != 0);
    CHECK(!s_hud_moved && memcmp(want, s_guest + 0x40, sizeof want) == 0);
    CHECK(s_hud_api->hud_capturing(NULL) == 0);

    /* The window done (frames 1..2): the hooks follow the mod again. */
    frame_out();
    frame_out();
    s_hud_pres = 0;
    guest_call(sub_std, args, 5);
    CHECK(s_hud_pres == 0 && s_hud_api->hud_capturing(&k_hudt) == 0);

    /* No capture: off means not called, and never told. */
    recomp_env_set(RENV_HUD_CAPTURE, NULL);
    recomp_env_set(RENV_HUD_CAPTURE_AT, NULL);
    start("[display]\naspect = \"16:9\"\n", NULL, one);
    CHECK(xbox_mod_set_enabled(index_of("hudt"), 0) == XBOX_MOD_OFF);
    frame_out();
    s_hud_pres = 0;
    guest_call(sub_std, args, 5);
    CHECK(s_hud_pres == 0 && s_hud_api->hud_capturing(&k_hudt) == 0);
    CHECK(xbox_mod_set_enabled(index_of("hudt"), 1) == XBOX_MOD_ON);
    frame_out();
}

int main(void)
{
    MKDIR("config");
    xbox_InputSetBackend(&k_fake);
    test_token_stock();
    test_token_live();
    test_hooks();
    test_hook_state();
    test_hud_table();
    test_hud_capturing();
    xbox_mod_shutdown();
    remove("config/config.toml");
    printf("mod_hook: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
