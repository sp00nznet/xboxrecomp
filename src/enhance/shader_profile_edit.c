/*
 * shader_profile_edit.c - the Shaders page's profiles; see
 * shader_profile_edit.h.
 */
#include "shader_profile_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_cfg.h"
#include "nv2a_present_chain.h"
#include "recomp_cfg.h"

#define LOGP "[SHADER] "
#define PATH_CAP 1024
#define ENTRIES_MAX (2 + SHADER_PROFILE_STAGES_MAX + SHADER_PROFILE_LIST_MAX + 8)
#define VALS_MAX SHADER_PROFILE_VALUES_MAX

static struct shader_edit_entry s_e[ENTRIES_MAX], s_tmp[ENTRIES_MAX];
static int s_n, s_active, s_init;
/* The choice the config holds (what Save last wrote, or the start's):
 * kind and slug of an entry, or SAVED_UNKNOWN after a Delete left a lower
 * layer's key in charge. */
#define SAVED_UNKNOWN (-1)
static int s_saved_kind;
static char s_saved_slug[SHADER_PROFILE_REF_MAX];
/* A profile file was deleted this session (the exit dialog says so). */
static int s_deleted;

/* The chosen chain as analyse() last found it. */
static struct shader_profile_compose s_info;
static int s_info_ok;
static char s_info_err[SHADER_PROFILE_ERR_MAX];
static int s_missing[SHADER_PROFILE_STAGES_MAX];
static char s_names[SHADER_PROFILE_STAGES_MAX][NV2A_PRESENT_PARAMS_MAX][64];
static int s_nnames[SHADER_PROFILE_STAGES_MAX];

/* Large scratch, one use at a time. */
static struct shader_profile_entry s_list[SHADER_PROFILE_LIST_MAX];
static struct shader_profile s_p, s_fp;
static struct nv2a_present_param s_rows[NV2A_PRESENT_PARAMS_MAX];
static char s_vname[VALS_MAX][64];
static float s_vv[VALS_MAX];

/* The config staging: two keys at most. */
static char s_ckey[2][32], s_cval[2][SHADER_PROFILE_REF_MAX * 2];
static int s_cnull[2];

static void say(char *msg, size_t n, const char *fmt, const char *a)
{
    if (msg && n)
        snprintf(msg, n, fmt, a ? a : "");
}

static struct shader_edit_entry *act(void)
{
    return &s_e[s_active];
}

static int edited(const struct shader_edit_entry *e)
{
    if (e->kind != SHADER_EDIT_USER || e->state != SHADER_PROFILE_OK)
        return 0;
    if (!e->file[0] || strcmp(e->file, e->slug) || strcmp(e->name, e->fname)
        || e->nstages != e->fnstages)
        return 1;
    for (int s = 0; s < e->nstages; s++)
        if (strcmp(e->stage[s], e->fstage[s]))
            return 1;
    return 0;
}

/* The preset key a one-stage entry's values live under (preset scope). */
static void preset_key(const struct shader_edit_entry *e, char *out, size_t cap)
{
    nv2a_present_chain_preset_key(e->stage[0], out, cap);
}

static int lower_sets(const char *key)
{
    for (int l = enhance_cfg_file(0) ? 1 : 2; l <= 2; l++) {
        const recomp_cfg *c = enhance_cfg_file(l);
        if (c && recomp_cfg_find(c, key) >= 0)
            return 1;
    }
    return 0;
}

/* ---- the entries ---- */

static struct shader_edit_entry *add_entry(struct shader_edit_entry *a, int *n, int kind,
                                           const char *name, const char *slug)
{
    struct shader_edit_entry *e;
    if (*n >= ENTRIES_MAX)
        return NULL;
    e = &a[(*n)++];
    memset(e, 0, sizeof *e);
    e->kind = kind;
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->slug, sizeof e->slug, "%s", slug);
    return e;
}

static void one_stage(struct shader_edit_entry *e, const char *ref)
{
    e->nstages = 1;
    snprintf(e->stage[0], sizeof e->stage[0], "%s", ref);
}

static void from_list(struct shader_edit_entry *e, const struct shader_profile_entry *l)
{
    e->state = l->state;
    snprintf(e->file, sizeof e->file, "%s", l->slug);
    snprintf(e->fname, sizeof e->fname, "%s", l->name);
    snprintf(e->err, sizeof e->err, "%s", l->err);
    if (l->state == SHADER_PROFILE_NEWER && !e->err[0])
        snprintf(e->err, sizeof e->err, "%s.toml is of a newer format", l->slug);
    e->nstages = e->fnstages = l->nstages;
    memcpy(e->stage, l->stage, sizeof e->stage);
    memcpy(e->fstage, l->stage, sizeof e->fstage);
}

static int find_user(const char *slug)
{
    for (int i = 0; i < s_n; i++)
        if (s_e[i].kind == SHADER_EDIT_USER && !strcmp(s_e[i].slug, slug))
            return i;
    return -1;
}

/* The start's profile counts as loaded once its deferred request ran at
 * the first present: its file's values are in the store from then on. A
 * menu request before that dropped it, and loaded the file itself. */
static void start_sync(void)
{
    char slug[SHADER_PROFILE_NAME_MAX + 1];
    if (!s_init || !shader_profile_start_loaded(slug, sizeof slug))
        return;
    for (int i = 0; i < s_n; i++)
        if (s_e[i].kind == SHADER_EDIT_USER && !strcmp(s_e[i].file, slug))
            s_e[i].loaded = 1;
}

