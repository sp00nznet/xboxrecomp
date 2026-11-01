/*
 * present_chain - see CMakeLists.txt. One case per process: the loader
 * remembers its first answer for the session, as the runtime wants.
 *
 *   present_chain_test CASE MODULE_DIR
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <sys/stat.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "librashader.h"
#include "nv2a_present_chain.h"
#include "platform/recomp_exe_dir.h"

static int s_fail;

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            s_fail = 1;                                                              \
        }                                                                            \
    } while (0)

static void requested_is(const char *want)
{
    char b[1024];
    nv2a_present_chain_requested(b, sizeof b);
    if (strcmp(b, want)) {
        fprintf(stderr, "FAIL requested \"%s\", want \"%s\"\n", b, want);
        s_fail = 1;
    }
}

static void case_request(void)
{
    unsigned g;

    CHECK(nv2a_present_chain_gen() == 0);
    CHECK(!nv2a_present_chain_wanted());
    requested_is("none");

    nv2a_present_chain_request("crt-lottes");
    g = nv2a_present_chain_gen();
    CHECK(g == 1);
    CHECK(nv2a_present_chain_wanted());
    requested_is("crt-lottes");

    /* A failure for an older generation leaves a newer request alone. */
    nv2a_present_chain_request("fsr");
    nv2a_present_chain_failed(g);
    CHECK(nv2a_present_chain_wanted());
    requested_is("fsr");

    /* The current one turns the chain off until the next request. */
    nv2a_present_chain_failed(nv2a_present_chain_gen());
    CHECK(!nv2a_present_chain_wanted());
    requested_is("fsr");
    nv2a_present_chain_request("crt-hyllian");
    CHECK(nv2a_present_chain_wanted());

    nv2a_present_chain_request("none");
    CHECK(!nv2a_present_chain_wanted());
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_request(NULL);
    CHECK(!nv2a_present_chain_wanted());
    requested_is("none");
    nv2a_present_chain_request("");
    requested_is("none");
    CHECK(nv2a_present_chain_gen() == 7);

    /* The menu's choices are the bundled names. */
    CHECK(!strcmp(nv2a_present_chain_names[0], "crt-lottes"));
    CHECK(nv2a_present_chain_names[4] == NULL);
}

static void write_file(const char *p)
{
    FILE *f = fopen(p, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs("shaders = 0\n", f);
        fclose(f);
    }
}

static void case_preset(void)
{
    const char *dir = recomp_exe_dir();
    char out[1024], want[1024], sub[1024];
    unsigned g = 99;

    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 0);
    CHECK(g == 0);

    /* A bundled name with no shaders/ beside the executable: not found. */
    snprintf(want, sizeof want, "%s/shaders/crt-lottes.slangp", dir);
    remove(want);
    nv2a_present_chain_request("crt-lottes");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);

    snprintf(sub, sizeof sub, "%s/shaders", dir);
    make_dir(sub);
    write_file(want);
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));
    CHECK(g == nv2a_present_chain_gen());

    /* A path relative to the executable, then an absolute one. */
    nv2a_present_chain_request("shaders/crt-lottes.slangp");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));
    nv2a_present_chain_request(want);
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == 1);
    CHECK(!strcmp(out, want));

    /* A .slangp name without a directory is a path, not a bundled name. */
    nv2a_present_chain_request("no-such.slangp");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);
    nv2a_present_chain_request("no-such-preset");
    CHECK(nv2a_present_chain_preset(out, sizeof out, &g) == -1);
    remove(want);
}

/* The loader refuses the library: said once, and it stays refused. */
static void refused(const char *path)
{
    char e[64];
    nv2a_present_chain_set_lib(path);
    CHECK(nv2a_present_chain_lib() == NULL);
    CHECK(nv2a_present_chain_lib() == NULL);
    nv2a_present_chain_error(NULL, e, sizeof e);
    CHECK(!strcmp(e, "unknown error"));
}

static void module_path(char *out, size_t cap, const char *dir, const char *name)
{
    snprintf(out, cap, "%s/fake_lrs_%s%s", dir, name, MOD_SUFFIX);
}

static int near(float got, float want)
{
    return got > want - 0.05f && got < want + 0.05f;
}

