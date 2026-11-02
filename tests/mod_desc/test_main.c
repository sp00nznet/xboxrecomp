/*
 * mod_desc test: the mod descriptor (mod.h v3; mod-settings D1-D4, D9) as a
 * title runs it through xbox_enhance_init: a descriptor's checks and each
 * refusal, a v2 mod beside it, a short `size`, the settings' precedence
 * rows (env, user key and alias, defaults key and alias, default) with
 * clamp, snap and the enum fallback and their [MOD] lines, the pending
 * rewrites, an env-forced value, on_setting deferred to the frame, the init
 * rule and the [ENHANCE] enable line, the ACTION controls' ids, unknown
 * keys, and the RECOMP_MODS_DESCRIBE dump against expected.json. Runs in
 * its working directory, where it writes config/config.toml, and writes
 * config.default.toml beside the executable.
 */
#include "enhance.h"
#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "input_backend.h"
#include "input_names.h"
#include "mod.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_pb_state.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"

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
int nv2a_overlay_stats(void) { return 0; }
void nv2a_overlay_toast(const char *fmt, ...) { (void)fmt; }
void enhance_menu_start(void);
void enhance_menu_start(void) {}

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

static char *read_all(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (b) {
        n = (long)fread(b, 1, (size_t)n, f);
        b[n] = 0;
    }
    fclose(f);
    return b;
}

static const char *defaults_file(void)
{
    static char p[4096];
    snprintf(p, sizeof p, "%s/config.default.toml", recomp_exe_dir());
    return p;
}

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

static int s_saved_fd = -1;
static char s_cap[65536];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("mod_desc_stderr.txt");
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
    f = fopen("mod_desc_stderr.txt", "rb");
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

/* ---- the mods ---------------------------------------------------------- */

static const char *const k_offon[] = { "off", "on", NULL };
static const char *const k_hud[] = { "keep", "hide", NULL };
static const char *const k_speed_alias[] = { "fly.old_speed", NULL };
static const char *const k_toggle_def[] = { "F6", NULL };
static const char *const k_move_def[] = { "left_stick", NULL };
static const char *const k_move_alias[] = { "fly.move_keys", NULL };

/* "fly": the free camera's shape. Its switch is a legacy key that only
 * applies at restart, so with the menu on it is still not inited dormant. */
static const struct xbox_mod_setting k_fly_set[] = {
    { "enabled", "fly.enabled", NULL, NULL, "Free camera", NULL, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE_RESTART, XBOX_SET_ENABLE, 0, 0, 0, 0, k_offon },
    { "speed", NULL, "RECOMP_APU_HRTF", k_speed_alias, "Speed", "Fly speed", "x",
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 1, 0.05, 20, 0.05, NULL },
    { "count", NULL, NULL, NULL, "Count", NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE_RESTART, 0, 3, 1, 10, 0, NULL },
    { "hud", NULL, NULL, NULL, "HUD \"while flying\"", NULL, NULL,
      XBOX_SET_ENUM, ENHANCE_LIVE_ACTIVATE, 0, 0, 0, 0, 0, k_hud },
    { "smooth", NULL, NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, 0, 1, 0, 0, 0, NULL },
};
static const struct xbox_mod_control k_fly_ctl[] = {
    { "toggle", "Toggle", NULL, NULL, XBOX_CTL_ACTION, k_toggle_def, NULL, "RECOMP_EXT_VMA" },
    { "move", "Move", NULL, "while flying", XBOX_CTL_STICK, k_move_def, k_move_alias, NULL },
    { "look", "Mouse: look", NULL, NULL, XBOX_CTL_INFO, NULL, NULL, NULL },
};
static const struct xbox_mod_desc k_fly_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc),
    "Free camera", "Fly the camera", "tests", "1.0",
    k_fly_set, 5, k_fly_ctl, 3
};

/* "probe": six floats, one per precedence row (a..f), no switch. */
#define PROBE(n, al) { n, NULL, NULL, al, NULL, NULL, NULL, \
    XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 0, 0, 100, 0, NULL }
static const char *const k_al_c[] = { "probe.old_c", NULL };
static const char *const k_al_e[] = { "probe.old_e", NULL };
static const struct xbox_mod_setting k_probe_set[] = {
    { "a", NULL, "RECOMP_HEAP_QUARANTINE", NULL, NULL, NULL, NULL,
      XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 0, 0, 100, 0, NULL },
    PROBE("b", NULL), PROBE("c", k_al_c), PROBE("d", NULL), PROBE("e", k_al_e),
    PROBE("f", NULL),
    { "slow", NULL, NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_INT, ENHANCE_LIVE_RESTART, 0, 2, 0, 9, 1, NULL },
};
static const struct xbox_mod_desc k_probe_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "Probe", NULL, NULL, NULL,
    k_probe_set, 7, NULL, 0
};

/* "dorm": a live switch, off; inited dormant when the menu is on. */
static const struct xbox_mod_setting k_dorm_set[] = {
    { "on", NULL, NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, NULL },
};
static const char *const k_dorm_def[] = { "F7", NULL };
static const struct xbox_mod_control k_dorm_ctl[] = {
    { "go", "Go", NULL, NULL, XBOX_CTL_ACTION, k_dorm_def, NULL, NULL },
};
static const struct xbox_mod_desc k_dorm_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), "Dormant", NULL, NULL, NULL,
    k_dorm_set, 1, k_dorm_ctl, 1
};