/* The fixed head: Off, the bundled presets, the config's path when the
 * start requested one. */
static void head(struct shader_edit_entry *a, int *n, const char *path)
{
    struct shader_edit_entry *e;
    char key[64];
    *n = 0;
    add_entry(a, n, SHADER_EDIT_OFF, "Off", "");
    for (int i = 0; nv2a_present_chain_names[i]; i++) {
        e = add_entry(a, n, SHADER_EDIT_BUILTIN, nv2a_present_chain_names[i],
                      nv2a_present_chain_names[i]);
        one_stage(e, nv2a_present_chain_names[i]);
    }
    if (path && *path) {
        const char *b = path;
        for (const char *s = path; *s; s++)
            if (*s == '/' || *s == '\\')
                b = s + 1;
        nv2a_present_chain_preset_key(path, key, sizeof key);
        e = add_entry(a, n, SHADER_EDIT_PATH, b, key);
        if (e)
            one_stage(e, path);
    }
}

static int list_folder(void)
{
    char dir[PATH_CAP];
    if (!shader_profile_dir(dir, sizeof dir))
        return 0;
    return shader_profile_list(dir, s_list, SHADER_PROFILE_LIST_MAX);
}

/* The chosen chain's stages found, described and composed, for the rows'
 * notes and the Chain header's pass count. */
static void analyse(void)
{
    const struct shader_edit_entry *e = act();
    char path[SHADER_PROFILE_STAGES_MAX][PATH_CAP], err[SHADER_PROFILE_ERR_MAX];
    const char *paths[SHADER_PROFILE_STAGES_MAX], *labels[SHADER_PROFILE_STAGES_MAX];
    int ok = 1;
    char *text;

    memset(&s_info, 0, sizeof s_info);
    memset(s_missing, 0, sizeof s_missing);
    memset(s_nnames, 0, sizeof s_nnames);
    s_info_ok = 0;
    s_info_err[0] = 0;
    if (e->kind == SHADER_EDIT_OFF || e->state != SHADER_PROFILE_OK)
        return;
    for (int s = 0; s < e->nstages; s++) {
        int k;
        paths[s] = path[s];
        labels[s] = e->stage[s];
        if (shader_profile_stage_path(e->stage[s], shader_profile_root(), NULL, path[s],
                                      sizeof path[s], err, sizeof err) < 0) {
            if (ok)
                snprintf(s_info_err, sizeof s_info_err, "stage %d: %s", s + 1, err);
            s_missing[s] = 1;
            ok = 0;
            continue;
        }
        k = nv2a_present_chain_describe(path[s], s_rows, NV2A_PRESENT_PARAMS_MAX);
        s_nnames[s] = k > 0 ? k : 0;
        for (int i = 0; i < s_nnames[s]; i++)
            snprintf(s_names[s][i], sizeof s_names[s][i], "%s", s_rows[i].name);
    }
    if (!ok)
        return;
    text = shader_profile_compose_text(paths, labels, e->nstages, e->slug, &s_info, err,
                                       sizeof err);
    if (!text) {
        snprintf(s_info_err, sizeof s_info_err, "%s", err);
        return;
    }
    free(text);
    s_info_ok = 1;
}

void shader_edit_refresh(void)
{
    char slug[SHADER_PROFILE_REF_MAX], req[SHADER_PROFILE_REF_MAX];
    int n, nl, old_active = s_active, new_active = 0;
    const char *path = NULL;
    static unsigned char kept[ENTRIES_MAX];

    req[0] = slug[0] = 0;
    if (!s_init) {
        if (!nv2a_present_chain_requested_profile(slug, sizeof slug))
            shader_profile_start_slug(slug, sizeof slug);
        nv2a_present_chain_requested(req, sizeof req);
        if (!slug[0] && strcmp(req, "none") && shader_profile_bundled(req) < 0)
            path = req;
    } else {
        for (int i = 0; i < s_n; i++)
            if (s_e[i].kind == SHADER_EDIT_PATH)
                path = s_e[i].stage[0];
    }
    head(s_tmp, &n, path);
    if (s_init) {
        /* The head keeps its place: Off, then the bundled, then a path. */
        if (old_active < n)
            new_active = old_active;
    }
    memset(kept, 0, sizeof kept);
    nl = list_folder();
    for (int l = 0; l < nl; l++) {
        int o = -1;
        for (int i = 0; i < s_n && s_init; i++)
            if (s_e[i].kind == SHADER_EDIT_USER && s_e[i].file[0]
                && !strcmp(s_e[i].file, s_list[l].slug))
                o = i;
        if (n >= ENTRIES_MAX)
            break;
        if (o >= 0 && (edited(&s_e[o]) || s_e[o].loaded || o == old_active)) {
            /* Its edits and values live on; the file is read at Save. */
            s_tmp[n] = s_e[o];
            kept[o] = 1;
        } else {
            struct shader_edit_entry *e = add_entry(s_tmp, &n, SHADER_EDIT_USER,
                                                    s_list[l].name, s_list[l].slug);
            from_list(e, &s_list[l]);
            if (o >= 0) {
                kept[o] = 1;
                e->loaded = s_e[o].loaded;
            }
            n--;
        }
        if (o == old_active && s_init)
            new_active = n;
        n++;
    }
    /* Unsaved profiles, and any with edits whose file went away. */
    for (int i = 0; i < s_n && s_init; i++) {
        if (s_e[i].kind != SHADER_EDIT_USER || kept[i] || n >= ENTRIES_MAX)
            continue;
        if (s_e[i].file[0] && !edited(&s_e[i]) && i != old_active)
            continue;
        s_tmp[n] = s_e[i];
        if (s_tmp[n].file[0] && s_tmp[n].state == SHADER_PROFILE_OK && i == old_active
            && !edited(&s_e[i]))
            s_tmp[n].file[0] = 0;   /* its file went: unsaved again */
        if (i == old_active)
            new_active = n;
        n++;
    }
    memcpy(s_e, s_tmp, sizeof s_e[0] * (size_t)n);
    s_n = n;
    s_active = new_active;
    if (!s_init) {
        s_init = 1;
        if (slug[0]) {
            int i = find_user(slug);
            if (i < 0 && s_n < ENTRIES_MAX) {
                /* Chosen at start, with no file in the list. */
                struct shader_edit_entry *e = add_entry(s_e, &s_n, SHADER_EDIT_USER, slug, slug);
                e->state = SHADER_EDIT_MISSING;
                snprintf(e->file, sizeof e->file, "%s", slug);
                if (!nv2a_present_chain_profile_error_get(e->err, sizeof e->err))
                    snprintf(e->err, sizeof e->err, "%s.toml is missing", slug);
                i = s_n - 1;
            }
            if (i >= 0) {
                s_active = i;
                /* Its values are in the store once the start's deferred
                 * request ran (start_sync), not before. */
                s_e[i].loaded = 0;
                if (s_e[i].state == SHADER_PROFILE_OK)
                    nv2a_present_chain_profile_error_get(s_e[i].err, sizeof s_e[i].err);
            }
        } else if (strcmp(req, "none")) {
            for (int i = 0; i < s_n; i++)
                if (s_e[i].kind != SHADER_EDIT_OFF && s_e[i].kind != SHADER_EDIT_USER
                    && !strcmp(s_e[i].stage[0], req))
                    s_active = i;
        }
        s_saved_kind = act()->kind;
        snprintf(s_saved_slug, sizeof s_saved_slug, "%s", act()->slug);
    }
    start_sync();
    analyse();
}