/* The frame-rate uniform follows the presents, not the display. */
static void case_rate(void)
{
    const uint64_t ms = 1000000ull;
    uint64_t t = 5000 * ms;
    float r = 0;
    int i;

    CHECK(near(nv2a_present_chain_rate(t), 60.0f));    /* first present */
    for (i = 0; i < 64; i++)
        r = nv2a_present_chain_rate(t += 33333333ull);  /* lock30 */
    CHECK(near(r, 30.0f));
    r = nv2a_present_chain_rate(t += 50 * ms);          /* one late present */
    CHECK(r > 25.0f && r < 30.0f);
    CHECK(near(nv2a_present_chain_rate(t += 2000 * ms), 60.0f));  /* a pause */
    for (i = 0; i < 64; i++)
        r = nv2a_present_chain_rate(t += 16666667ull);
    CHECK(near(r, 60.0f));
    CHECK(near(nv2a_present_chain_rate(t), 60.0f));     /* a clock that did not move */
}

static void load_params_lib(const char *dir)
{
    char p[1024];
    module_path(p, sizeof p, dir, "params");
    nv2a_present_chain_set_lib(p);
    CHECK(nv2a_present_chain_lib() != NULL);
}

/* What the backend's set_param saw, for params_apply. */
static struct { char name[64]; float v; } s_set[8];
static int s_nset;

static void record_set(void *ctx, const char *name, float v)
{
    CHECK(ctx == &s_nset);
    if (s_nset < 8) {
        snprintf(s_set[s_nset].name, sizeof s_set[0].name, "%s", name);
        s_set[s_nset].v = v;
    }
    s_nset++;
}

static float set_value(const char *name)
{
    for (int i = 0; i < s_nset && i < 8; i++)
        if (!strcmp(s_set[i].name, name))
            return s_set[i].v;
    return -99.0f;
}

static int override_row(const char *key, const char *name, float *v, int *has, int *dirty)
{
    char k[64], n[64];
    for (int i = 0; nv2a_present_chain_override_at(i, k, sizeof k, n, sizeof n, v, has, dirty); i++)
        if (!strcmp(k, key) && !strcmp(n, name))
            return 1;
    return 0;
}

/* The store: the table a backend publishes, the overrides the config and
 * the menu set, what Save writes, and what the backend applies. */