/* Refusals: one fault each. */
static const struct xbox_mod_setting k_bad_lohi[] = {
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_FLOAT, ENHANCE_LIVE, 0, 5, 9, 1, 0, NULL },
};
static const struct xbox_mod_setting k_bad_dup[] = {
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, 0, 0, 0, 0, 0, NULL },
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, 0, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_setting k_bad_enum[] = {
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_ENUM, ENHANCE_LIVE, 0, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_setting k_bad_kind[] = {
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, 9, ENHANCE_LIVE, 0, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_setting k_bad_enable[] = {
    { "x", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE,
      0, 0, 0, 0, NULL },
    { "y", NULL, NULL, NULL, NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE,
      0, 0, 0, 0, NULL },
};
static const struct xbox_mod_setting k_bad_env[] = {
    { "x", NULL, "RECOMP_NO_SUCH_ROW", NULL, NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, 0,
      0, 0, 0, 0, NULL },
};
/* Full keys of 64 bytes or more: enhance_cfg could not bind their env row. */
static const struct xbox_mod_setting k_bad_keylen[] = {
    { "x", "a.key.that.is.far.too.long.to.bind.an.env.row.to.it.at.all.ever.x", NULL, NULL,
      NULL, NULL, NULL, XBOX_SET_BOOL, ENHANCE_LIVE, 0, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_control k_bad_ctl_keylen[] = {
    { "a_control_id_long_enough_to_pass_sixty_four_bytes", NULL, NULL, NULL, XBOX_CTL_INFO,
      NULL, NULL, NULL },
};
static const struct xbox_mod_control k_bad_ctl_dup[] = {
    { "x", NULL, NULL, NULL, XBOX_CTL_INFO, NULL, NULL, NULL },
    { "x", NULL, NULL, NULL, XBOX_CTL_INFO, NULL, NULL, NULL },
};
static const struct xbox_mod_control k_bad_ctl_kind[] = {
    { "x", NULL, NULL, NULL, 7, NULL, NULL, NULL },
};
#define BAD_DESC(set, nset, ctl, nctl) { XBOX_MOD_DESC_VERSION, \
    sizeof(struct xbox_mod_desc), NULL, NULL, NULL, NULL, set, nset, ctl, nctl }
static const struct xbox_mod_desc k_d_lohi = BAD_DESC(k_bad_lohi, 1, NULL, 0);
static const struct xbox_mod_desc k_d_dup = BAD_DESC(k_bad_dup, 2, NULL, 0);
static const struct xbox_mod_desc k_d_enum = BAD_DESC(k_bad_enum, 1, NULL, 0);
static const struct xbox_mod_desc k_d_kind = BAD_DESC(k_bad_kind, 1, NULL, 0);
static const struct xbox_mod_desc k_d_enable = BAD_DESC(k_bad_enable, 2, NULL, 0);
static const struct xbox_mod_desc k_d_env = BAD_DESC(k_bad_env, 1, NULL, 0);
static const struct xbox_mod_desc k_d_ctl_dup = BAD_DESC(NULL, 0, k_bad_ctl_dup, 2);
static const struct xbox_mod_desc k_d_ctl_kind = BAD_DESC(NULL, 0, k_bad_ctl_kind, 1);
static const struct xbox_mod_desc k_d_keylen = BAD_DESC(k_bad_keylen, 1, NULL, 0);
static const struct xbox_mod_desc k_d_ctl_keylen = BAD_DESC(NULL, 0, k_bad_ctl_keylen, 1);
static const struct xbox_mod_desc k_d_ver0 = { 0, sizeof(struct xbox_mod_desc),
    NULL, NULL, NULL, NULL, NULL, 0, NULL, 0 };
static const struct xbox_mod_desc k_d_tiny = { XBOX_MOD_DESC_VERSION, 4,
    NULL, NULL, NULL, NULL, NULL, 0, NULL, 0 };
/* Written by a build that knew no controls: its size stops before them, so
 * the bad table behind it is never read. */
static const struct xbox_mod_desc k_d_short = { XBOX_MOD_DESC_VERSION,
    (uint32_t)offsetof(struct xbox_mod_desc, controls), "Short", NULL, NULL, NULL,
    k_dorm_set, 1, k_bad_ctl_dup, 2 };

/* What the mods' code did: init and frame counts per mod, by name. */
static char s_log[1024];
static void say(const char *what)
{
    if (strlen(s_log) + strlen(what) + 2 < sizeof s_log) {
        strcat(s_log, what);
        strcat(s_log, " ");
    }
}

static const struct xbox_mod_api *s_api;
static int s_toggle_id = -2, s_move_id = -2, s_look_id = -2;
static double s_init_speed;
static uint32_t s_cb_idx = 99;
static double s_cb_val;
static int s_cb_n;

static void fly_frame(void *u) { (void)u; say("fly.frame"); }
static void fly_setting(uint32_t idx, double v, void *u)
{
    (void)u;
    s_cb_idx = idx;
    s_cb_val = v;
    s_cb_n++;
}
static int fly_init(const struct xbox_mod_api *api)
{
    struct xbox_ctl_value cv;
    say("fly.init");
    s_api = api;
    s_init_speed = api->setting(&k_fly_desc, 1);
    s_toggle_id = api->control_action(&k_fly_desc, 0);
    s_move_id = api->control_action(&k_fly_desc, 1);
    s_look_id = api->control_action(&k_fly_desc, 2);
    api->on_frame(fly_frame, NULL);
    api->on_setting(&k_fly_desc, fly_setting, NULL);
    memset(&cv, 0xff, sizeof cv);
    CHECK(api->control_value(&k_fly_desc, 1, &cv) == 0 && cv.connected == 0 && cv.raw[0] == 0);
    return 0;
}
static int probe_init(const struct xbox_mod_api *api)
{
    say("probe.init");
    api->on_frame(fly_frame, NULL);
    api->on_setting(&k_probe_desc, fly_setting, NULL);
    return 0;
}
static void dorm_enable(int on, void *u) { (void)u; say(on ? "dorm.on" : "dorm.off"); }
static int dorm_init(const struct xbox_mod_api *api)
{
    say("dorm.init");
    api->on_enable(dorm_enable, NULL);
    return 0;
}
static int quiet_init(const struct xbox_mod_api *api) { (void)api; say("quiet.init"); return 0; }
static int legacy_init(const struct xbox_mod_api *api) { (void)api; say("legacy.init"); return 0; }
static int bad_init(const struct xbox_mod_api *api) { (void)api; say("bad.init"); return 0; }

static const struct xbox_mod m_fly = { "fly", fly_init, NULL, &k_fly_desc };
static const struct xbox_mod m_probe = { "probe", probe_init, NULL, &k_probe_desc };
static const struct xbox_mod m_dorm = { "dorm", dorm_init, NULL, &k_dorm_desc };
/* The same descriptor shape as dorm, with no on_enable. */
static const struct xbox_mod_setting k_quiet_set[] = {
    { "on", NULL, NULL, NULL, NULL, NULL, NULL,
      XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 0, 0, NULL },
};
static const struct xbox_mod_desc k_quiet_desc = {
    XBOX_MOD_DESC_VERSION, sizeof(struct xbox_mod_desc), NULL, NULL, NULL, NULL,
    k_quiet_set, 1, NULL, 0
};
static const struct xbox_mod m_quiet = { "quiet", quiet_init, NULL, &k_quiet_desc };
static const struct xbox_mod m_legacy = { "legacy", legacy_init, NULL, NULL };
static const struct xbox_mod m_bad = { "bad", bad_init, NULL, &k_d_lohi };

/* ---- 1. validation: each refusal, a v2 mod, a short size -------------- */

static void test_refusals(void)
{
    static const struct xbox_mod b1 = { "b1", bad_init, NULL, &k_d_lohi };
    static const struct xbox_mod b2 = { "b2", bad_init, NULL, &k_d_dup };
    static const struct xbox_mod b3 = { "b3", bad_init, NULL, &k_d_enum };
    static const struct xbox_mod b4 = { "b4", bad_init, NULL, &k_d_kind };
    static const struct xbox_mod b5 = { "b5", bad_init, NULL, &k_d_enable };
    static const struct xbox_mod b6 = { "b6", bad_init, NULL, &k_d_env };
    static const struct xbox_mod b7 = { "b7", bad_init, NULL, &k_d_ctl_dup };
    static const struct xbox_mod b8 = { "b8", bad_init, NULL, &k_d_ctl_kind };
    static const struct xbox_mod b9 = { "b9", bad_init, NULL, &k_d_ver0 };
    static const struct xbox_mod b10 = { "b10", bad_init, NULL, &k_d_tiny };
    static const struct xbox_mod b11 = { "b11", bad_init, NULL, &k_d_keylen };
    static const struct xbox_mod b12 = { "b12", bad_init, NULL, &k_d_ctl_keylen };
    static const struct xbox_mod shortd = { "short", quiet_init, NULL, &k_d_short };
    static const struct xbox_mod *const mods[] = {
        &b1, &b2, &b3, &b4, &b5, &b6, &b7, &b8, &b9, &b10, &b11, &b12, &m_legacy, &shortd,
        NULL };
    static const struct { const char *name, *reason; } want[] = {
        { "b1", "setting 'x': lo 9 > hi 1" },
        { "b2", "setting 'x' declared twice" },
        { "b3", "enum setting 'x' has no choices" },
        { "b4", "setting 'x': unknown kind 9" },
        { "b5", "a second enable setting 'y'" },
        { "b6", "setting 'x': no env row RECOMP_NO_SUCH_ROW" },
        { "b7", "control 'x' declared twice" },
        { "b8", "control 'x': unknown kind 7" },
        { "b9", "descriptor version 0" },
        { "b10", "descriptor size 4 is too small" },
        { "b11", "setting 'x': its key is 64 bytes or longer" },
        { "b12", "control 'a_control_id_long_enough_to_pass_sixty_four_bytes': its key is 64 "
                 "bytes or longer" },
    };
    const char *out;
    char line[256];
    int i;

    s_log[0] = 0;
    /* A refused mod's keys: its refusal line names the fault, so they are
     * not reported as unknown on top (F7). */
    out = start("[mods]\n\"b1.x\" = 3\n[input.controls]\n\"b1.go\" = [\"F6\"]\n",
                NULL, mods);
    for (i = 0; i < (int)(sizeof want / sizeof want[0]); i++) {
        int m = index_of(want[i].name);
        CHECK(m >= 0);
        CHECK(xbox_mod_state(m) == XBOX_MOD_FAILED);
        CHECK(!strcmp(xbox_mod_init_outcome(m), "failed"));
        CHECK(!strcmp(xbox_mod_fail_reason(m), want[i].reason));
        CHECK(xbox_mod_desc_of(m) == NULL);
        CHECK(xbox_mod_has_desc(m));
        snprintf(line, sizeof line, "[MOD] %s: descriptor refused: %s; left out\n",
                 want[i].name, want[i].reason);
        HAS(out, line);
    }
    /* None ran; the v2 mod did. */
    CHECK(strstr(s_log, "bad.init") == NULL);
    CHECK(strstr(s_log, "legacy.init") != NULL);
    HAS(out, "[MOD] 1 running: legacy (0 frame, 0 filter)\n");

    i = index_of("legacy");
    CHECK(!xbox_mod_has_desc(i) && xbox_mod_desc_of(i) == NULL);
    CHECK(xbox_mod_enable_setting(i) == -1);
    CHECK(xbox_mod_setting_key(i, 0) == NULL);
    CHECK(!strcmp(xbox_mod_init_outcome(i), "yes"));
    CHECK(!strcmp(xbox_mod_fail_reason(i), ""));

    /* The short descriptor: settings read, controls past its size not. */
    i = index_of("short");
    CHECK(xbox_mod_desc_of(i) != NULL);
    CHECK(xbox_mod_desc_of(i)->n_settings == 1);
    CHECK(xbox_mod_desc_of(i)->n_controls == 0);
    CHECK(xbox_mod_desc_of(i)->controls == NULL);
    CHECK(xbox_mod_desc_of(i)->size == sizeof(struct xbox_mod_desc));
    /* Its switch is off and so is the menu: not inited. */
    CHECK(!strcmp(xbox_mod_init_outcome(i), "no"));

    capture_begin();
    enhance_cfg_report_unused();
    out = capture_end();
    LACKS(out, "mods.b1.x");
    LACKS(out, "input.controls.b1.go");
}

/* ---- 2. reading: precedence, clamp, snap, enum fallback, rewrites ----- */

static void test_precedence(void)
{
    static const struct xbox_mod *const mods[] = { &m_fly, &m_probe, NULL };
    const char *keys[8], *values[8], *olds[8];
    const char *out;
    int fly, probe, n;

    set_env("RECOMP_HEAP_QUARANTINE", "7");
    out = start("[mods]\n"
                "\"probe.a\" = 1\n"
                "\"probe.b\" = 2\n"
                "\"fly.speed\" = 25\n"
                "\"fly.count\" = 2.6\n"
                "\"fly.hud\" = \"bogus\"\n"
                "\"fly.smooth\" = \"yes\"\n"
                "[probe]\n"
                "old_c = 3\n"
                "[fly]\n"
                "enabled = \"on\"\n",
                "[mods]\n"
                "\"probe.b\" = 9\n"
                "\"probe.c\" = 9\n"
                "\"probe.d\" = 4\n"
                "\"fly.smooth\" = false\n"
                "[probe]\n"
                "old_e = 5\n",
                mods);
    set_env("RECOMP_HEAP_QUARANTINE", NULL);
    fly = index_of("fly");
    probe = index_of("probe");

    CHECK(xbox_mod_setting_value(probe, 0) == 7);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 0), "env"));
    CHECK(xbox_mod_setting_forced(probe, 0));
    CHECK(xbox_mod_setting_value(probe, 1) == 2);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 1), "user"));
    CHECK(xbox_mod_setting_value(probe, 2) == 3);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 2), "user_alias"));
    CHECK(xbox_mod_setting_value(probe, 3) == 4);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 3), "defaults"));
    CHECK(xbox_mod_setting_value(probe, 4) == 5);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 4), "defaults_alias"));
    CHECK(xbox_mod_setting_value(probe, 5) == 0);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 5), "default"));
    for (uint32_t k = 1; k < 6; k++)
        CHECK(!xbox_mod_setting_forced(probe, k));
    CHECK(!strcmp(xbox_mod_setting_key(probe, 2), "mods.probe.c"));
    CHECK(!strcmp(xbox_mod_setting_key(fly, 0), "fly.enabled"));

    /* Clamp, snap, rounding and the enum fallback, one line each. */
    HAS(out, "[MOD] fly.speed: 25 -> 20\n");
    HAS(out, "[MOD] fly.count: 2.6 -> 3\n");
    HAS(out, "[MOD] fly.hud: bogus -> keep\n");
    CHECK(xbox_mod_setting_value(fly, 1) == 20);
    CHECK(xbox_mod_setting_value(fly, 2) == 3);
    CHECK(xbox_mod_setting_value(fly, 3) == 0);
    CHECK(!strcmp(xbox_mod_setting_source(fly, 3), "user"));
    /* The wrong type: one line, and the next row (the defaults file). */
    HAS(out, "[MOD] fly.smooth: ./config/config.toml:7: mods.fly.smooth should be true or false;"
             " ignored\n");
    CHECK(xbox_mod_setting_value(fly, 4) == 0);
    CHECK(!strcmp(xbox_mod_setting_source(fly, 4), "defaults"));
    /* The switch on: the enable line, inited, its action live. */
    HAS(out, "[ENHANCE] fly.enabled=on\n");
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "yes"));
    CHECK(s_init_speed == 20);
    CHECK(xbox_mod_enable_setting(fly) == 0);

    /* The user alias waits to be rewritten under its key. */
    n = xbox_mod_pending_rewrites(keys, values, olds, 8);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(!strcmp(keys[0], "mods.probe.c"));
        CHECK(!strcmp(values[0], "3.0"));
        CHECK(!strcmp(olds[0], "probe.old_c"));
    }

    /* Snap on the grid: 1.02 on 0.05 steps is 1, and 1.0 stays quiet. */
    out = start("[mods]\n\"fly.speed\" = 1.02\n", NULL, mods);
    HAS(out, "[MOD] fly.speed: 1.02 -> 1\n");
    out = start("[mods]\n\"fly.speed\" = 1.0\n", NULL, mods);
    LACKS(out, "fly.speed:");
    CHECK(xbox_mod_setting_value(index_of("fly"), 1) == 1);
    /* An alias spelled in the user file over a defaults key; env text of
     * the wrong type is ignored with its line and the files are read. */
    set_env("RECOMP_APU_HRTF", "fast");
    out = start("[fly]\nold_speed = 2.5\n", "[mods]\n\"fly.speed\" = 4\n", mods);
    set_env("RECOMP_APU_HRTF", NULL);
    HAS(out, "[MOD] fly.speed: RECOMP_APU_HRTF='fast' is not a number; ignored\n");
    fly = index_of("fly");
    CHECK(xbox_mod_setting_value(fly, 1) == 2.5);
    CHECK(!strcmp(xbox_mod_setting_source(fly, 1), "user_alias"));
    CHECK(!xbox_mod_setting_forced(fly, 1));
    n = xbox_mod_pending_rewrites(keys, values, olds, 8);
    CHECK(n == 1 && !strcmp(keys[0], "mods.fly.speed") && !strcmp(values[0], "2.5") &&
          !strcmp(olds[0], "fly.old_speed"));
    /* A captured control's alias goes on the same list, as an array. */
    out = start("[fly]\nmove_keys = [\"w\", \"a\"]\n", NULL, mods);
    fly = index_of("fly");
    {
        char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
        const char *src = NULL;
        CHECK(xbox_mod_control_bindings(fly, 1, b, ENHANCE_ACTION_BINDS, &src) == 2);
        CHECK(src && !strcmp(src, "user_alias"));
        CHECK(!strcmp(b[0], "w") && !strcmp(b[1], "a"));
    }
    n = xbox_mod_pending_rewrites(keys, values, olds, 8);
    CHECK(n == 1 && !strcmp(keys[0], "input.controls.fly.move") &&
          !strcmp(values[0], "[\"w\", \"a\"]") && !strcmp(olds[0], "fly.move_keys"));
}

