/*
 * shader_profile_req.c - a profile as the present chain's request: the
 * start's choice between present.profile and present.shader (design D7) and
 * the request itself, which the menu makes too. Apart from shader_profile.c
 * because describing a stage loads librashader (nv2a_present_chain_describe).
 */
#include "shader_profile.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define getcwd _getcwd
#define HOST_WIN 1
#define req_yield() SwitchToThread()
#else
#include <sched.h>
#include <unistd.h>
#define HOST_WIN 0
#define req_yield() sched_yield()
#endif

#include "enhance_cfg.h"
#include "nv2a_present_chain.h"
#include "recomp_cfg.h"

#define LOGP "[SHADER] "
#define PATH_CAP 1024

/* kernel_path.c: <save root>/ShaderCache, 0 before xbox_path_init. */
int xbox_path_shader_cache(char *out, size_t n);

/* The user data root (xbox_enhance_init's), where user: stages live. */
static char s_root[PATH_CAP] = ".";

/* The request's stage lists and the profile being requested: large, so
 * static, and under s_req_lock. The start's deferred request runs on the
 * present thread at its first present, which can be while the menu's
 * thread makes one. */
static atomic_flag s_req_lock = ATOMIC_FLAG_INIT;
static struct nv2a_present_stage s_stage[SHADER_PROFILE_STAGES_MAX];
static struct shader_profile s_work;
/* The start's profile while its request waits for the first present. */
static struct shader_profile s_pending;
/* The profile the start chose, even when it failed: the menu shows it. */
static char s_start_slug[SHADER_PROFILE_NAME_MAX + 1];
/* The chain's generation right after the start's pending request: the
 * deferred half applies only while it stands (a menu request wins). */
static unsigned s_start_gen;
/* 1 once the deferred half put the profile's values in the store, until
 * the menu takes it (shader_profile_start_loaded). */
static atomic_int s_start_loaded;
/* 1 once the start's wait for the shader cache checked its stages and
 * said so. Each start clears it before its pending request; only that
 * request's prepare, under s_req_lock, sets it. */
static int s_start_waiting;

/* The start's deferred request: its stock fallback and its request go
 * through the generation check (request_profile_at). Not in the header. */
#define REQ_START 0x100u

/* Composed files a rename left under the old slug. The running chain may
 * still read one (rekey keeps its path), so each goes once a request has
 * named another file. Under req_lock. */
#define STALE_MAX 8
static char s_stale[STALE_MAX][SHADER_PROFILE_NAME_MAX + 1];
static int s_nstale;

static void req_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_req_lock, memory_order_acquire))
        req_yield();
}

static void req_unlock(void)
{
    atomic_flag_clear_explicit(&s_req_lock, memory_order_release);
}

void shader_profile_set_root(const char *root)
{
    snprintf(s_root, sizeof s_root, "%s", root && *root ? root : ".");
}

const char *shader_profile_root(void)
{
    return s_root;
}

/* p made absolute against the working directory: the chain reads a
 * relative request beside the executable, and root is often ".". */
static void absolute(char *p, size_t cap)
{
    char cwd[PATH_CAP], t[PATH_CAP];
    if (p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':') || !getcwd(cwd, sizeof cwd))
        return;
    shader_profile_path_join(cwd, p, HOST_WIN, t, sizeof t);
    snprintf(p, cap, "%s", t);
}

static void cache_remove(const char *slug)
{
    char cache[PATH_CAP], path[PATH_CAP];
    if (!slug || !*slug || !xbox_path_shader_cache(cache, sizeof cache))
        return;
    shader_profile_cache_path(cache, slug, path, sizeof path);
    absolute(path, sizeof path);
    remove(path);
}

static void stale_drop(const char *slug)
{
    for (int i = 0; i < s_nstale; i++)
        if (!strcmp(s_stale[i], slug)) {
            memmove(s_stale[i], s_stale[i + 1], sizeof s_stale[0] * (size_t)(s_nstale - i - 1));
            s_nstale--;
            return;
        }
}

/* A request named slug's file: every other stale file is out of use. */
static void stale_sweep(const char *slug)
{
    stale_drop(slug);
    for (int i = 0; i < s_nstale; i++)
        cache_remove(s_stale[i]);
    s_nstale = 0;
}

void shader_profile_cache_renamed(const char *from, const char *to)
{
    req_lock();
    stale_drop(to);
    stale_drop(from);
    if (s_nstale == STALE_MAX) {
        cache_remove(s_stale[0]);
        stale_drop(s_stale[0]);
    }
    snprintf(s_stale[s_nstale++], sizeof s_stale[0], "%s", from);
    req_unlock();
}

void shader_profile_cache_sweep(void)
{
    req_lock();
    stale_sweep("");
    req_unlock();
}

