/*
 * mod_describe.c - the descriptor dump (mod.h; mod-settings D9):
 * RECOMP_MODS_DESCRIBE=json writes every mod's descriptor with the values
 * and bindings this run resolved, for a launcher or the CLI, before the
 * game starts and without running any mod's code.
 *
 * Schema 1. Fields are only added; one removed or changed in meaning bumps
 * "schema", since the readers ship apart from the game. A field that does
 * not apply to a setting or control is left out. The text fields (summary,
 * help, labels) are written as the descriptor has them, glyph tokens
 * ({ctl:move}) included; a reader expands or shows them.
 */
#include "mod.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_menu.h"
#include "nv2a_present_chain.h"
#include "recomp_env.h"
#include "shader_profile.h"

#define LOGP "[MOD] "

/* A JSON string: quotes, backslash and control characters escaped, other
 * bytes as they are (the descriptor's text is UTF-8). NULL is null. */
static void jstr(FILE *f, const char *s)
{
    const unsigned char *p = (const unsigned char *)s;

    if (!s) {
        fputs("null", f);
        return;
    }
    fputc('"', f);
    for (; *p; p++) {
        switch (*p) {
        case '"': fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\b': fputs("\\b", f); break;
        case '\f': fputs("\\f", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20)
                fprintf(f, "\\u%04x", *p);
            else
                fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* A number; JSON has no inf or nan, so those are null. */
static void jnum(FILE *f, double v)
{
    char t[64];
    if (!isfinite(v)) {
        fputs("null", f);
        return;
    }
    xbox_mod_num_text(t, sizeof t, v);
    fputs(t, f);
}

/* A float as the shortest text that reads back as the same float: shader
 * parameters are floats, and their double form (0.009999999776482582)
 * is not what the preset says (0.01). */
static void jfloat(FILE *f, float v)
{
    char t[32];
    if (!isfinite(v)) {
        fputs("null", f);
        return;
    }
    for (int p = 1; p <= 9; p++) {
        snprintf(t, sizeof t, "%.*g", p, (double)v);
        if ((float)strtod(t, NULL) == v)
            break;
    }
    fputs(t, f);
}

/* ,"name": with the comma the field before it needs. */
static void jkey(FILE *f, int *first, const char *name)
{
    fputs(*first ? "" : ", ", f);
    *first = 0;
    jstr(f, name);
    fputs(": ", f);
}

static void jopt_str(FILE *f, int *first, const char *name, const char *v)
{
    if (!v)
        return;
    jkey(f, first, name);
    jstr(f, v);
}

static void jlist(FILE *f, const char *const *v, int n)
{
    fputc('[', f);
    for (int i = 0; v && (n < 0 ? v[i] != NULL : i < n); i++) {
        if (i)
            fputs(", ", f);
        jstr(f, v[i]);
    }
    fputc(']', f);
}

static const char *const s_kind[] = { "bool", "int", "float", "enum" };
static const char *const s_ctl_kind[] = { "action", "button", "stick", "info" };

static const char *live_name(int live)
{
    return live == ENHANCE_LIVE_ACTIVATE ? "activate"
         : live == ENHANCE_LIVE_RESTART ? "restart" : "live";
}

/* A setting's value in its kind's JSON form: a bool true/false, an enum
 * its choice text, a number as a number. */
static void jvalue(FILE *f, const struct xbox_mod_setting *s, double v)
{
    if (s->kind == XBOX_SET_BOOL)
        fputs(v ? "true" : "false", f);
    else if (s->kind == XBOX_SET_ENUM)
        jstr(f, s->choices[(int)v]);
    else
        jnum(f, v);
}

static void setting(FILE *f, int i, uint32_t k, const struct xbox_mod_setting *s)
{
    int first = 1;

    fputs("\n      {", f);
    jkey(f, &first, "name");
    jstr(f, s->name);
    jkey(f, &first, "key");
    jstr(f, xbox_mod_setting_key(i, k));
    jkey(f, &first, "kind");
    jstr(f, s_kind[s->kind]);
    jopt_str(f, &first, "label", s->label);
    jopt_str(f, &first, "help", s->help);
    jopt_str(f, &first, "unit", s->unit);
    if (s->flags & XBOX_SET_ENABLE) {
        jkey(f, &first, "enable");
        fputs("true", f);
    }
    jkey(f, &first, "default");
    jvalue(f, s, s->def);
    if (s->kind == XBOX_SET_INT || s->kind == XBOX_SET_FLOAT) {
        jkey(f, &first, "lo");
        jnum(f, s->lo);
        jkey(f, &first, "hi");
        jnum(f, s->hi);
        if (s->step > 0) {
            jkey(f, &first, "step");
            jnum(f, s->step);
        }
    }
    if (s->kind == XBOX_SET_ENUM) {
        jkey(f, &first, "choices");
        jlist(f, s->choices, -1);
    }
    jkey(f, &first, "live");
    jstr(f, live_name(s->live));
    jopt_str(f, &first, "env", xbox_mod_setting_env(i, k));
    if (s->aliases && s->aliases[0]) {
        jkey(f, &first, "aliases");
        jlist(f, s->aliases, -1);
    }
    jkey(f, &first, "value");
    jvalue(f, s, xbox_mod_setting_value(i, k));
    jkey(f, &first, "source");
    jstr(f, xbox_mod_setting_source(i, k));
    fputc('}', f);
}

static void control(FILE *f, int i, uint32_t k, const struct xbox_mod_control *c)
{
    char b[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    const char *bp[ENHANCE_ACTION_BINDS];
    const char *src;
    int first = 1, n, a = xbox_mod_control_action(i, k);

    fputs("\n      {", f);
    jkey(f, &first, "id");
    jstr(f, c->id);
    jkey(f, &first, "kind");
    jstr(f, s_ctl_kind[c->kind]);
    if (a >= 0) {
        jkey(f, &first, "action");
        jstr(f, enhance_action_name(a));
    }
    jopt_str(f, &first, "label", c->label);
    jopt_str(f, &first, "help", c->help);
    jkey(f, &first, "when");
    jstr(f, c->when);
    if (c->kind != XBOX_CTL_INFO || (c->defaults && c->defaults[0])) {
        jkey(f, &first, "defaults");
        jlist(f, c->defaults, -1);
    }
    if (c->kind != XBOX_CTL_INFO) {
        n = xbox_mod_control_bindings(i, k, b, ENHANCE_ACTION_BINDS, &src);
        for (int j = 0; j < n; j++)
            bp[j] = b[j];
        jkey(f, &first, "bindings");
        jlist(f, bp, n);
        /* The table drops what the rules refuse or another action owns,
         * as a start does; bindings stays what the config says, so the
         * launcher can show the player's own text and mark these. */
        if (a >= 0 && (n = enhance_action_refused(a, b, ENHANCE_ACTION_BINDS)) > 0) {
            for (int j = 0; j < n; j++)
                bp[j] = b[j];
            jkey(f, &first, "refused");
            jlist(f, bp, n);
        }
    }
    jopt_str(f, &first, "env", c->env);
    if (c->aliases && c->aliases[0]) {
        jkey(f, &first, "aliases");
        jlist(f, c->aliases, -1);
    }
    if (c->kind != XBOX_CTL_INFO) {
        jkey(f, &first, "source");
        jstr(f, src);
    }
    fputc('}', f);
}

static void mod(FILE *f, int i)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_of(i);
    int first = 1, e = xbox_mod_enable_setting(i);

    fputs("\n  {", f);
    jkey(f, &first, "id");
    jstr(f, xbox_mod_name(i));
    if (!xbox_mod_has_desc(i)) {
        jkey(f, &first, "descriptor");
        fputs("false", f);
        fputc('}', f);
        return;
    }
    if (d) {
        jopt_str(f, &first, "title", d->title);
        jopt_str(f, &first, "summary", d->summary);
        jopt_str(f, &first, "author", d->author);
        jopt_str(f, &first, "version", d->version_text);
    }
    jkey(f, &first, "descriptor");
    fputs("true", f);
    jkey(f, &first, "init");
    jstr(f, xbox_mod_init_outcome(i));
    if (!strcmp(xbox_mod_init_outcome(i), "failed")) {
        jkey(f, &first, "reason");
        jstr(f, xbox_mod_fail_reason(i));
    }
    if (!d) {
        fputc('}', f);
        return;     /* refused: its tables are not to be trusted */
    }
    if (e >= 0) {
        jkey(f, &first, "enable");
        jstr(f, xbox_mod_setting_key(i, (uint32_t)e));
    }
    jkey(f, &first, "settings");
    fputc('[', f);
    for (uint32_t k = 0; k < d->n_settings; k++) {
        if (k)
            fputc(',', f);
        setting(f, i, k, &d->settings[k]);
    }
    fputc(']', f);
    jkey(f, &first, "controls");
    fputc('[', f);
    for (uint32_t k = 0; k < d->n_controls; k++) {
        if (k)
            fputc(',', f);
        control(f, i, k, &d->controls[k]);
    }
    fputs("]}", f);
}

/* One preset's parameters (present.shader_params, nv2a_present_chain.h):
 * the range, the preset's value, the value this run would use (a config
 * override clamped to the range) and the key that sets it. 0 when the
 * preset cannot be read. */
static int shader_preset(FILE *f, int *any, const char *request)
{
    static struct nv2a_present_param p[NV2A_PRESENT_PARAMS_MAX];
    char key[64], full[192];
    int n = nv2a_present_chain_describe(request, p, NV2A_PRESENT_PARAMS_MAX), first = 1;

    if (n < 0)
        return 0;
    nv2a_present_chain_preset_key(request, key, sizeof key);
    fputs(*any ? ",\n    {" : "\n    {", f);
    *any = 1;
    jkey(f, &first, "name");
    jstr(f, key);
    jkey(f, &first, "params");
    fputc('[', f);
    for (int i = 0; i < n; i++) {
        int pf = 1;
        float v = p[i].initial;
        if (nv2a_present_chain_override_get(key, p[i].name, &v))
            v = v < p[i].min ? p[i].min : v > p[i].max ? p[i].max : v;
        snprintf(full, sizeof full, "present.shader_params.%s.%s", key, p[i].name);
        fputs(i ? ",\n      {" : "\n      {", f);
        jkey(f, &pf, "name");
        jstr(f, p[i].name);
        jkey(f, &pf, "label");
        jstr(f, p[i].desc[0] ? p[i].desc : p[i].name);
        jkey(f, &pf, "min");
        jfloat(f, p[i].min);
        jkey(f, &pf, "max");
        jfloat(f, p[i].max);
        jkey(f, &pf, "step");
        jfloat(f, p[i].step);
        jkey(f, &pf, "default");
        jfloat(f, p[i].initial);
        jkey(f, &pf, "value");
        jfloat(f, v);
        jkey(f, &pf, "key");
        jstr(f, full);
        fputc('}', f);
    }
    fputs("]}", f);
    return 1;
}

static void profile(FILE *f, int *any, const char *name, const char *slug, const char *file,
                    int nstages, const char (*stage)[SHADER_PROFILE_REF_MAX], int builtin,
                    int state)
{
    int first = 1;
    fputs(*any ? ",\n    {" : "\n    {", f);
    *any = 1;
    jkey(f, &first, "name");
    jstr(f, name);
    jkey(f, &first, "slug");
    jstr(f, slug);
    jkey(f, &first, "file");
    jstr(f, file);
    jkey(f, &first, "stages");
    fputc('[', f);
    for (int s = 0; s < nstages; s++) {
        if (s)
            fputs(", ", f);
        jstr(f, stage[s]);
    }
    fputc(']', f);
    jkey(f, &first, "builtin");
    fputs(builtin ? "true" : "false", f);
    jkey(f, &first, "state");
    jstr(f, state == SHADER_PROFILE_OK ? "ok" : state == SHADER_PROFILE_NEWER ? "newer" : "broken");
    fputc('}', f);
}

/* "profiles" (design D5): each bundled preset the library read (have[i])
 * as a one-stage profile, then the profile files in the folder;
 * "active_profile" is the one the start chose ("none" when it chose none). */
static void profiles(FILE *f, const unsigned char *have)
{
    static struct shader_profile_entry l[SHADER_PROFILE_LIST_MAX];
    char slug[SHADER_PROFILE_REF_MAX], dir[1024], file[SHADER_PROFILE_NAME_MAX + 8];
    char one[1][SHADER_PROFILE_REF_MAX];
    int any = 0, n = 0;

    if (!shader_profile_start_slug(slug, sizeof slug))
        snprintf(slug, sizeof slug, "none");
    fputs(", \"active_profile\": ", f);
    jstr(f, slug);
    fputs(", \"profiles\": [", f);
    for (int i = 0; nv2a_present_chain_names[i]; i++) {
        if (i >= 64 || !have[i])
            continue;
        snprintf(one[0], sizeof one[0], "%s", nv2a_present_chain_names[i]);
        profile(f, &any, one[0], one[0], "", 1, (const char (*)[SHADER_PROFILE_REF_MAX])one, 1,
                SHADER_PROFILE_OK);
    }
    if (shader_profile_dir(dir, sizeof dir))
        n = shader_profile_list(dir, l, SHADER_PROFILE_LIST_MAX);
    for (int i = 0; i < n; i++) {
        snprintf(file, sizeof file, "%s.toml", l[i].slug);
        profile(f, &any, l[i].name, l[i].slug, file, l[i].nstages,
                (const char (*)[SHADER_PROFILE_REF_MAX])l[i].stage, 0, l[i].state);
    }
    fputs(any ? "\n  ]" : "]", f);
}

/* "shaders": the request in force and every bundled preset's parameters,
 * plus the requested one when it is a path, then the profiles. Without the
 * library the preset list is empty: a reader shows no Shaders settings. */
static void shaders(FILE *f)
{
    char req[512];
    unsigned char have[64] = { 0 };
    int any = 0, bundled = 0;

    nv2a_present_chain_requested(req, sizeof req);
    fputs(", \"shaders\": {\"active\": ", f);
    jstr(f, req);
    fputs(", \"presets\": [", f);
    for (int i = 0; nv2a_present_chain_names[i]; i++) {
        int got = shader_preset(f, &any, nv2a_present_chain_names[i]);
        if (i < (int)sizeof have)
            have[i] = (unsigned char)got;
        bundled |= !strcmp(req, nv2a_present_chain_names[i]);
    }
    if (!bundled && strcmp(req, "none"))
        shader_preset(f, &any, req);
    fputs(any ? "\n  ]" : "]", f);
    profiles(f, have);
    fputc('}', f);
}

int xbox_mod_describe_wanted(void)
{
    const char *v = recomp_env(RENV_MODS_DESCRIBE);

    if (!v || !*v)
        return 0;
    if (!strcmp(v, "json"))
        return 1;
    fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE='%s' is not json; ignored\n", v);
    return 0;
}

int xbox_mod_describe_write(const char *path)
{
    FILE *f = stdout;
    int i, n = xbox_mod_count(), bad;

    if (path && *path) {
        f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE_OUT=%s: cannot write it (%s)\n", path,
                    strerror(errno));
            return 1;
        }
    }
    fprintf(f, "{\"schema\": 1, \"api\": %u, \"mods\": [", XBOX_MOD_API_VERSION);
    for (i = 0; i < n; i++) {
        if (i)
            fputc(',', f);
        mod(f, i);
    }
    fputs(n ? "\n]" : "]", f);
    shaders(f);
    fputs("}\n", f);
    bad = ferror(f);
    if (f != stdout)
        bad |= fclose(f) != 0;
    else
        bad |= fflush(f) != 0;
    if (bad) {
        fprintf(stderr, LOGP "RECOMP_MODS_DESCRIBE_OUT=%s: the write failed\n",
                path && *path ? path : "(stdout)");
        return 1;
    }
    return 0;
}