/* ---- 3. the env-forced value, the menu's set and on_setting ----------- */

static void test_set_and_callback(void)
{
    static const struct xbox_mod *const mods[] = { &m_fly, &m_probe, NULL };
    int fly, probe;

    set_env("RECOMP_HEAP_QUARANTINE", "7");
    start("[fly]\nenabled = \"on\"\n", NULL, mods);
    set_env("RECOMP_HEAP_QUARANTINE", NULL);
    fly = index_of("fly");
    probe = index_of("probe");

    /* Forced by env: read-only. */
    CHECK(xbox_mod_setting_set(probe, 0, 50) == 7);
    CHECK(xbox_mod_setting_value(probe, 0) == 7);
    CHECK(!strcmp(xbox_mod_setting_source(probe, 0), "env"));

    /* A live setting: stored now, clamped, the callback at the frame. */
    s_cb_n = 0;
    s_log[0] = 0;
    CHECK(xbox_mod_setting_set(fly, 1, 30) == 20);
    CHECK(s_api->setting(&k_fly_desc, 1) == 20);
    CHECK(s_api->setting(xbox_mod_desc_of(fly), 1) == 20);
    CHECK(!strcmp(xbox_mod_setting_source(fly, 1), "menu"));
    CHECK(s_cb_n == 0);
    xbox_mod_frame();
    CHECK(s_cb_n == 1 && s_cb_idx == 1 && s_cb_val == 20);
    xbox_mod_frame();
    CHECK(s_cb_n == 1);
    /* A RESTART setting records the value only: no callback, and the
     * running value is the start's. */
    CHECK(xbox_mod_setting_set(fly, 2, 7) == 7);
    CHECK(xbox_mod_setting_value(fly, 2) == 7);
    CHECK(s_api->setting(&k_fly_desc, 2) == 3);
    xbox_mod_frame();
    CHECK(s_cb_n == 1);
    /* probe's RESTART int too. */
    CHECK(xbox_mod_setting_set(probe, 6, 4.4) == 4);
    xbox_mod_frame();
    CHECK(s_cb_n == 1);
    /* The switch is RESTART: turning it off is recorded for the next start. */
    xbox_mod_setting_set(fly, 0, 0);
    CHECK(xbox_mod_state(fly) == XBOX_MOD_RESTART);
    xbox_mod_setting_set(fly, 0, 1);
    CHECK(xbox_mod_state(fly) == XBOX_MOD_ON);
    /* Bad indexes and descriptors. */
    CHECK(s_api->setting(&k_fly_desc, 99) == 0);
    CHECK(s_api->setting(&k_dorm_desc, 0) == 0);
    CHECK(s_api->control_action(&k_fly_desc, 99) == -1);
    CHECK(!strcmp(xbox_mod_setting_source(fly, 99), "default"));
    CHECK(xbox_mod_setting_key(fly, 99) == NULL);
}