static void case_params(const char *dir)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char key[64];
    unsigned applied = 0, g, pg;
    void *preset = NULL;    /* the fake's fixed table */
    int has = 0, dirty = 0;
    float v = 0;

    load_params_lib(dir);
    /* Config at start: one for the preset that will run, one for another. */
    nv2a_present_chain_override_load("crt-lottes", "maskDark", 0.8f);
    nv2a_present_chain_override_load("fsr", "FSR_SHARPENING", 0.1f);
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    CHECK(nv2a_present_chain_override_get("crt-lottes", "maskDark", &v) && near(v, 0.8f));

    /* No chain yet: nothing is active. */
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    CHECK(key[0] == 0);
    CHECK(!nv2a_present_chain_param_set("warpX", 0.0f));
    CHECK(!nv2a_present_chain_param_get("warpX", &v));

    /* A publish for an older request is dropped. */
    nv2a_present_chain_request("crt-lottes");
    g = nv2a_present_chain_gen();
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_params_publish(&preset, g);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);

    pg = nv2a_present_chain_params_gen();
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
    CHECK(nv2a_present_chain_params_gen() != pg);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 2);
    CHECK(!strcmp(key, "crt-lottes"));
    CHECK(!strcmp(t[0].name, "warpX") && near(t[0].initial, 0.01f) && near(t[0].max, 0.125f));
    CHECK(!strcmp(t[1].desc, "Lottes maskDark"));

    /* The backend applies every row: the config's override, the preset's
     * value for the rest. Nothing again until the generation moves. */
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 2);
    CHECK(near(set_value("maskDark"), 0.8f) && near(set_value("warpX"), 0.01f));
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 0);

    /* The menu sets a value: clamped to the range, live, unsaved. */
    CHECK(nv2a_present_chain_param_set("warpX", 9.0f));
    CHECK(nv2a_present_chain_param_get("warpX", &v) && near(v, 0.125f));
    CHECK(!nv2a_present_chain_param_set("noSuch", 1.0f));
    CHECK(nv2a_present_chain_overrides_dirty() == 1);
    CHECK(override_row("crt-lottes", "warpX", &v, &has, &dirty) && has && dirty);
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 2 && near(set_value("warpX"), 0.125f));

    /* Back to the preset's value (float steps add up): no override, so
     * nothing to save for warpX. */
    nv2a_present_chain_param_set("warpX", 0.0100001f);
    CHECK(override_row("crt-lottes", "warpX", &v, &has, &dirty) && !has && !dirty);
    CHECK(nv2a_present_chain_overrides_dirty() == 0);

    /* Removing a loaded override is a change Save writes (as a delete). */
    nv2a_present_chain_param_set("maskDark", 0.5f);
    CHECK(override_row("crt-lottes", "maskDark", &v, &has, &dirty) && !has && dirty);
    CHECK(nv2a_present_chain_overrides_dirty() == 1);
    nv2a_present_chain_overrides_saved();
    CHECK(nv2a_present_chain_overrides_dirty() == 0);

    /* A hand-edited value outside the range runs clamped. */
    nv2a_present_chain_override_load("crt-lottes", "maskDark", -3.0f);
    CHECK(nv2a_present_chain_param_get("maskDark", &v) && near(v, 0.0f));

    /* Reset to preset drops this preset's overrides, not another's. */
    nv2a_present_chain_param_set("warpX", 0.05f);
    nv2a_present_chain_params_reset("crt-lottes");
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "maskDark", NULL));
    CHECK(nv2a_present_chain_override_get("fsr", "FSR_SHARPENING", &v) && near(v, 0.1f));
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(near(set_value("maskDark"), 0.5f) && near(set_value("warpX"), 0.01f));
    /* Reset to defaults drops every preset's. */
    nv2a_present_chain_params_reset(NULL);
    CHECK(!nv2a_present_chain_override_get("fsr", "FSR_SHARPENING", NULL));

    /* The chain goes away: no table, and the generation moves. */
    pg = nv2a_present_chain_params_gen();
    nv2a_present_chain_params_clear();
    CHECK(nv2a_present_chain_params_gen() != pg);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    CHECK(!nv2a_present_chain_param_get("warpX", &v));

    /* A full table keeps what it holds and refuses the new row; once Save
     * has written the removals, the empty rows are reused. */
    nv2a_present_chain_params_reset_for_test();
    for (int i = 0; i < NV2A_PRESENT_OVERRIDES_MAX; i++) {
        char n[16];
        snprintf(n, sizeof n, "p%d", i);
        nv2a_present_chain_override_load("other", n, 1.0f);
    }
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    CHECK(nv2a_present_chain_override_get("other", "p0", &v) && near(v, 1.0f));
    nv2a_present_chain_params_reset("other");
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    nv2a_present_chain_overrides_saved();
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(nv2a_present_chain_override_get("crt-lottes", "warpX", &v) && near(v, 0.05f));
    nv2a_present_chain_params_reset_for_test();
}

static void key_is(const char *req, const char *want)
{
    char k[64];
    nv2a_present_chain_preset_key(req, k, sizeof k);
    if (strcmp(k, want)) {
        fprintf(stderr, "FAIL key of \"%s\" is \"%s\", want \"%s\"\n", req ? req : "(null)", k,
                want);
        s_fail = 1;
    }
}