int shader_edit_count(void)
{
    return s_n ? s_n : 1;
}

const struct shader_edit_entry *shader_edit_entry(int i)
{
    static struct shader_edit_entry off = { .kind = SHADER_EDIT_OFF, .name = "Off" };
    return i >= 0 && i < s_n ? &s_e[i] : i == 0 ? &off : NULL;
}

int shader_edit_active(void)
{
    return s_active;
}

const char *shader_edit_forced(void)
{
    const char *p;
    if (enhance_cfg_env("present.shader"))
        return "RECOMP_PRESENT_SHADER";
    p = enhance_cfg_env("present.profile");
    return p && strcmp(p, "none") ? "RECOMP_PRESENT_PROFILE" : NULL;
}

/* An env var holds the chain: every edit is refused, as Save refuses it,
 * since the next start would drop the edit anyway. */
static int refused(char *msg, size_t n)
{
    const char *f = shader_edit_forced();
    if (f)
        say(msg, n, "Set by %s", f);
    return f != NULL;
}

/* ---- requests ---- */

/* p from entry e: name, slug and stages; values from its file when the
 * store has none yet. The request's flags. */
static unsigned build(const struct shader_edit_entry *e, struct shader_profile *p)
{
    char dir[PATH_CAP], path[PATH_CAP], err[SHADER_PROFILE_ERR_MAX];
    unsigned flags = SHADER_PROFILE_REQ_KEEP;

    memset(p, 0, sizeof *p);
    if (!e->loaded && e->file[0] && shader_profile_dir(dir, sizeof dir)) {
        shader_profile_path(dir, e->file, path, sizeof path);
        if (shader_profile_load(path, p, err, sizeof err) == SHADER_PROFILE_OK)
            flags = 0;
        else
            memset(p, 0, sizeof *p);
    }
    snprintf(p->name, sizeof p->name, "%s", e->name);
    snprintf(p->slug, sizeof p->slug, "%s", e->slug);
    p->version = SHADER_PROFILE_VERSION;
    p->nstages = e->nstages;
    memcpy(p->stage, e->stage, sizeof p->stage);
    if (!e->file[0])
        flags |= SHADER_PROFILE_REQ_UNSAVED;
    return flags;
}

/* Request the chosen entry as it stands now. */
static int request(char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char err[SHADER_PROFILE_ERR_MAX];
    int rc = 0;

    start_sync();
    switch (e->kind) {
    case SHADER_EDIT_OFF:
        nv2a_present_chain_request("none");
        shader_profile_cache_sweep();
        break;
    case SHADER_EDIT_BUILTIN:
    case SHADER_EDIT_PATH:
        nv2a_present_chain_request(e->stage[0]);
        shader_profile_cache_sweep();
        break;
    default: {
        unsigned flags;
        if (e->state != SHADER_PROFILE_OK) {
            nv2a_present_chain_request("none");
            nv2a_present_chain_profile_error_set(e->err);
            break;
        }
        flags = build(e, &s_p);
        rc = shader_profile_request_with(&s_p, flags, err, sizeof err);
        if (!(flags & SHADER_PROFILE_REQ_KEEP) && rc == 0)
            e->loaded = 1;
        if (rc < 0)
            say(msg, n, "%s", err);
        break;
    }
    }
    analyse();
    return rc;
}