/* ---- 4. the init rule, the enable line, the ACTION controls ----------- */

static void test_init_rule(void)
{
    static const struct xbox_mod *const mods[] = { &m_fly, &m_dorm, &m_quiet, NULL };
    const char *out;
    int fly, dorm, quiet, id;

    /* Menu off, every switch off: none inited, each enable line printed. */
    s_log[0] = 0;
    out = start(NULL, NULL, mods);
    HAS(out, "[ENHANCE] fly.enabled=off\n");
    HAS(out, "[ENHANCE] mods.dorm.on=off\n");
    HAS(out, "[ENHANCE] mods.quiet.on=off\n");
    HAS(out, "[MOD] 0 running: (0 frame, 0 filter)\n");
    HAS(out, "[ENHANCE] actions hook=off\n");
    CHECK(s_log[0] == 0);
    fly = index_of("fly");
    dorm = index_of("dorm");
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "no"));
    CHECK(!strcmp(xbox_mod_init_outcome(dorm), "no"));
    CHECK(xbox_mod_state(fly) == XBOX_MOD_OFF);
    /* Not inited, still registered, dormant: the id is there and its
     * binding keeps F6 from another action. */
    id = xbox_mod_control_action(fly, 0);
    CHECK(id >= 0 && id == enhance_action_find("fly.toggle"));
    CHECK(enhance_action_dormant(id));
    CHECK(xbox_mod_control_action(fly, 1) == -1);
    CHECK(xbox_mod_control_action(fly, 2) == -1);
    {
        char bound[64] = "";
        CHECK(enhance_action_owner("F6", -1, bound, sizeof bound) == id);
        CHECK(!strcmp(bound, "F6"));
    }

    /* Menu on: the live switch inits dormant; with on_enable it is quiet,
     * without one it says the switch applies at restart. fly's switch is
     * RESTART, so it is still not inited. */
    s_log[0] = 0;
    out = start("[menu]\nenabled = true\n", NULL, mods);
    dorm = index_of("dorm");
    quiet = index_of("quiet");
    fly = index_of("fly");
    CHECK(!strcmp(xbox_mod_init_outcome(dorm), "dormant"));
    CHECK(!strcmp(xbox_mod_init_outcome(quiet), "dormant"));
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "no"));
    CHECK(strstr(s_log, "dorm.init") && strstr(s_log, "quiet.init") && !strstr(s_log, "fly.init"));
    LACKS(out, "[MOD] dorm: inited dormant");
    HAS(out, "[MOD] quiet: inited dormant but gave no on_enable; turning it on applies at"
             " restart\n");
    HAS(out, "[MOD] 2 running: dorm quiet (0 frame, 0 filter)\n");
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_OFF);
    id = xbox_mod_control_action(dorm, 0);
    CHECK(enhance_action_dormant(id));
    /* The menu's switch: live, through on_enable at the next frame, and
     * the action wakes with no binding moved. */
    s_log[0] = 0;
    CHECK(xbox_mod_setting_set(dorm, 0, 1) == 1);
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_ON);
    CHECK(!enhance_action_dormant(id));
    xbox_mod_frame();
    CHECK(strstr(s_log, "dorm.on") != NULL);
    {
        char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
        CHECK(enhance_action_bindings(id, b, ENHANCE_ACTION_BINDS) == 1 && !strcmp(b[0], "F7"));
    }
    s_log[0] = 0;
    xbox_mod_set_enabled(dorm, 0);
    CHECK(enhance_action_dormant(id));
    xbox_mod_frame();
    CHECK(strstr(s_log, "dorm.off") != NULL);

    /* mods."<id>" = false wins over the switch on: the line, not inited. */
    s_log[0] = 0;
    out = start("[mods]\nfly = false\n[fly]\nenabled = \"on\"\n", NULL, mods);
    fly = index_of("fly");
    /* The line says what the run uses, and why (F4). */
    HAS(out, "[ENHANCE] fly.enabled=off (held off by mods.fly)\n");
    LACKS(out, "[ENHANCE] fly.enabled=on");
    HAS(out, "[MOD] fly: off (mods.fly)\n");
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "no"));
    CHECK(strstr(s_log, "fly.init") == NULL);

    /* Switch on: inited; the ACTION's id from init matches, the others -1;
     * the env row replaces its bindings. */
    s_log[0] = 0;
    set_env("RECOMP_EXT_VMA", "F9, guide+y");
    out = start("[fly]\nenabled = \"on\"\n", NULL, mods);
    fly = index_of("fly");
    CHECK(!strcmp(xbox_mod_init_outcome(fly), "yes"));
    CHECK(strstr(s_log, "fly.init") != NULL);
    CHECK(s_toggle_id == enhance_action_find("fly.toggle") && s_toggle_id >= 0);
    CHECK(s_move_id == -1 && s_look_id == -1);
    CHECK(!enhance_action_dormant(s_toggle_id));
    {
        char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
        const char *src = NULL;
        CHECK(enhance_action_bindings(s_toggle_id, b, ENHANCE_ACTION_BINDS) == 2);
        CHECK(!strcmp(b[0], "F9") && !strcmp(b[1], "guide+y"));
        CHECK(xbox_mod_control_bindings(fly, 0, b, ENHANCE_ACTION_BINDS, &src) == 2);
        CHECK(src && !strcmp(src, "env"));
    }
    set_env("RECOMP_EXT_VMA", NULL);
}