/* The config section a preset's overrides live under. */
static void case_params_key(void)
{
    key_is("crt-lottes", "crt-lottes");
    key_is("none", "");
    key_is(NULL, "");
    key_is("", "");
    key_is("C:\\mods\\My Shader.slangp", "My_Shader");
    key_is("/home/deck/shaders/crt-royale.v2.slangp", "crt-royale_v2");
    key_is("shaders/x.slangp", "x");

    /* A CRT preset draws one scanline per source row, so it is fed the
     * guest's lines; anything else keeps the scaled frame. */
    CHECK(nv2a_present_chain_key_lines("crt-lottes"));
    CHECK(nv2a_present_chain_key_lines("CRT-Royale_v2"));
    CHECK(nv2a_present_chain_key_lines("crtglow_gauss"));
    CHECK(nv2a_present_chain_key_lines("zfast-crt"));
    CHECK(nv2a_present_chain_key_lines("newpixie-crt"));
    CHECK(nv2a_present_chain_key_lines("my_CRT_v2"));
    CHECK(nv2a_present_chain_key_lines("scanlines-sine-abs"));
    CHECK(nv2a_present_chain_key_lines("fake_scanline"));
    CHECK(!nv2a_present_chain_key_lines("fsr"));
    CHECK(!nv2a_present_chain_key_lines("cr"));
    CHECK(!nv2a_present_chain_key_lines("acrtx"));
    CHECK(!nv2a_present_chain_key_lines("megatron"));
    CHECK(!nv2a_present_chain_key_lines("scan"));
    CHECK(!nv2a_present_chain_key_lines(""));
    CHECK(!nv2a_present_chain_key_lines(NULL));
    /* RetroArch's layout: the folder says it */
    CHECK(nv2a_present_chain_ref_lines("/rs/shaders_slang/crt/gtu-v050.slangp"));
    CHECK(nv2a_present_chain_ref_lines("C:\\rs\\CRT\\megatron.slangp"));
    CHECK(!nv2a_present_chain_ref_lines("/rs/shaders_slang/crtx/gtu-v050.slangp"));
    CHECK(!nv2a_present_chain_ref_lines("/rs/crt-stuff/edge/fsr.slangp"));
    CHECK(!nv2a_present_chain_ref_lines("fsr"));
    CHECK(!nv2a_present_chain_ref_lines("none"));
    CHECK(nv2a_present_chain_ref_lines("crt-lottes"));
    nv2a_present_chain_request("crt-hyllian");
    CHECK(nv2a_present_chain_lines());
    nv2a_present_chain_request("/home/deck/shaders/crt-royale.v2.slangp");
    CHECK(nv2a_present_chain_lines());
    nv2a_present_chain_request("/rs/shaders_slang/crt/mame_hlsl.slangp");
    CHECK(nv2a_present_chain_lines());
    nv2a_present_chain_request("/rs/shaders_slang/presets/mame_hlsl.slangp");
    CHECK(!nv2a_present_chain_lines());
    nv2a_present_chain_request("fsr");
    CHECK(!nv2a_present_chain_lines());
    nv2a_present_chain_request("none");
    CHECK(!nv2a_present_chain_lines());

    /* A profile: its first stage decides, not the composed file's name. */
    {
        static struct nv2a_present_stage st[2];
        char out[1024];
        int on = -1;

        memset(st, 0, sizeof st);
        snprintf(st[0].key, sizeof st[0].key, "crt-lottes");
        snprintf(st[1].key, sizeof st[1].key, "fsr");
        nv2a_present_chain_request_profile("soft", "/cache/soft.slangp", st, 2);
        CHECK(nv2a_present_chain_lines());
        snprintf(st[0].key, sizeof st[0].key, "fsr");
        snprintf(st[1].key, sizeof st[1].key, "crt-lottes");
        nv2a_present_chain_request_profile("up", "/cache/crt/up.slangp", st, 2);
        CHECK(!nv2a_present_chain_lines());
        /* the profile's own lines over the key */
        st[0].lines = NV2A_PRESENT_LINES_GUEST;
        nv2a_present_chain_request_profile("up", "/cache/up.slangp", st, 2);
        CHECK(nv2a_present_chain_lines());
        snprintf(st[0].key, sizeof st[0].key, "crt-lottes");
        st[0].lines = NV2A_PRESENT_LINES_HOST;
        nv2a_present_chain_request_profile("soft", "/cache/soft.slangp", st, 2);
        CHECK(!nv2a_present_chain_lines());
        /* present.shader_lines over both */
        nv2a_present_chain_set_lines(NV2A_PRESENT_LINES_GUEST);
        CHECK(nv2a_present_chain_lines());
        nv2a_present_chain_request("fsr");
        CHECK(nv2a_present_chain_lines());
        /* the path and the choice come from one snapshot */
        nv2a_present_chain_preset_lines(out, sizeof out, NULL, &on);
        CHECK(on == 1);
        nv2a_present_chain_set_lines(NV2A_PRESENT_LINES_HOST);
        nv2a_present_chain_request("crt-lottes");
        CHECK(!nv2a_present_chain_lines());
        nv2a_present_chain_request("none");
        on = -1;
        CHECK(nv2a_present_chain_preset_lines(out, sizeof out, NULL, &on) == 0 && on == 0);
        nv2a_present_chain_set_lines(99);
        nv2a_present_chain_request("crt-lottes");
        CHECK(nv2a_present_chain_lines());
        nv2a_present_chain_set_lines(NV2A_PRESENT_LINES_AUTO);
        nv2a_present_chain_request("none");
    }
}