int shader_edit_choose(int i, char *msg, size_t n)
{
    const char *f = shader_edit_forced();
    if (f) {
        say(msg, n, "Set by %s", f);
        return -1;
    }
    if (i < 0 || i >= s_n)
        return -1;
    if (s_e[i].kind == SHADER_EDIT_USER && s_e[i].state != SHADER_PROFILE_OK) {
        say(msg, n, "%s", s_e[i].err);
        return -1;
    }
    s_active = i;
    return request(msg, n);
}

int shader_edit_step(int dir, char *msg, size_t n)
{
    int i = s_active;
    if (shader_edit_forced())
        return shader_edit_choose(i, msg, n);
    for (int k = 0; k < s_n; k++) {
        i = (i + (dir < 0 ? s_n - 1 : 1)) % s_n;
        if (s_e[i].kind != SHADER_EDIT_USER || s_e[i].state == SHADER_PROFILE_OK)
            return shader_edit_choose(i, msg, n);
    }
    return -1;
}

int shader_edit_info(struct shader_profile_compose *info, char *err, size_t errlen)
{
    if (info)
        *info = s_info;
    if (err && errlen)
        snprintf(err, errlen, "%s", s_info_err);
    return s_info_ok;
}

int shader_edit_stage_missing(int s)
{
    return s >= 0 && s < SHADER_PROFILE_STAGES_MAX && s_missing[s];
}

const char (*shader_edit_stage_names(int s, int *n))[64]
{
    if (s < 0 || s >= SHADER_PROFILE_STAGES_MAX) {
        *n = 0;
        return NULL;
    }
    *n = s_nnames[s];
    return s_names[s];
}

/* ---- new profiles and the fork ---- */

static int name_taken(const char *name, int except)
{
    for (int i = 0; i < s_n; i++)
        if (i != except && s_e[i].kind == SHADER_EDIT_USER && !strcmp(s_e[i].name, name))
            return 1;
    return 0;
}

/* A slug no other profile holds, as its slug or its file. */
static int slug_taken(const char *slug, int except)
{
    for (int i = 0; i < s_n; i++)
        if (i != except && s_e[i].kind == SHADER_EDIT_USER
            && (!strcmp(s_e[i].slug, slug) || !strcmp(s_e[i].file, slug)))
            return 1;
    return 0;
}

static void free_slug(const char *name, char *out, size_t cap)
{
    static const char *taken[2 * ENTRIES_MAX];
    int nt = 0;
    for (int i = 0; i < s_n; i++)
        if (s_e[i].kind == SHADER_EDIT_USER) {
            taken[nt++] = s_e[i].slug;
            if (s_e[i].file[0])
                taken[nt++] = s_e[i].file;
        }
    shader_profile_slug_free(name, taken, nt, out, cap);
}

/* base, or "base 2", "base 3" ... the first that no profile is named. */
static void free_name(const char *base, char *out, size_t cap)
{
    snprintf(out, cap, "%s", base);
    for (int k = 2; name_taken(out, -1) && k < 1000; k++)
        snprintf(out, cap, "%.*s %d", SHADER_PROFILE_NAME_MAX - 5, base, k);
}

/* A new unsaved profile named after base, with the chosen entry's chain and
 * values, chosen (not yet requested). Its index, or -1. */
static int clone_active(const char *base, char *msg, size_t n)
{
    struct shader_edit_entry *src = act(), *e;
    char name[SHADER_PROFILE_NAME_MAX + 1], slug[SHADER_PROFILE_NAME_MAX + 1];
    int count = 0;

    start_sync();
    if (src->kind == SHADER_EDIT_OFF || src->state != SHADER_PROFILE_OK) {
        say(msg, n, "Pick a shader first", NULL);
        return -1;
    }
    free_name(base, name, sizeof name);
    free_slug(name, slug, sizeof slug);
    if (s_n >= ENTRIES_MAX) {
        say(msg, n, "Too many profiles", NULL);
        return -1;
    }
    e = &s_e[s_n];
    memset(e, 0, sizeof *e);
    e->kind = SHADER_EDIT_USER;
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->slug, sizeof e->slug, "%s", slug);
    e->nstages = src->nstages;
    memcpy(e->stage, src->stage, sizeof e->stage);
    e->loaded = 1;
    /* Its values: the profile's, or a preset's overrides. A copy loads as
     * saved, but the profile has no file, so it counts as unsaved. */
    nv2a_present_chain_profile_forget(slug);
    if (src->kind == SHADER_EDIT_USER) {
        if (!src->loaded) {
            /* Never chosen this session: its file's values. */
            build(src, &s_fp);
            for (int i = 0; i < s_fp.nvalues; i++)
                nv2a_present_chain_profile_value_load(slug, s_fp.value[i].name,
                                                      s_fp.value[i].v);
        } else {
            count = nv2a_present_chain_profile_values(src->slug, s_vname, s_vv, VALS_MAX);
            for (int i = 0; i < count; i++)
                nv2a_present_chain_profile_value_load(slug, s_vname[i], s_vv[i]);
        }
    } else {
        char key[64], k[64], nm[64];
        float v;
        int has, dirty;
        preset_key(src, key, sizeof key);
        for (int i = 0; nv2a_present_chain_override_at(i, k, sizeof k, nm, sizeof nm, &v, &has,
                                                       &dirty); i++)
            if (has && !strcmp(k, key))
                nv2a_present_chain_profile_value_load(slug, nm, v);
    }
    s_active = s_n++;
    return s_active;
}

/* The chosen entry as a profile that can be edited: itself, or a fork of a
 * bundled preset or a path into "<name> custom". */