void shader_profile_cache_forget(const char *slug)
{
    req_lock();
    stale_drop(slug);
    cache_remove(slug);
    req_unlock();
}

static void fail(const char *slug, const char *err, int said)
{
    nv2a_present_chain_request("none");
    /* After the request, which clears it: the menu's red Chain header. */
    nv2a_present_chain_profile_error_set(err);
    if (!said)
        fprintf(stderr, LOGP "profile %s: %s; the present stays stock\n", slug, err);
}

/* fail(), or for the start's deferred half the same under the
 * generation check: a menu request made meanwhile keeps its chain. */
static void failed(const char *slug, unsigned flags, const char *err, int said)
{
    if (!(flags & REQ_START)) {
        fail(slug, err, said);
        return;
    }
    if (nv2a_present_chain_request_profile_at(s_start_gen, slug, NULL, NULL, 0, err) && !said)
        fprintf(stderr, LOGP "profile %s: %s; the present stays stock\n", slug, err);
}

static int request_locked(const struct shader_profile *p, unsigned flags, char *err,
                          size_t errlen);

int shader_profile_request(const struct shader_profile *p, char *err, size_t errlen)
{
    return shader_profile_request_with(p, 0, err, errlen);
}

int shader_profile_request_with(const struct shader_profile *p, unsigned flags, char *err,
                                size_t errlen)
{
    int rc;
    req_lock();
    rc = request_locked(p, flags & ~REQ_START, err, errlen);
    req_unlock();
    return rc;
}

static int request_locked(const struct shader_profile *p, unsigned flags, char *err,
                          size_t errlen)
{
    char path[SHADER_PROFILE_STAGES_MAX][PATH_CAP], out[PATH_CAP], cache[PATH_CAP];
    const char *paths[SHADER_PROFILE_STAGES_MAX], *labels[SHADER_PROFILE_STAGES_MAX];
    static const char *names[SHADER_PROFILE_STAGES_MAX][NV2A_PRESENT_PARAMS_MAX];
    static struct nv2a_present_param rows[NV2A_PRESENT_PARAMS_MAX];
    struct shader_profile_stage_params sp[SHADER_PROFILE_STAGES_MAX];
    char e[SHADER_PROFILE_ERR_MAX];
    int n, s;

    if (!err || !errlen) {
        err = e;
        errlen = sizeof e;
    }
    err[0] = 0;
    if (p != &s_work)
        s_work = *p;
    p = &s_work;
    n = p->nstages;
    if (n < 1) {
        snprintf(err, errlen, "no stages");
        failed(p->slug, flags, err, 0);
        return -1;
    }
    for (s = 0; s < n; s++) {
        int k;
        if (shader_profile_stage_path(p->stage[s], s_root, NULL, path[s], sizeof path[s], err,
                                      errlen) < 0) {
            failed(p->slug, flags, err, 0);
            return -1;
        }
        absolute(path[s], sizeof path[s]);
        paths[s] = path[s];
        labels[s] = p->stage[s];
        /* The names each stage declares, for the rows' stage tags (D4):
         * without the library there is no chain to tag, so none. */
        k = nv2a_present_chain_describe(path[s], rows, NV2A_PRESENT_PARAMS_MAX);
        shader_profile_stage_key(p->stage[s], s_stage[s].key, sizeof s_stage[s].key);
        /* The first stage sees the frame: the profile's choice, else a
         * preset under a crt folder, which its key alone cannot show. */
        s_stage[s].lines = NV2A_PRESENT_LINES_AUTO;
        if (s == 0)
            s_stage[s].lines = p->lines != NV2A_PRESENT_LINES_AUTO ? p->lines
                : nv2a_present_chain_ref_lines(path[s]) ? NV2A_PRESENT_LINES_GUEST
                : NV2A_PRESENT_LINES_AUTO;
        s_stage[s].nnames = k > 0 ? k : 0;
        for (int i = 0; i < s_stage[s].nnames; i++) {
            snprintf(s_stage[s].name[i], sizeof s_stage[s].name[i], "%s", rows[i].name);
            names[s][i] = s_stage[s].name[i];
        }
        sp[s].n = s_stage[s].nnames;
        sp[s].name = names[s];
    }
    /* One stage runs as its preset; a chain as one composed file. */
    if (n == 1) {
        snprintf(out, sizeof out, "%s", path[0]);
    } else {
        if (!xbox_path_shader_cache(cache, sizeof cache)) {
            snprintf(err, errlen, "no shader cache folder yet");
            failed(p->slug, flags, err, 0);
            return -1;
        }
        shader_profile_cache_path(cache, flags & SHADER_PROFILE_REQ_UNSAVED ? NULL : p->slug,
                                  out, sizeof out);
        absolute(out, sizeof out);
        if (shader_profile_compose(paths, labels, n, p->slug, out, NULL, err, errlen) < 0) {
            failed(p->slug, flags, err, 1);
            return -1;
        }
    }
    /* A menu request since the start's pending one: it stands, and the
     * store keeps what the menu put there. */
    if ((flags & REQ_START) && nv2a_present_chain_request_gen() != s_start_gen)
        return 0;
    /* Values under their owners, then into the profile scope keyed by
     * name: one value per name drives every stage that declares it. */
    if (!(flags & SHADER_PROFILE_REQ_KEEP)) {
        shader_profile_rehome(&s_work, sp, p->slug);
        nv2a_present_chain_profile_forget(p->slug);
        for (int i = 0; i < p->nvalues; i++)
            nv2a_present_chain_profile_value_load(p->slug, p->value[i].name, p->value[i].v);
    }
    if (!(flags & REQ_START))
        nv2a_present_chain_request_profile(p->slug, out, s_stage, n);
    else if (nv2a_present_chain_request_profile_at(s_start_gen, p->slug, out, s_stage, n, NULL))
        atomic_store(&s_start_loaded, 1);
    else
        return 0;
    stale_sweep(flags & SHADER_PROFILE_REQ_UNSAVED ? "" : p->slug);
    return 0;
}