/* The dump's parse: no GPU and no chain, and the active table untouched.
 * nolib: the default library path, which is not there. */
static void case_describe(const char *dir, int nolib)
{
    struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    char ok[1024], broken[1024], key[64];

    if (nolib) {
        CHECK(nv2a_present_chain_describe("crt-lottes", t, NV2A_PRESENT_PARAMS_MAX) == -1);
        return;
    }
    load_params_lib(dir);
    snprintf(ok, sizeof ok, "%s/pc_describe_ok.slangp", recomp_exe_dir());
    snprintf(broken, sizeof broken, "%s/pc_describe_broken.slangp", recomp_exe_dir());
    write_file(ok);
    write_file(broken);
    CHECK(nv2a_present_chain_describe(ok, t, NV2A_PRESENT_PARAMS_MAX) == 2);
    CHECK(!strcmp(t[1].name, "maskDark") && near(t[1].step, 0.05f));
    CHECK(nv2a_present_chain_describe(ok, t, 1) == 1);
    CHECK(nv2a_present_chain_describe(broken, t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_describe("no-such-preset", t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_describe("none", t, NV2A_PRESENT_PARAMS_MAX) == -1);
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key) == 0);
    remove(ok);
    remove(broken);
}

/* A preset handle from the stand-in, as a backend's libra_preset_create
 * gives it; the fake picks its table by the file's name. */
static void *fake_preset(const char *path)
{
    void *(*create)(const char *, void **) =
        (void *(*)(const char *, void **))nv2a_present_chain_lib()->sym("libra_preset_create");
    void *h = NULL;
    CHECK(create && !create(path, &h));
    return h;
}

/* A stage of a profile request: its preset key and what it declares, from
 * the GPU-free parse, as the profile module fills it at compose time. */
static void stage_of(struct nv2a_present_stage *st, const char *path)
{
    static struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    int n = nv2a_present_chain_describe(path, t, NV2A_PRESENT_PARAMS_MAX);
    memset(st, 0, sizeof *st);
    nv2a_present_chain_preset_key(path, st->key, sizeof st->key);
    CHECK(n > 0);
    for (int i = 0; i < n; i++)
        snprintf(st->name[st->nnames++], sizeof st->name[0], "%s", t[i].name);
}

static const struct nv2a_present_param *row(const struct nv2a_present_param *t, int n,
                                            const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, name))
            return &t[i];
    return NULL;
}

/* A profile request: its slug and stage lists, the rows tagged by stage and
 * shared flag, its values in the profile scope apart from the per-preset
 * overrides, the profile error, and a rename and a delete. */