static int editable(char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char base[SHADER_PROFILE_NAME_MAX + 1];
    if (refused(msg, n))
        return -1;
    if (e->kind == SHADER_EDIT_USER)
        return e->state == SHADER_PROFILE_OK ? 0 : (say(msg, n, "%s", e->err), -1);
    if (e->kind == SHADER_EDIT_OFF) {
        say(msg, n, "Pick a shader first", NULL);
        return -1;
    }
    snprintf(base, sizeof base, "%.*s custom", SHADER_PROFILE_NAME_MAX - 7, e->name);
    return clone_active(base, msg, n) < 0 ? -1 : 0;
}

int shader_edit_new(int copy, char *msg, size_t n)
{
    char base[SHADER_PROFILE_NAME_MAX + 1];
    if (refused(msg, n))
        return -1;
    if (copy) {
        snprintf(base, sizeof base, "%.*s copy", SHADER_PROFILE_NAME_MAX - 5, act()->name);
    } else {
        int k = 1;
        do
            snprintf(base, sizeof base, "Profile %d", k++);
        while (name_taken(base, -1) && k < 1000);
    }
    if (clone_active(base, msg, n) < 0)
        return -1;
    return request(msg, n);
}

/* ---- chain edits ---- */

int shader_edit_move(int s, int dir, char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char t[SHADER_PROFILE_REF_MAX];
    int d = s + (dir < 0 ? -1 : 1);
    if (e->kind == SHADER_EDIT_OFF || s < 0 || s >= e->nstages || d < 0 || d >= e->nstages)
        return 0;
    if (editable(msg, n) < 0)
        return -1;
    e = act();
    memcpy(t, e->stage[s], sizeof t);
    memcpy(e->stage[s], e->stage[d], sizeof t);
    memcpy(e->stage[d], t, sizeof t);
    return request(msg, n);
}

int shader_edit_remove(int s, char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    if (s < 0 || s >= e->nstages)
        return -1;
    if (e->nstages <= 1) {
        say(msg, n, "The last stage stays: pick Off instead", NULL);
        return -1;
    }
    if (editable(msg, n) < 0)
        return -1;
    e = act();
    memmove(e->stage[s], e->stage[s + 1], sizeof e->stage[0] * (size_t)(e->nstages - s - 1));
    e->nstages--;
    memset(e->stage[e->nstages], 0, sizeof e->stage[0]);
    return request(msg, n);
}

int shader_edit_add_full(char *msg, size_t n)
{
    const struct shader_edit_entry *e = act();
    char lim[64];
    if (refused(msg, n))
        return 1;
    if (e->kind != SHADER_EDIT_OFF && e->nstages >= SHADER_PROFILE_STAGES_MAX) {
        snprintf(lim, sizeof lim, "%d stages is the most a chain holds",
                 SHADER_PROFILE_STAGES_MAX);
        say(msg, n, "%s", lim);
        return 1;
    }
    if (s_info_ok && s_info.passes >= SHADER_PROFILE_PASSES_MAX) {
        snprintf(lim, sizeof lim, "%d passes is the most a chain holds",
                 SHADER_PROFILE_PASSES_MAX);
        say(msg, n, "%s", lim);
        return 1;
    }
    return 0;
}

int shader_edit_add(const char *ref, char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char path[SHADER_PROFILE_STAGES_MAX + 1][PATH_CAP], err[SHADER_PROFILE_ERR_MAX];
    const char *paths[SHADER_PROFILE_STAGES_MAX + 1], *labels[SHADER_PROFILE_STAGES_MAX + 1];
    char *text;
    int k;

    if (refused(msg, n))
        return -1;
    if (e->kind == SHADER_EDIT_OFF) {
        say(msg, n, "Pick a shader first", NULL);
        return -1;
    }
    if (shader_edit_add_full(msg, n))
        return -1;
    for (k = 0; k <= e->nstages; k++) {
        const char *r = k < e->nstages ? e->stage[k] : ref;
        if (shader_profile_stage_path(r, shader_profile_root(), NULL, path[k], sizeof path[k],
                                      err, sizeof err) < 0) {
            say(msg, n, "%s", err);
            return -1;
        }
        paths[k] = path[k];
        labels[k] = r;
    }
    /* Parsed by the library as the descriptor dump does, with no GPU. */
    if (nv2a_present_chain_describe(path[e->nstages], s_rows, NV2A_PRESENT_PARAMS_MAX) < 0) {
        say(msg, n, "%s does not parse (see the log)", ref);
        return -1;
    }
    text = shader_profile_compose_text(paths, labels, e->nstages + 1, e->slug, NULL, err,
                                       sizeof err);
    if (!text) {
        say(msg, n, "%s", err);
        return -1;
    }
    free(text);
    if (editable(msg, n) < 0)
        return -1;
    e = act();
    snprintf(e->stage[e->nstages], sizeof e->stage[0], "%s", ref);
    e->nstages++;
    return request(msg, n);
}

int shader_edit_presets(char (*ref)[SHADER_PROFILE_REF_MAX], int cap)
{
    static char stem[SHADER_PROFILE_LIST_MAX][64];
    char dir[PATH_CAP];
    int n = 0, k;

    for (int i = 0; nv2a_present_chain_names[i] && n < cap; i++)
        snprintf(ref[n++], sizeof ref[0], "%s", nv2a_present_chain_names[i]);
    shader_profile_path_join(shader_profile_root(), "shaders", 0, dir, sizeof dir);
    k = shader_profile_list_presets(dir, stem, SHADER_PROFILE_LIST_MAX);
    for (int i = 0; i < k && n < cap; i++)
        snprintf(ref[n++], sizeof ref[0], "user:%s", stem[i]);
    return n;
}