/* RECOMP_DEBUG=mod_set_at: the Mods page's switch made from the
 * flip tick once the guest reaches a frame count, each entry once; an
 * entry naming no setting is skipped. */
static void test_set_at(void)
{
    static const struct xbox_mod *const mods[] = { &m_dorm, NULL };
    int dorm;

    set_env("RECOMP_DEBUG", "mod_set_at=2:mods.dorm.on=on,3:mods.dorm.on=off,1:nope=1");
    start("[menu]\nenabled = true\n", NULL, mods);
    dorm = index_of("dorm");
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_OFF);
    xbox_mod_frame();
    xbox_mod_debug_tick();                  /* frame 1: nothing due */
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_OFF);
    xbox_mod_frame();
    xbox_mod_debug_tick();
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_ON);
    s_log[0] = 0;
    xbox_mod_frame();
    CHECK(strstr(s_log, "dorm.on") != NULL);
    xbox_mod_debug_tick();
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_OFF);
    CHECK(xbox_mod_setting_set(dorm, 0, 1) == 1);
    xbox_mod_debug_tick();                  /* each entry once */
    CHECK(xbox_mod_state(dorm) == XBOX_MOD_ON);
    set_env("RECOMP_DEBUG", NULL);
    start(NULL, NULL, mods);
}