int shader_profile_place(struct shader_profile *p)
{
    char path[PATH_CAP], e[SHADER_PROFILE_ERR_MAX];
    static struct nv2a_present_param rows[NV2A_PRESENT_PARAMS_MAX];
    static char name[SHADER_PROFILE_STAGES_MAX][NV2A_PRESENT_PARAMS_MAX][64];
    static const char *names[SHADER_PROFILE_STAGES_MAX][NV2A_PRESENT_PARAMS_MAX];
    struct shader_profile_stage_params sp[SHADER_PROFILE_STAGES_MAX];
    int ok = 1;

    req_lock();
    for (int s = 0; s < p->nstages && s < SHADER_PROFILE_STAGES_MAX; s++) {
        int k = -1;
        if (shader_profile_stage_path(p->stage[s], s_root, NULL, path, sizeof path, e,
                                      sizeof e) > 0)
            k = nv2a_present_chain_describe(path, rows, NV2A_PRESENT_PARAMS_MAX);
        if (k < 0)
            ok = 0;
        sp[s].n = k > 0 ? k : 0;
        for (int i = 0; i < sp[s].n; i++) {
            snprintf(name[s][i], sizeof name[s][i], "%s", rows[i].name);
            names[s][i] = name[s][i];
        }
        sp[s].name = names[s];
    }
    shader_profile_rehome(p, sp, p->slug);
    req_unlock();
    return ok ? 0 : -1;
}

int shader_profile_start_slug(char *out, size_t cap)
{
    snprintf(out, cap, "%s", s_start_slug);
    return s_start_slug[0] != 0;
}

int shader_profile_start_loaded(char *out, size_t cap)
{
    if (!atomic_exchange(&s_start_loaded, 0))
        return 0;
    snprintf(out, cap, "%s", s_start_slug);
    return 1;
}

/* The start's deferred half, on the present thread at its first present
 * (request_pending): it composes, and describing a stage loads librashader,
 * which waits for a presenter that wants it. */
static void start_prepare(void)
{
    char cache[PATH_CAP], path[PATH_CAP], err[SHADER_PROFILE_ERR_MAX];
    req_lock();
    /* A chain composes under the hdd's shader cache, and the boot can
     * present before the path module is up (cat opens its window first):
     * wait for the folder, one cheap test a present, rather than fail the
     * start. One stage needs no cache and goes on at once. */
    if (s_pending.nstages > 1 && !xbox_path_shader_cache(cache, sizeof cache)) {
        /* The stages do not need the folder: a missing one fails now,
         * not when the wait ends, and a wait that never ends (a host
         * that presents without path init) is in the log. */
        if (!s_start_waiting) {
            for (int s = 0; s < s_pending.nstages; s++)
                if (shader_profile_stage_path(s_pending.stage[s], s_root, NULL, path,
                                              sizeof path, err, sizeof err) < 0) {
                    failed(s_pending.slug, REQ_START, err, 0);
                    req_unlock();
                    return;
                }
            s_start_waiting = 1;
            fprintf(stderr, LOGP "profile %s: waiting for the shader cache folder\n",
                    s_pending.slug);
        }
        if (nv2a_present_chain_request_rearm(s_start_gen, start_prepare)) {
            req_unlock();
            return;
        }
    }
    request_locked(&s_pending, REQ_START, NULL, 0);
    req_unlock();
}

/* A layer's string value of key, or NULL; any other type is said once and
 * counts as not set. */