/* ---- rename and delete ---- */

int shader_edit_rename(const char *name, char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char slug[SHADER_PROFILE_NAME_MAX + 1];

    if (refused(msg, n))
        return -1;
    if (e->kind != SHADER_EDIT_USER || e->state != SHADER_PROFILE_OK) {
        say(msg, n, "Only your own profiles can be renamed", NULL);
        return -1;
    }
    if (!name || !*name) {
        say(msg, n, "A name cannot be empty", NULL);
        return -1;
    }
    if (!shader_profile_name_ok(name) || !shader_profile_slug(name, slug, sizeof slug)) {
        say(msg, n, "\"%s\" is not a name a profile can take", name);
        return -1;
    }
    if (slug_taken(slug, s_active) || name_taken(name, s_active)) {
        say(msg, n, "A profile named \"%s\" exists", name);
        return -1;
    }
    if (strcmp(slug, e->slug)) {
        nv2a_present_chain_profile_rekey(e->slug, slug);
        shader_profile_cache_renamed(e->slug, slug);
    }
    snprintf(e->slug, sizeof e->slug, "%s", slug);
    snprintf(e->name, sizeof e->name, "%s", name);
    return 0;
}

int shader_edit_delete(char *msg, size_t n)
{
    struct shader_edit_entry *e = act();
    char dir[PATH_CAP], err[SHADER_PROFILE_ERR_MAX * 2], name[SHADER_PROFILE_NAME_MAX + 1];
    char stuck[PATH_CAP];

    if (refused(msg, n))
        return -1;
    if (e->kind != SHADER_EDIT_USER) {
        say(msg, n, "Only your own profiles can be deleted", NULL);
        return -1;
    }
    snprintf(name, sizeof name, "%s", e->name);
    stuck[0] = 0;
    if (e->file[0]) {
        int names = s_saved_kind == SHADER_EDIT_USER && !strcmp(s_saved_slug, e->file);
        int decides = 0, written = enhance_cfg_file(0) ? 0 : 1;
        if (!shader_profile_dir(dir, sizeof dir)) {
            say(msg, n, "No profiles under RECOMP_CONFIG=none", NULL);
            return -1;
        }
        if (e->state != SHADER_EDIT_MISSING
            && shader_profile_delete(dir, e->file, err, sizeof err) < 0) {
            say(msg, n, "%s", err);
            return -1;
        }
        /* The config never points at a deleted file: the key goes in the
         * same step, not at Save, from every layer that names it (a user
         * file under a title file would name it at the next start). The
         * written file is known from the last Save as well as from its
         * parse; the defaults ship read-only, so a key there is reported. */
        for (int l = written; l <= 2; l++) {
            const recomp_cfg *c = enhance_cfg_file(l);
            const char *path = l == written ? enhance_cfg_user_path()
                               : c              ? recomp_cfg_name(c)
                                                : NULL;
            const char *keys[1] = { "present.profile" }, *vals[1] = { NULL };
            int has = c && recomp_cfg_find(c, "present.profile") >= 0;
            int here = has && recomp_cfg_type_of(c, "present.profile") == RCFG_STRING
                       && !strcmp(recomp_cfg_string(c, "present.profile", ""), e->file);
            if (l == written)
                here |= names;
            if (!here) {
                /* Another value below the written file decides once the
                 * written file's key is gone. */
                decides |= has && l > written;
                continue;
            }
            names = 1;
            if (l < 2 && path && enhance_cfg_patch(path, keys, vals, 1, err, sizeof err) >= 0)
                continue;
            if (l < 2)
                fprintf(stderr, LOGP "menu: removing present.profile from %s failed: %s\n",
                        path ? path : "?", err);
            decides |= l > written;
            if (!stuck[0])
                snprintf(stuck, sizeof stuck, "%s", path ? path : "a config file");
        }
        if (lower_sets("present.shader"))
            decides = 1;
        if (names) {
            /* A lower layer's key may decide now: Save writes Off over it. */
            s_saved_kind = decides ? SAVED_UNKNOWN : SHADER_EDIT_OFF;
            s_saved_slug[0] = 0;
        }
        fprintf(stderr, LOGP "menu: deleted profile %s (%s.toml)\n", name, e->file);
        s_deleted = 1;
    }
    nv2a_present_chain_profile_forget(e->slug);
    nv2a_present_chain_request("none");
    shader_profile_cache_forget(e->slug);
    if (e->file[0] && strcmp(e->file, e->slug))
        shader_profile_cache_forget(e->file);
    memmove(&s_e[s_active], &s_e[s_active + 1], sizeof s_e[0] * (size_t)(s_n - s_active - 1));
    s_n--;
    s_active = 0;
    if (stuck[0] && msg && n)
        snprintf(msg, n, "Deleted %s; present.profile still names it in %s", name, stuck);
    else
        say(msg, n, "Deleted %s", name);
    analyse();
    return 0;
}

/* ---- values ---- */