/* ---- 5. unknown keys --------------------------------------------------- */

static void test_unknown_keys(void)
{
    static const struct xbox_mod *const mods[] = { &m_fly, &m_legacy, NULL };
    const char *out;

    out = start("[mods]\n"
                "\"fly.sped\" = 2\n"
                "\"fly.speed\" = 2\n"
                "\"legacy.whatever\" = 1\n"
                "\"ghost.x\" = 1\n"
                "[input.controls]\n"
                "\"fly.move\" = [\"w\"]\n"
                "\"fly.nope\" = [\"x\"]\n",
                NULL, mods);
    (void)out;
    capture_begin();
    enhance_cfg_report_unused();
    out = capture_end();
    /* fly is off (its switch): its typo is still reported, its keys not. */
    HAS(out, "mods.fly.sped");
    LACKS(out, "mods.fly.speed");
    HAS(out, "mods.ghost.x");
    HAS(out, "input.controls.fly.nope");
    LACKS(out, "input.controls.fly.move");
    /* A running v2 mod reads its own keys: one it never read is a typo. */
    HAS(out, "mods.legacy.whatever");
}

/* ---- 6. the dump ------------------------------------------------------- */

static void test_dump(void)
{
    static const struct xbox_mod *const mods[] = {
        &m_fly, &m_probe, &m_dorm, &m_bad, &m_legacy, NULL };
    static const char *const user =
        "[menu]\nenabled = true\n"
        "[mods]\n\"probe.b\" = 2\n\"fly.hud\" = \"hide\"\n"
        "[probe]\nold_c = 3\n"
        "[input.controls]\n\"fly.move\" = [\"w\", \"a\", \"s\", \"d\"]\n"
        "[input.actions]\n\"dorm.go\" = [\"F7\", \"NoSuchKey\"]\n";
    static const char *const defaults = "[mods]\n\"probe.d\" = 4\n[probe]\nold_e = 5.25\n";
    char *got, *want;
    const char *out;
    int n;

    /* The config loaded as a start loads it, then the dump's own steps
     * (enhance.c) on this table. */
    set_env("RECOMP_HEAP_QUARANTINE", "7");
    start(user, defaults, NULL);
    s_log[0] = 0;
    xbox_mod_register_table(mods);
    capture_begin();
    n = xbox_mod_init_describe();
    enhance_actions_rebuild();
    CHECK(xbox_mod_describe_write("mods.json") == 0);
    out = capture_end();
    set_env("RECOMP_HEAP_QUARANTINE", NULL);
    CHECK(n == 5);
    /* No mod code ran and nothing runs per frame. */
    CHECK(s_log[0] == 0);
    xbox_mod_frame();
    CHECK(s_log[0] == 0);
    LACKS(out, "running:");
    HAS(out, "[MOD] bad: descriptor refused: setting 'x': lo 9 > hi 1; left out\n");

    got = read_all("mods.json");
    want = read_all(MOD_DESC_EXPECTED);
    CHECK(got != NULL && want != NULL);
    if (got && want) {
        /* expected.json may have been checked out with CRLF. */
        char *r = want, *w = want;
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
        CHECK(!strcmp(got, want));
        if (strcmp(got, want))
            fprintf(stderr, "mods.json differs from %s:\n%s", MOD_DESC_EXPECTED, got);
    }
    free(got);
    free(want);

    /* A path that cannot be written: 1 and its line. */
    capture_begin();
    CHECK(xbox_mod_describe_write("no_such_dir/mods.json") == 1);
    out = capture_end();
    HAS(out, "[MOD] RECOMP_MODS_DESCRIBE_OUT=no_such_dir/mods.json: cannot write it (");

    /* Only json asks for it; another value is logged and ignored. */
    set_env("RECOMP_MODS_DESCRIBE", "yaml");
    capture_begin();
    CHECK(xbox_mod_describe_wanted() == 0);
    out = capture_end();
    HAS(out, "[MOD] RECOMP_MODS_DESCRIBE='yaml' is not json; ignored\n");
    set_env("RECOMP_MODS_DESCRIBE", "json");
    CHECK(xbox_mod_describe_wanted() == 1);
    set_env("RECOMP_MODS_DESCRIBE", NULL);
    CHECK(xbox_mod_describe_wanted() == 0);
    /* An empty table: an empty list. */
    xbox_mod_register_table(NULL);
    xbox_mod_init_describe();
    CHECK(xbox_mod_describe_write("empty.json") == 0);
    got = read_all("empty.json");
    CHECK(got && !strcmp(got, "{\"schema\": 1, \"api\": 7, \"mods\": [], \"shaders\": "
                             "{\"active\": \"none\", \"presets\": [], \"active_profile\": "
                             "\"none\", \"profiles\": []}}\n"));
    free(got);

#ifndef _WIN32
    /* The real path: xbox_enhance_init exits with the write's result and
     * no init ran (an init would leave its marker). */
    {
        static const struct xbox_mod *const one[] = { &m_fly, NULL };
        int st;
        pid_t pid;

        remove("child.json");
        remove("child_init.txt");
        for (int bad = 0; bad < 2; bad++) {
            fflush(NULL);
            pid = fork();
            if (pid == 0) {
                int fd = OPEN_TRUNC("child_stderr.txt");
                if (fd >= 0) {
                    DUP2(fd, 2);
                    CLOSE(fd);
                }
                set_env("RECOMP_MODS_DESCRIBE", "json");
                set_env("RECOMP_MODS_DESCRIBE_OUT", bad ? "no_such_dir/x.json" : "child.json");
                write_file("config/config.toml", "[fly]\nenabled = \"on\"\n");
                xbox_mod_register_table(one);
                xbox_enhance_init(".", NULL);
                write_file("child_init.txt", "returned");
                _exit(9);
            }
            CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
            CHECK(WIFEXITED(st) && WEXITSTATUS(st) == bad);
        }
        got = read_all("child.json");
        CHECK(got && strstr(got, "\"init\": \"yes\"") != NULL);
        free(got);
        CHECK(read_all("child_init.txt") == NULL);
        got = read_all("child_stderr.txt");
        CHECK(got && strstr(got, "[MOD] RECOMP_MODS_DESCRIBE_OUT=no_such_dir/x.json: cannot"
                                 " write it (") != NULL);
        CHECK(got && strstr(got, "actions hook=") == NULL);
        free(got);
    }

    /* The shaders field: the requested preset (a path) parsed by a
     * librashader stand-in, with the config's override as its value. A
     * child, since the loader keeps its first answer for the process. */
    {
        int st;
        pid_t pid;

        remove("shaders.json");
        write_file("dump_test.slangp", "shaders = 0\n");
        MKDIR("config/shader-profiles");
        write_file("config/shader-profiles/duo.toml",
                   "version = 1\nname = \"Duo\"\nstages = [\"fsr\", \"user:mine\"]\n");
        write_file("config/shader-profiles/bad.toml", "version = 1\nstages = 3\n");
        write_file("config/shader-profiles/later.toml",
                   "version = 9\nname = \"Later\"\nstages = [\"fsr\"]\n");
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            int fd = OPEN_TRUNC("child_stderr.txt");
            if (fd >= 0) {
                DUP2(fd, 2);
                CLOSE(fd);
            }
            set_env("RECOMP_MODS_DESCRIBE", "json");
            set_env("RECOMP_MODS_DESCRIBE_OUT", "shaders.json");
            write_file("config/config.toml",
                       "[present]\nshader = \"dump_test.slangp\"\nshader_lib = \"" FAKE_LRS_PARAMS
                       "\"\n[present.shader_params.dump_test]\nmaskDark = 0.8\n");
            xbox_mod_register_table(NULL);
            xbox_enhance_init(".", NULL);
            _exit(9);
        }
        CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        got = read_all("shaders.json");
        CHECK(got && strstr(got, "\"shaders\": {\"active\": \"dump_test.slangp\", \"presets\": [\n"
                                 "    {\"name\": \"dump_test\", \"params\": [\n"
                                 "      {\"name\": \"warpX\", \"label\": \"warpX\", \"min\": 0, ")
                  != NULL);
        CHECK(got && strstr(got, "{\"name\": \"maskDark\", \"label\": \"Lottes maskDark\", "
                                 "\"min\": 0, \"max\": 2, \"step\": 0.05, \"default\": 0.5, "
                                 "\"value\": 0.8, \"key\": "
                                 "\"present.shader_params.dump_test.maskDark\"}]}\n  ], ")
                  != NULL);
        /* The profiles: the folder's files, the broken and the newer one
         * listed with their state (the bundled presets have no files here,
         * so the library reads none of them). */
        CHECK(got && strstr(got, "\"active_profile\": \"none\", \"profiles\": [\n"
                                 "    {\"name\": \"bad\", \"slug\": \"bad\", \"file\": "
                                 "\"bad.toml\", \"stages\": [], \"builtin\": false, \"state\": "
                                 "\"broken\"},\n"
                                 "    {\"name\": \"Duo\", \"slug\": \"duo\", \"file\": \"duo.toml\", "
                                 "\"stages\": [\"fsr\", \"user:mine\"], \"builtin\": false, "
                                 "\"state\": \"ok\"},\n"
                                 "    {\"name\": \"Later\", \"slug\": \"later\", \"file\": "
                                 "\"later.toml\", \"stages\": [\"fsr\"], \"builtin\": false, "
                                 "\"state\": \"newer\"}\n  ]}}\n")
                  != NULL);
        if (got && s_fail)
            fprintf(stderr, "shaders.json:\n%s", got);
        free(got);
        remove("shaders.json");

        /* active_profile: the start's choice. */
        fflush(NULL);
        pid = fork();
        if (pid == 0) {
            int fd = OPEN_TRUNC("child_stderr.txt");
            if (fd >= 0) {
                DUP2(fd, 2);
                CLOSE(fd);
            }
            set_env("RECOMP_MODS_DESCRIBE", "json");
            set_env("RECOMP_MODS_DESCRIBE_OUT", "shaders.json");
            write_file("config/config.toml", "[present]\nprofile = \"duo\"\nshader_lib = \""
                       FAKE_LRS_PARAMS "\"\n");
            xbox_mod_register_table(NULL);
            xbox_enhance_init(".", NULL);
            _exit(9);
        }
        CHECK(pid > 0 && waitpid(pid, &st, 0) == pid);
        CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0);
        got = read_all("shaders.json");
        CHECK(got && strstr(got, "\"active_profile\": \"duo\", \"profiles\": [") != NULL);
        if (got && s_fail)
            fprintf(stderr, "shaders.json:\n%s", got);
        free(got);
        remove("shaders.json");
        remove("config/shader-profiles/duo.toml");
        remove("config/shader-profiles/bad.toml");
        remove("config/shader-profiles/later.toml");
        rmdir("config/shader-profiles");
        remove("dump_test.slangp");
        remove("config/config.toml");
    }
#endif
}

int main(void)
{
    MKDIR("config");
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_MODS_DESCRIBE", NULL);
    set_env("RECOMP_MODS_DESCRIBE_OUT", NULL);
    test_refusals();
    test_precedence();
    test_set_and_callback();
    test_init_rule();
    test_set_at();
    test_unknown_keys();
    test_dump();
    xbox_mod_shutdown();
    remove(defaults_file());
    printf("mod_desc: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