static void case_profile(const char *dir)
{
    static struct nv2a_present_stage st[NV2A_PRESENT_STAGES_MAX + 1];
    static struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    const struct nv2a_present_param *r;
    char a[1024], b[1024], comp[1024], key[64], err[NV2A_PRESENT_PROFILE_ERR_MAX];
    char names[8][64];
    float vals[8], v = 0;
    unsigned applied = 0, g;
    void *h;
    int n, has, dirty;

    load_params_lib(dir);
    snprintf(a, sizeof a, "%s/pc_stage_a.slangp", recomp_exe_dir());
    snprintf(b, sizeof b, "%s/pc_stage_b.slangp", recomp_exe_dir());
    snprintf(comp, sizeof comp, "%s/pc_composed.slangp", recomp_exe_dir());
    write_file(a);
    write_file(b);
    write_file(comp);
    stage_of(&st[0], a);
    stage_of(&st[1], b);
    CHECK(!strcmp(st[0].key, "pc_stage_a") && st[0].nnames == 2 && st[1].nnames == 2);

    /* The profile's file says SHARED 0.8; a per-preset override under the
     * composed file's own key must not leak into the profile. */
    nv2a_present_chain_profile_value_load("soft", "SHARED", 0.8f);
    nv2a_present_chain_override_load("pc_composed", "SHARED", 0.1f);
    CHECK(nv2a_present_chain_profile_dirty(NULL) == 0);
    CHECK(!nv2a_present_chain_override_get("soft", "SHARED", NULL));

    nv2a_present_chain_profile_error_set("stage 2 (x) is missing");
    CHECK(nv2a_present_chain_profile_error_get(err, sizeof err));
    CHECK(!nv2a_present_chain_requested_profile(key, sizeof key) && key[0] == 0);
    nv2a_present_chain_request_profile("soft", comp, st, 2);
    CHECK(!nv2a_present_chain_profile_error_get(err, sizeof err) && err[0] == 0);
    CHECK(nv2a_present_chain_requested_profile(key, sizeof key) && !strcmp(key, "soft"));
    CHECK(nv2a_present_chain_wanted());
    requested_is(comp);
    {
        unsigned pg;
        char out[1024];
        CHECK(nv2a_present_chain_preset(out, sizeof out, &pg) == 1 && !strcmp(out, comp));
    }

    h = fake_preset(comp);
    nv2a_present_chain_params_publish(&h, nv2a_present_chain_gen());
    n = nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key);
    CHECK(n == 4 && !strcmp(key, "soft") && nv2a_present_chain_params_profile());
    CHECK((r = row(t, n, "A1")) && r->stage == 0 && !r->shared);
    CHECK((r = row(t, n, "SHARED")) && r->stage == 0 && r->shared);
    CHECK((r = row(t, n, "B1")) && r->stage == 1 && !r->shared);
    CHECK((r = row(t, n, "ORPHAN")) && r->stage == -1 && !r->shared);

    /* The backend runs the profile's value, not the preset override. */
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 4 && near(set_value("SHARED"), 0.8f) && near(set_value("B1"), 3.0f));

    /* The menu's change goes to the profile scope; the config's overrides
     * and their dirty count do not see it. */
    CHECK(nv2a_present_chain_param_set("B1", 2.0f));
    CHECK(nv2a_present_chain_profile_value_get("soft", "B1", &v) && near(v, 2.0f));
    CHECK(!nv2a_present_chain_override_get("soft", "B1", NULL));
    CHECK(!override_row("soft", "B1", &v, &has, &dirty));
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    CHECK(nv2a_present_chain_profile_dirty("soft") == 1);
    CHECK(nv2a_present_chain_profile_dirty("other") == 0);
    CHECK(nv2a_present_chain_profile_dirty(NULL) == 1);
    n = nv2a_present_chain_profile_values("soft", names, vals, 8);
    CHECK(n == 2 && row(t, 4, names[0]) && row(t, 4, names[1]));
    nv2a_present_chain_profile_saved("soft");
    CHECK(nv2a_present_chain_profile_dirty(NULL) == 0);

    /* Reset to preset of the built-ins leaves profiles alone; a profile's
     * stage reset removes its names only. */
    nv2a_present_chain_params_reset(NULL);
    CHECK(nv2a_present_chain_profile_value_get("soft", "SHARED", &v) && near(v, 0.8f));
    nv2a_present_chain_profile_reset("soft", "B1");
    CHECK(!nv2a_present_chain_profile_value_get("soft", "B1", NULL));
    CHECK(nv2a_present_chain_profile_dirty("soft") == 1);
    CHECK(nv2a_present_chain_param_get("B1", &v) && near(v, 3.0f));

    /* A rename carries the values, the table's key and the request. */
    nv2a_present_chain_profile_rekey("soft", "soft-2");
    CHECK(nv2a_present_chain_profile_value_get("soft-2", "SHARED", &v) && near(v, 0.8f));
    CHECK(!nv2a_present_chain_profile_value_get("soft", "SHARED", NULL));
    CHECK(nv2a_present_chain_requested_profile(key, sizeof key) && !strcmp(key, "soft-2"));
    nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key);
    CHECK(!strcmp(key, "soft-2"));
    CHECK(nv2a_present_chain_param_get("SHARED", &v) && near(v, 0.8f));
    nv2a_present_chain_profile_forget("soft-2");
    CHECK(!nv2a_present_chain_profile_value_get("soft-2", "SHARED", NULL));
    CHECK(nv2a_present_chain_profile_dirty(NULL) == 0);

    /* A build failure of an older generation says nothing; of this one, the
     * profile error names the composed file. The next request clears it. */
    g = nv2a_present_chain_gen();
    nv2a_present_chain_failed(g - 1);
    CHECK(!nv2a_present_chain_profile_error_get(err, sizeof err));
    nv2a_present_chain_failed(g);
    CHECK(!nv2a_present_chain_wanted());
    CHECK(nv2a_present_chain_profile_error_get(err, sizeof err) && strstr(err, "pc_composed"));
    nv2a_present_chain_request("crt-lottes");
    CHECK(!nv2a_present_chain_profile_error_get(err, sizeof err));
    CHECK(!nv2a_present_chain_requested_profile(key, sizeof key));
    /* A present.shader failure sets no profile error. */
    nv2a_present_chain_failed(nv2a_present_chain_gen());
    CHECK(!nv2a_present_chain_profile_error_get(err, sizeof err));

    /* A plain request's table is the preset scope, untagged (the reset
     * above dropped the override). */
    nv2a_present_chain_override_load("pc_composed", "SHARED", 0.1f);
    nv2a_present_chain_request(comp);
    nv2a_present_chain_params_publish(&h, nv2a_present_chain_gen());
    n = nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key);
    CHECK(n == 4 && !strcmp(key, "pc_composed") && !nv2a_present_chain_params_profile());
    CHECK((r = row(t, n, "SHARED")) && !r->shared && r->stage == 0);
    CHECK(nv2a_present_chain_param_get("SHARED", &v) && near(v, 0.1f));

    /* Nine stages: the first eight kept. B1 is in the ninth only, so no
     * kept stage lists it. */
    for (int i = 2; i < NV2A_PRESENT_STAGES_MAX; i++)
        st[i] = st[0];
    st[NV2A_PRESENT_STAGES_MAX] = st[1];
    memcpy(&st[1], &st[0], sizeof st[0]);
    nv2a_present_chain_request_profile("nine", comp, st, NV2A_PRESENT_STAGES_MAX + 1);
    nv2a_present_chain_params_publish(&h, nv2a_present_chain_gen());
    n = nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX, key, sizeof key);
    CHECK((r = row(t, n, "B1")) && r->stage == -1 && !r->shared);
    CHECK((r = row(t, n, "A1")) && r->stage == 0 && r->shared);

    /* No path or no slug: stock. */
    nv2a_present_chain_request_profile("soft", NULL, st, 1);
    CHECK(!nv2a_present_chain_wanted() && !nv2a_present_chain_requested_profile(key, sizeof key));
    nv2a_present_chain_request_profile("", comp, st, 1);
    CHECK(!nv2a_present_chain_wanted());

    remove(a);
    remove(b);
    remove(comp);
    nv2a_present_chain_params_reset_for_test();
}