static const char *layer_string(const recomp_cfg *c, const char *key)
{
    recomp_cfg_type t;
    if (!c || recomp_cfg_find(c, key) < 0)
        return NULL;
    t = recomp_cfg_type_of(c, key);
    if (t != RCFG_STRING) {
        fprintf(stderr, LOGP "config warning: %s:%d: %s is not a string; ignored\n",
                recomp_cfg_name(c), recomp_cfg_line(c, key), key);
        return NULL;
    }
    return recomp_cfg_string(c, key, NULL);
}

static int is_none(const char *s)
{
    return !s || !*s || !strcmp(s, "none");
}

/* Load profile slug and request it; stock with the profile error on any
 * failure (D7: a chosen profile never falls through to present.shader). */
static void start_profile(const char *slug, const char *why)
{
    char dir[PATH_CAP], path[PATH_CAP], err[SHADER_PROFILE_ERR_MAX];
    int r;

    snprintf(s_start_slug, sizeof s_start_slug, "%s", slug);
    if (!shader_profile_slug_ok(slug)) {
        snprintf(err, sizeof err, "\"%s\" is not a profile file name (%s)", slug, why);
        fail(slug, err, 0);
        return;
    }
    if (!shader_profile_dir(dir, sizeof dir)) {
        snprintf(err, sizeof err, "no profiles under RECOMP_CONFIG=none (%s)", why);
        fail(slug, err, 0);
        return;
    }
    shader_profile_path(dir, slug, path, sizeof path);
    r = shader_profile_load(path, &s_pending, err, sizeof err);
    if (r == SHADER_PROFILE_NEWER)
        snprintf(err, sizeof err, "%s is of a newer format", path);
    if (r != SHADER_PROFILE_OK) {
        fail(slug, err, 0);
        return;
    }
    fprintf(stderr, LOGP "present.profile=%s (%s): %d stage%s\n", slug, why,
            s_pending.nstages, s_pending.nstages == 1 ? "" : "s");
    /* Requested at the first present: a chain is composed under the shader
     * cache, which the path module sets up after this call, and describing
     * any stage loads librashader, which only a presenter that runs a chain
     * should load. One stage or eight take the same path. */
    s_start_waiting = 0;
    nv2a_present_chain_request_pending(s_pending.slug, start_prepare);
    s_start_gen = nv2a_present_chain_request_gen();
}

int shader_profile_start(const char *root, char *shader, size_t cap)
{
    static const char *const src[] = { "title config", "config", "config.default.toml" };
    struct enhance_cfg_hit hit[4];
    const char *env_shader, *env_profile, *profile = NULL, *sh = NULL;
    char why[PATH_CAP];
    int i;

    shader_profile_set_root(root);
    s_start_slug[0] = 0;
    atomic_store(&s_start_loaded, 0);
    if (cap)
        shader[0] = 0;
    /* Both keys count as read in every layer, whichever decides. */
    enhance_cfg_layered("present.profile", NULL, hit, 4);
    enhance_cfg_layered("present.shader", NULL, hit, 4);
    env_shader = enhance_cfg_env("present.shader");
    env_profile = enhance_cfg_env("present.profile");

    /* 1. RECOMP_PRESENT_SHADER: that preset, as before profiles. */
    if (env_shader) {
        for (i = 0; !profile && i < 3; i++)
            profile = layer_string(enhance_cfg_file(i), "present.profile");
        if (!is_none(env_profile) || !is_none(profile))
            fprintf(stderr, LOGP "present.profile=%s ignored: RECOMP_PRESENT_SHADER is set\n",
                    !is_none(env_profile) ? env_profile : profile);
        nv2a_present_chain_request(env_shader);
        snprintf(shader, cap, "%s", env_shader);
        return is_none(env_shader) ? 0 : 1;
    }
    /* 2. RECOMP_PRESENT_PROFILE, unless none. */
    if (!is_none(env_profile)) {
        start_profile(env_profile, "RECOMP_PRESENT_PROFILE");
        return 2;
    }
    /* 3. The highest layer that sets either key decides, alone. */
    for (i = 0; i < 3; i++) {
        const recomp_cfg *c = enhance_cfg_file(i);
        profile = layer_string(c, "present.profile");
        sh = layer_string(c, "present.shader");
        if (!profile && !sh)
            continue;
        if (!is_none(profile)) {
            snprintf(why, sizeof why, "%s", recomp_cfg_name(c) ? recomp_cfg_name(c) : src[i]);
            start_profile(profile, why);
            return 2;
        }
        if (!is_none(sh)) {
            nv2a_present_chain_request(sh);
            snprintf(shader, cap, "%s", sh);
            return 1;
        }
        break;
    }
    /* 4. Stock. */
    nv2a_present_chain_request("none");
    return 0;
}