static void reset_names(const struct shader_edit_entry *e, int s)
{
    if (e->kind == SHADER_EDIT_BUILTIN || e->kind == SHADER_EDIT_PATH) {
        char key[64];
        preset_key(e, key, sizeof key);
        nv2a_present_chain_params_reset(key);
        return;
    }
    if (e->kind != SHADER_EDIT_USER)
        return;
    if (s < 0) {
        nv2a_present_chain_profile_reset(e->slug, NULL);
        return;
    }
    for (int i = 0; i < s_nnames[s]; i++) {
        int owned = 1;
        for (int t = 0; t < s && owned; t++)
            for (int j = 0; j < s_nnames[t] && owned; j++)
                owned = strcmp(s_names[t][j], s_names[s][i]) != 0;
        if (owned)
            nv2a_present_chain_profile_reset(e->slug, s_names[s][i]);
    }
}

void shader_edit_reset_values(void)
{
    reset_names(act(), -1);
}

void shader_edit_reset_stage(int s)
{
    if (s >= 0 && s < act()->nstages)
        reset_names(act(), act()->nstages == 1 ? -1 : s);
}

/* ---- dirty state and Save ---- */

static int same_choice(void)
{
    const struct shader_edit_entry *e = act();
    if (s_saved_kind != e->kind)
        return 0;
    return e->kind == SHADER_EDIT_OFF || !strcmp(s_saved_slug, e->slug);
}

static int preset_dirty(const struct shader_edit_entry *e)
{
    char key[64], k[64], nm[64];
    float v;
    int has, dirty, d = 0;
    preset_key(e, key, sizeof key);
    for (int i = 0; nv2a_present_chain_override_at(i, k, sizeof k, nm, sizeof nm, &v, &has,
                                                   &dirty); i++)
        d += dirty && !strcmp(k, key);
    return d;
}

int shader_edit_dirty(int i)
{
    const struct shader_edit_entry *e = shader_edit_entry(i);
    if (!e || e->kind == SHADER_EDIT_OFF)
        return 0;
    if (e->kind != SHADER_EDIT_USER)
        return preset_dirty(e) > 0;
    return edited(e) || nv2a_present_chain_profile_dirty(e->slug) > 0;
}

int shader_edit_unsaved(void)
{
    int n = nv2a_present_chain_profile_dirty(NULL);
    if (!s_init)
        return n;
    n += !shader_edit_forced() && !same_choice();
    for (int i = 0; i < s_n; i++)
        n += edited(&s_e[i]);
    return n;
}

static int stage_cfg(int k, const char *key, const char *val)
{
    snprintf(s_ckey[k], sizeof s_ckey[k], "%s", key);
    s_cnull[k] = !val;
    if (val)
        recomp_cfg_quote(val, s_cval[k], sizeof s_cval[k]);
    return k + 1;
}

int shader_edit_config(const char **keys, const char **vals, int max)
{
    const struct shader_edit_entry *e = act();
    int k = 0;
    if (!s_init || shader_edit_forced() || same_choice() || max < 2)
        return 0;
    /* D7: the highest layer that sets either key decides alone. Save
     * writes the user config, so a key it removes there may still be set
     * below, and would decide: "none" goes in instead. */
    switch (e->kind) {
    case SHADER_EDIT_USER:
        k = stage_cfg(k, "present.profile", e->slug);
        k = stage_cfg(k, "present.shader", NULL);
        break;
    case SHADER_EDIT_BUILTIN:
    case SHADER_EDIT_PATH:
        k = stage_cfg(k, "present.shader", e->stage[0]);
        k = stage_cfg(k, "present.profile", lower_sets("present.profile") ? "none" : NULL);
        break;
    default:
        k = stage_cfg(k, "present.profile", lower_sets("present.profile") ? "none" : NULL);
        k = stage_cfg(k, "present.shader", lower_sets("present.shader") ? "none" : NULL);
        break;
    }
    for (int i = 0; i < k; i++) {
        keys[i] = s_ckey[i];
        vals[i] = s_cnull[i] ? NULL : s_cval[i];
    }
    return k;
}

void shader_edit_config_saved(void)
{
    /* The save is the new base: a Delete before it is no longer news. */
    s_deleted = 0;
    if (!s_init || shader_edit_forced())
        return;
    s_saved_kind = act()->kind;
    snprintf(s_saved_slug, sizeof s_saved_slug, "%s", act()->slug);
}

/* Write entry e's file: its stages, name and the store's values, each
 * under its owner's preset key (the file's own key for a name it held). */