/* A preset past the table: its first 128 rows. */
static void case_params_cap(const char *dir)
{
    static struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX + 8];
    char key[64];
    void *h;

    load_params_lib(dir);
    h = fake_preset("pc_many.slangp");
    nv2a_present_chain_request("pc_many.slangp");
    nv2a_present_chain_params_publish(&h, nv2a_present_chain_gen());
    CHECK(nv2a_present_chain_params(t, NV2A_PRESENT_PARAMS_MAX + 8, key, sizeof key)
          == NV2A_PRESENT_PARAMS_MAX);
    CHECK(!strcmp(t[0].name, "m0") && !strcmp(t[NV2A_PRESENT_PARAMS_MAX - 1].name, "m127"));
    CHECK(nv2a_present_chain_param_set("m127", 0.5f));
    CHECK(!nv2a_present_chain_param_set("m128", 0.5f));
}

/* Exit without saving: the overrides go back to what was loaded or saved,
 * one added since goes, a profile's values likewise, and the backend
 * applies the values again. */
static void case_revert(const char *dir)
{
    void *preset = NULL;
    unsigned applied = 0, pg;
    float v = 0;

    load_params_lib(dir);
    nv2a_present_chain_override_load("crt-lottes", "maskDark", 0.8f);
    nv2a_present_chain_request("crt-lottes");
    nv2a_present_chain_params_publish(&preset, nv2a_present_chain_gen());
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);

    /* A loaded one moved, a new one added, then a saved state. */
    CHECK(nv2a_present_chain_param_set("maskDark", 0.3f));
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    CHECK(nv2a_present_chain_overrides_dirty() == 2);
    pg = nv2a_present_chain_params_gen();
    nv2a_present_chain_override_revert();
    CHECK(nv2a_present_chain_params_gen() != pg);
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    CHECK(nv2a_present_chain_override_get("crt-lottes", "maskDark", &v) && near(v, 0.8f));
    CHECK(!nv2a_present_chain_override_get("crt-lottes", "warpX", NULL));
    s_nset = 0;
    nv2a_present_chain_params_apply(&applied, record_set, &s_nset);
    CHECK(s_nset == 2 && near(set_value("maskDark"), 0.8f) && near(set_value("warpX"), 0.01f));

    /* After a Save the saved values are what Revert restores; a removal
     * since then comes back. */
    CHECK(nv2a_present_chain_param_set("warpX", 0.05f));
    nv2a_present_chain_overrides_saved();
    nv2a_present_chain_params_reset("crt-lottes");
    CHECK(nv2a_present_chain_overrides_dirty() == 2);
    nv2a_present_chain_override_revert();
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    CHECK(nv2a_present_chain_override_get("crt-lottes", "warpX", &v) && near(v, 0.05f));

    /* The profile scope, by slug: another profile's edit stays. */
    nv2a_present_chain_profile_value_load("mine", "warpX", 0.02f);
    nv2a_present_chain_profile_reset("mine", NULL);
    nv2a_present_chain_profile_reset("other", NULL);
    CHECK(nv2a_present_chain_profile_dirty("mine") == 1);
    nv2a_present_chain_profile_revert("mine");
    CHECK(nv2a_present_chain_profile_dirty("mine") == 0);
    CHECK(nv2a_present_chain_profile_value_get("mine", "warpX", &v) && near(v, 0.02f));
    CHECK(nv2a_present_chain_overrides_dirty() == 0);
    nv2a_present_chain_params_reset_for_test();
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    const char *dir = argc > 2 ? argv[2] : ".";
    char p[1024];

    if (!strcmp(c, "request")) {
        case_request();
    } else if (!strcmp(c, "rate")) {
        case_rate();
    } else if (!strcmp(c, "preset")) {
        case_preset();
    } else if (!strcmp(c, "missing")) {
        refused("no-such-librashader" MOD_SUFFIX);
    } else if (!strcmp(c, "bad_abi") || !strcmp(c, "old_api") || !strcmp(c, "no_exports")) {
        module_path(p, sizeof p, dir, c);
        refused(p);
    } else if (!strcmp(c, "params")) {
        case_params(dir);
    } else if (!strcmp(c, "revert")) {
        case_revert(dir);
    } else if (!strcmp(c, "profile")) {
        case_profile(dir);
    } else if (!strcmp(c, "params_cap")) {
        case_params_cap(dir);
    } else if (!strcmp(c, "params_key")) {
        case_params_key();
    } else if (!strcmp(c, "describe") || !strcmp(c, "describe_nolib")) {
        case_describe(dir, c[8] != 0);
    } else if (!strcmp(c, "good")) {
        const struct nv2a_lrs *l;
        /* Relative to the executable's directory, where the stand-in is. */
        snprintf(p, sizeof p, "fake_lrs_good%s", MOD_SUFFIX);
        if (strcmp(dir, recomp_exe_dir()))
            module_path(p, sizeof p, dir, "good");
        nv2a_present_chain_set_lib(p);
        l = nv2a_present_chain_lib();
        CHECK(l != NULL);
        if (l) {
            CHECK(l->abi == LIBRASHADER_CURRENT_ABI);
            CHECK(l->api == LIBRASHADER_CURRENT_VERSION);
            CHECK(l->sym("libra_instance_abi_version") != NULL);
            CHECK(l->sym("libra_preset_create") == NULL);
        }
        CHECK(nv2a_present_chain_lib() == l);
    } else {
        fprintf(stderr, "unknown case \"%s\"\n", c);
        return 2;
    }
    if (!s_fail)
        printf("present_chain %s: ok\n", c);
    return s_fail;
}