static int save_one(struct shader_edit_entry *e, const char *dir, char *msg, size_t n)
{
    char path[PATH_CAP], err[SHADER_PROFILE_ERR_MAX * 2], key0[64];
    char keys[SHADER_PROFILE_STAGES_MAX][64];
    int nv, w;

    start_sync();
    build(e, &s_p);
    s_p.nvalues = 0;
    s_fp.nvalues = 0;
    if (e->file[0]) {
        shader_profile_path(dir, e->file, path, sizeof path);
        if (shader_profile_load(path, &s_fp, err, sizeof err) != SHADER_PROFILE_OK)
            s_fp.nvalues = 0;
    }
    for (int s = 0; s < e->nstages; s++)
        shader_profile_stage_key(e->stage[s], keys[s], sizeof keys[s]);
    snprintf(key0, sizeof key0, "%s", keys[0]);
    if (!e->loaded) {
        /* Never chosen: the store has nothing, the file's values stay. */
        nv = s_fp.nvalues < VALS_MAX ? s_fp.nvalues : VALS_MAX;
        for (int i = 0; i < nv; i++) {
            snprintf(s_vname[i], sizeof s_vname[i], "%s", s_fp.value[i].name);
            s_vv[i] = s_fp.value[i].v;
        }
    } else {
        nv = nv2a_present_chain_profile_values(e->slug, s_vname, s_vv, VALS_MAX);
    }
    for (int i = 0; i < nv; i++) {
        const char *k = key0;
        for (int j = 0; j < s_fp.nvalues; j++)
            if (!strcmp(s_fp.value[j].name, s_vname[i]))
                k = s_fp.value[j].key;
        shader_profile_value_set(&s_p, k, s_vname[i], s_vv[i]);
    }
    shader_profile_place(&s_p);
    /* A value under a stage the chain no longer has goes with it. */
    for (int i = 0; i < s_p.nvalues;) {
        int keep = 0;
        for (int s = 0; s < e->nstages && !keep; s++)
            keep = !strcmp(s_p.value[i].key, keys[s]);
        if (keep)
            i++;
        else
            shader_profile_value_remove(&s_p, s_p.value[i].key, s_p.value[i].name);
    }
    if (e->file[0] && strcmp(e->file, e->slug)) {
        if (shader_profile_rename(dir, e->file, e->slug, err, sizeof err) < 0) {
            say(msg, n, "%s", err);
            return -1;
        }
        snprintf(e->file, sizeof e->file, "%s", e->slug);
    }
    shader_profile_path(dir, e->slug, path, sizeof path);
    w = shader_profile_save(path, &s_p, err, sizeof err);
    if (w < 0) {
        say(msg, n, "%s", err);
        return -1;
    }
    nv2a_present_chain_profile_saved(e->slug);
    snprintf(e->file, sizeof e->file, "%s", e->slug);
    snprintf(e->fname, sizeof e->fname, "%s", e->name);
    e->fnstages = e->nstages;
    memcpy(e->fstage, e->stage, sizeof e->fstage);
    fprintf(stderr, LOGP "menu: saved profile %s to %s\n", e->name, path);
    return 0;
}

int shader_edit_save(char *msg, size_t n)
{
    char dir[PATH_CAP];
    int wrote = 0, have_dir = shader_profile_dir(dir, sizeof dir);

    for (int i = 0; i < s_n; i++) {
        struct shader_edit_entry *e = &s_e[i];
        if (e->kind != SHADER_EDIT_USER || !shader_edit_dirty(i))
            continue;
        if (!have_dir) {
            say(msg, n, "No profiles under RECOMP_CONFIG=none", NULL);
            return -1;
        }
        if (save_one(e, dir, msg, n) < 0)
            return -1;
        wrote++;
    }
    /* A rename moved the composed file's name: the chain goes on with the
     * one it has, and the next request composes under the new slug. */
    return wrote;
}

void shader_edit_reset_defaults(void)
{
    if (shader_edit_forced())
        return;
    /* The page never opened: the start's choice is still to be taken, so
     * Reset can put a start shader off too. */
    if (!s_init)
        shader_edit_refresh();
    s_active = 0;
    request(NULL, 0);
}

int shader_edit_choice_dirty(void)
{
    return s_init && !shader_edit_forced() && !same_choice();
}

int shader_edit_deleted(void)
{
    return s_deleted;
}

int shader_edit_revert(void)
{
    int n = 0, active = 0;

    if (!s_init) {
        nv2a_present_chain_profile_revert(NULL);
        return s_deleted;
    }
    if (!shader_edit_unsaved()) {
        /* Nothing to undo: no new request, so the chain is not rebuilt. */
        return s_deleted;
    }
    for (int i = 0; i < s_n; i++) {
        struct shader_edit_entry *e = &s_e[i];
        if (e->kind == SHADER_EDIT_USER && !e->file[0]) {
            /* Never written: an unsaved New, Copy or fork. */
            nv2a_present_chain_profile_forget(e->slug);
            shader_profile_cache_forget(e->slug);
            continue;
        }
        if (e->kind == SHADER_EDIT_USER && e->state == SHADER_PROFILE_OK) {
            if (strcmp(e->slug, e->file)) {
                /* A rename not yet saved: the values go back to the file's
                 * slug with the name. */
                nv2a_present_chain_profile_rekey(e->slug, e->file);
                shader_profile_cache_renamed(e->slug, e->file);
                snprintf(e->slug, sizeof e->slug, "%s", e->file);
            }
            snprintf(e->name, sizeof e->name, "%s", e->fname);
            e->nstages = e->fnstages;
            memcpy(e->stage, e->fstage, sizeof e->stage);
        }
        if (i != n)
            s_e[n] = *e;
        n++;
    }
    s_n = n;
    nv2a_present_chain_profile_revert(NULL);
    /* The choice the config holds; one a Delete left to a lower layer is
     * unknown here, so Off stays, as the Delete left it. */
    for (int i = 0; i < s_n && s_saved_kind != SAVED_UNKNOWN; i++) {
        const struct shader_edit_entry *e = &s_e[i];
        if (e->kind == s_saved_kind
            && (e->kind == SHADER_EDIT_OFF || !strcmp(e->slug, s_saved_slug)))
            active = i;
    }
    s_active = active;
    shader_edit_refresh();
    request(NULL, 0);
    return s_deleted;
}

void shader_edit_reset_for_test(void)
{
    s_deleted = 0;
    s_n = s_active = s_init = 0;
    s_saved_kind = SHADER_EDIT_OFF;
    s_saved_slug[0] = 0;
    memset(&s_info, 0, sizeof s_info);
    s_info_ok = 0;
    s_info_err[0] = 0;
}
