/*
 * shader_param_kinds.c - see shader_param_kinds.h.
 */
#include "shader_param_kinds.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "platform/recomp_exe_dir.h"
#include "recomp_cfg.h"

#define LOGP "[SHADER] "
#define SIDECAR "param-kinds.toml"
#define PROFILE_PREFIX "param_kinds."

static void seterr(char *err, size_t errlen, const char *msg)
{
    if (err && errlen)
        snprintf(err, errlen, "%s", msg);
}

/* src into out (cap bytes), cut at a UTF-8 character boundary: a help line
 * cut inside "·" would draw a box. */
static void utf8_copy(char *out, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (!cap)
        return;
    if (n >= cap) {
        n = cap - 1;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, src, n);
    out[n] = 0;
}

static int kind_of(const char *s)
{
    if (!strcmp(s, "toggle"))
        return SHADER_PARAM_TOGGLE;
    if (!strcmp(s, "choice"))
        return SHADER_PARAM_CHOICE;
    if (!strcmp(s, "slider"))
        return SHADER_PARAM_SLIDER;
    return -1;
}

/* One field of a declaration; 0 (and a line on stderr) when it does not
 * hold what the field takes. */
static int decl_field(struct shader_param_decl *d, const recomp_cfg *c, const char *key,
                      const char *field, const char *src)
{
    recomp_cfg_type t = recomp_cfg_type_of(c, key);
    int line = recomp_cfg_line(c, key);

    if (!strcmp(field, "kind")) {
        int k = t == RCFG_STRING ? kind_of(recomp_cfg_string(c, key, "")) : -1;
        if (k < 0) {
            fprintf(stderr, LOGP "%s:%d: kind is \"toggle\", \"choice\" or \"slider\"\n", src,
                    line);
            return 0;
        }
        d->kind = k;
        return 1;
    }
    if (!strcmp(field, "choices")) {
        size_t n = recomp_cfg_array_len(c, key);
        if (t != RCFG_ARRAY || n < 2 || n > SHADER_PARAM_CHOICES_MAX) {
            fprintf(stderr, LOGP "%s:%d: choices is a list of 2 to %d names\n", src, line,
                    SHADER_PARAM_CHOICES_MAX);
            return 0;
        }
        for (size_t i = 0; i < n; i++) {
            const char *s = recomp_cfg_array_string(c, key, i, NULL);
            if (recomp_cfg_array_type(c, key, i) != RCFG_STRING || !s || !*s
                || strlen(s) >= SHADER_PARAM_CHOICE_LEN) {
                fprintf(stderr, LOGP "%s:%d: choice %zu is not a name of 1 to %d bytes\n", src,
                        line, i + 1, SHADER_PARAM_CHOICE_LEN - 1);
                d->nchoices = 0;
                return 0;
            }
            snprintf(d->choice[i], sizeof d->choice[i], "%s", s);
        }
        d->nchoices = (int)n;
        return 1;
    }
    if (!strcmp(field, "help")) {
        if (t != RCFG_STRING) {
            fprintf(stderr, LOGP "%s:%d: help is a string\n", src, line);
            return 0;
        }
        utf8_copy(d->help, sizeof d->help, recomp_cfg_string(c, key, ""));
        return 1;
    }
    fprintf(stderr, LOGP "%s:%d: unknown key %s\n", src, line, key);
    return 0;
}

static struct shader_param_decl *decl_add(struct shader_param_kinds *t, const char *file,
                                          const char *name, const char *src)
{
    struct shader_param_decl *d;
    for (int i = 0; i < t->n; i++)
        if (!strcmp(t->d[i].file, file) && !strcmp(t->d[i].name, name))
            return &t->d[i];
    if (t->n >= SHADER_PARAM_DECLS_MAX) {
        fprintf(stderr, LOGP "%s: more than %d parameter kinds; %s skipped\n", src,
                SHADER_PARAM_DECLS_MAX, name);
        return NULL;
    }
    d = &t->d[t->n++];
    memset(d, 0, sizeof *d);
    d->kind = -1;
    snprintf(d->file, sizeof d->file, "%s", file);
    snprintf(d->name, sizeof d->name, "%s", name);
    return d;
}

int shader_param_kinds_parse(const char *text, const char *src, struct shader_param_kinds *t,
                             char *err, size_t errlen)
{
    static const char *const k_fields[] = { "kind", "choices", "help", NULL };
    char e[256], key[96];
    recomp_cfg *c;
    size_t n;

    memset(t, 0, sizeof *t);
    if (!src)
        src = SIDECAR;
    c = recomp_cfg_load_string_aot(text ? text : "", src, e, sizeof e);
    if (!c) {
        seterr(err, errlen, e);
        return -1;
    }
    n = recomp_cfg_table_count(c, "param");
    for (size_t i = 0; i < n; i++) {
        struct shader_param_decl *d;
        const char *file, *name;
        snprintf(key, sizeof key, "param.%zu.file", i);
        file = recomp_cfg_string(c, key, "");
        snprintf(key, sizeof key, "param.%zu.name", i);
        name = recomp_cfg_string(c, key, "");
        if (!*file || !*name || strlen(file) >= sizeof d->file || strlen(name) >= sizeof d->name
            || strchr(file, '/') || strchr(file, '\\')) {
            fprintf(stderr, LOGP "%s: [[param]] %zu needs file (a shader's file name) and "
                    "name; skipped\n", src, i + 1);
            continue;
        }
        d = decl_add(t, file, name, src);
        if (!d)
            break;
        for (int f = 0; k_fields[f]; f++) {
            snprintf(key, sizeof key, "param.%zu.%s", i, k_fields[f]);
            if (recomp_cfg_type_of(c, key) != RCFG_NONE)
                decl_field(d, c, key, k_fields[f], src);
        }
    }
    /* A key the loop did not read: a typo the author should hear of. */
    for (size_t i = 0; recomp_cfg_key_at(c, i); i++) {
        const char *k = recomp_cfg_key_at(c, i), *dot;
        size_t idx;
        char *end;
        if (strncmp(k, "param.", 6)) {
            fprintf(stderr, LOGP "%s:%d: unknown key %s\n", src, recomp_cfg_line(c, k), k);
            continue;
        }
        idx = (size_t)strtoul(k + 6, &end, 10);
        dot = end;
        if (idx < n && *dot == '.'
            && (!strcmp(dot + 1, "file") || !strcmp(dot + 1, "name") || !strcmp(dot + 1, "kind")
                || !strcmp(dot + 1, "choices") || !strcmp(dot + 1, "help")))
            continue;
        fprintf(stderr, LOGP "%s:%d: unknown key %s\n", src, recomp_cfg_line(c, k), k);
    }
    recomp_cfg_free(c);
    return 0;
}

int shader_param_kinds_profile_key(const char *key)
{
    return key && !strncmp(key, PROFILE_PREFIX, sizeof PROFILE_PREFIX - 1);
}

int shader_param_kinds_parse_profile(const char *text, const char *src,
                                     struct shader_param_kinds *t, char *err, size_t errlen)
{
    char e[256], name[64];
    recomp_cfg *c;
    const char *k;

    memset(t, 0, sizeof *t);
    if (!src)
        src = "profile";
    c = recomp_cfg_load_string(text ? text : "", src, e, sizeof e);
    if (!c) {
        seterr(err, errlen, e);
        return -1;
    }
    for (size_t i = 0; (k = recomp_cfg_key_at(c, i)); i++) {
        const char *rest, *dot;
        struct shader_param_decl *d;
        if (!shader_param_kinds_profile_key(k))
            continue;
        rest = k + sizeof PROFILE_PREFIX - 1;
        dot = strchr(rest, '.');
        if (!dot || dot == rest || (size_t)(dot - rest) >= sizeof name || strchr(dot + 1, '.')) {
            fprintf(stderr, LOGP "%s:%d: %s is not param_kinds.<param>.<kind|choices|help>;"
                    " skipped\n", src, recomp_cfg_line(c, k), k);
            continue;
        }
        memcpy(name, rest, (size_t)(dot - rest));
        name[dot - rest] = 0;
        d = decl_add(t, "", name, src);
        if (d)
            decl_field(d, c, k, dot + 1, src);
    }
    recomp_cfg_free(c);
    return 0;
}

const struct shader_param_decl *shader_param_kinds_find(const struct shader_param_kinds *t,
                                                        const char *file, const char *name)
{
    if (!t || !name)
        return NULL;
    for (int i = 0; i < t->n; i++) {
        const struct shader_param_decl *d = &t->d[i];
        if (strcmp(d->name, name))
            continue;
        if (!file || !*file || !d->file[0] || !strcmp(d->file, file))
            return d;
    }
    return NULL;
}

/* ---- the sidecar beside the executable ---------------------------------- */

static struct shader_param_kinds s_builtin;
static int s_builtin_read;
static char s_builtin_path[1024];

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || n > (1 << 20) || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    if (buf)
        buf[n] = 0;
    fclose(f);
    return buf;
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0;
}

void shader_param_kinds_builtin_reset(const char *path)
{
    snprintf(s_builtin_path, sizeof s_builtin_path, "%s", path ? path : "");
    s_builtin_read = 0;
    memset(&s_builtin, 0, sizeof s_builtin);
}

const struct shader_param_kinds *shader_param_kinds_builtin(void)
{
    char path[1024], err[256];
    char *text;

    if (s_builtin_read)
        return &s_builtin;
    s_builtin_read = 1;
    if (s_builtin_path[0]) {
        snprintf(path, sizeof path, "%s", s_builtin_path);
    } else {
        snprintf(path, sizeof path, "%s/shaders/" SIDECAR, recomp_exe_dir());
#ifdef __APPLE__
        if (!file_exists(path))
            snprintf(path, sizeof path, "%s/../Resources/shaders/" SIDECAR, recomp_exe_dir());
#endif
    }
    /* No sidecar is no error: the inferred rules still run. */
    if (!file_exists(path) || !(text = read_text(path)))
        return &s_builtin;
    if (shader_param_kinds_parse(text, path, &s_builtin, err, sizeof err) < 0)
        fprintf(stderr, LOGP "%s\n", err);
    free(text);
    return &s_builtin;
}

/* The Shaders page builds its rows often; the profile file is read again
 * only when it is another file or changed on disk (a hand edit shows the
 * next time the rows are built). */
static struct shader_param_kinds s_prof;
static char s_prof_path[1024];
static long long s_prof_mtime = -1, s_prof_nsec = -1, s_prof_size = -1;

/* The sub-second part of st's time where the host keeps one: a save and a
 * hand edit inside one second, of the same size, still read again. */
static long long stat_nsec(const struct stat *st)
{
#if defined(__APPLE__)
    return (long long)st->st_mtimespec.tv_nsec;
#elif defined(__linux__) || defined(__unix__)
    return (long long)st->st_mtim.tv_nsec;
#else
    (void)st;
    return 0;
#endif
}

const struct shader_param_kinds *shader_param_kinds_profile_file(const char *path)
{
    struct stat st;
    char err[256];
    char *text;

    if (!path || !*path || stat(path, &st)) {
        s_prof_path[0] = 0;
        return NULL;
    }
    if (!strcmp(path, s_prof_path) && (long long)st.st_mtime == s_prof_mtime
        && stat_nsec(&st) == s_prof_nsec && (long long)st.st_size == s_prof_size)
        return &s_prof;
    snprintf(s_prof_path, sizeof s_prof_path, "%s", path);
    s_prof_mtime = (long long)st.st_mtime;
    s_prof_nsec = stat_nsec(&st);
    s_prof_size = (long long)st.st_size;
    memset(&s_prof, 0, sizeof s_prof);
    if ((text = read_text(path))) {
        /* A profile that does not parse is the profile loader's to report. */
        shader_param_kinds_parse_profile(text, path, &s_prof, err, sizeof err);
        free(text);
    }
    return &s_prof;
}

/* ---- the rules ----------------------------------------------------------- */

static int is_int(float x)
{
    return fabsf(x - roundf(x)) < 1e-4f;
}

/* The number of values an integer range of step 1 (or no step) holds; 0
 * when the range is not one. */
static int int_count(const struct nv2a_present_param *p)
{
    if (!is_int(p->min) || !is_int(p->max) || p->max < p->min
        || !(p->step == 0 || fabsf(p->step - 1.0f) < 1e-4f))
        return 0;
    /* No rule wants more than a few values; a wide range (0..1e9) must not
     * overflow the count. */
    if (p->max - p->min > 64.0f)
        return 0;
    return (int)roundf(p->max - p->min) + 1;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t';
}

/* desc's trailing ": a | b ..." into names (at most cap), its label part
 * into label. Returns the name count, 0 when desc has no such list (and
 * label is desc whole). */
static int desc_names(const char *desc, char *label, size_t lcap,
                      char (*names)[SHADER_PARAM_CHOICE_LEN], int cap)
{
    const char *colon = strrchr(desc, ':'), *s, *e;
    size_t ln;
    int n = 0;

    utf8_copy(label, lcap, desc);
    if (!colon || !strchr(colon, '|'))
        return 0;
    for (s = colon + 1;; s = e + 1) {
        const char *b = s, *t;
        e = strchr(s, '|');
        if (!e)
            e = s + strlen(s);
        while (b < e && is_space(*b))
            b++;
        t = e;
        while (t > b && is_space(t[-1]))
            t--;
        if (t == b || (size_t)(t - b) >= SHADER_PARAM_CHOICE_LEN || n >= cap)
            return 0;
        snprintf(names[n++], SHADER_PARAM_CHOICE_LEN, "%.*s", (int)(t - b), b);
        if (!*e)
            break;
    }
    ln = (size_t)(colon - desc);
    while (ln && is_space(desc[ln - 1]))
        ln--;
    if (!ln)
        return 0;
    if (ln >= lcap)
        ln = lcap - 1;
    /* desc fits label (both hold a preset's 95-byte description), so
     * the cut lands where the list began. */
    memcpy(label, desc, ln);
    label[ln] = 0;
    return n;
}

/* d declares a kind p's range can take. */
static int decl_fits(const struct shader_param_decl *d, const struct nv2a_present_param *p,
                     int count)
{
    if (!d || d->kind < 0)
        return 0;
    if (d->kind == SHADER_PARAM_TOGGLE)
        return p->max > p->min;
    if (d->kind == SHADER_PARAM_CHOICE)
        return count >= 2 && d->nchoices == count;
    return 1;
}

void shader_param_kind_resolve(const struct nv2a_present_param *p,
                               const struct shader_param_kinds *profile,
                               const struct shader_param_kinds *sidecar,
                               struct shader_param_kind_info *out)
{
    char dlabel[96], dnames[SHADER_PARAM_CHOICES_MAX][SHADER_PARAM_CHOICE_LEN];
    const struct shader_param_decl *pd = shader_param_kinds_find(profile, NULL, p->name);
    const struct shader_param_decl *sd = p->file[0]
                                             ? shader_param_kinds_find(sidecar, p->file, p->name)
                                             : NULL;
    const struct shader_param_decl *d;
    int count = int_count(p);
    int nd = desc_names(p->desc[0] ? p->desc : p->name, dlabel, sizeof dlabel, dnames,
                        SHADER_PARAM_CHOICES_MAX);

    memset(out, 0, sizeof *out);
    out->kind = SHADER_PARAM_SLIDER;
    snprintf(out->label, sizeof out->label, "%s", p->desc[0] ? p->desc : p->name);
    /* The profile's help wins, as its kind does; either one's help counts
     * even when it declares no kind. */
    if (pd && pd->help[0])
        snprintf(out->help, sizeof out->help, "%s", pd->help);
    else if (sd && sd->help[0])
        snprintf(out->help, sizeof out->help, "%s", sd->help);

    /* 1. Declared: the profile's, then the sidecar's when the profile's
     * cannot fit the range, so a profile's mistake leaves the shader's
     * own declaration in force. A choice needs one name per value. */
    d = decl_fits(pd, p, count) ? pd : decl_fits(sd, p, count) ? sd : NULL;
    if (d && d->kind == SHADER_PARAM_SLIDER)
        return;
    if (d && d->kind == SHADER_PARAM_TOGGLE) {
        out->kind = SHADER_PARAM_TOGGLE;
        if (d->nchoices == 2) {
            out->nnames = 2;
            memcpy(out->name, d->choice, sizeof out->name);
        }
    } else if (d && d->kind == SHADER_PARAM_CHOICE) {
        out->kind = SHADER_PARAM_CHOICE;
        out->nnames = count;
        memcpy(out->name, d->choice, sizeof out->name);
    } else if (count == 2 && p->min == 0.0f && p->max == 1.0f && p->step > 0) {
        /* 2. Inferred toggle: 0..1 by 1. Before 3, so 3 never sees one. */
        out->kind = SHADER_PARAM_TOGGLE;
    } else if (count >= 3 && count <= SHADER_PARAM_CHOICES_MAX && p->step > 0 && nd == count) {
        /* 3. Inferred choice: the label names every value. */
        out->kind = SHADER_PARAM_CHOICE;
        out->nnames = nd;
        memcpy(out->name, dnames, sizeof out->name);
    } else {
        return;   /* 4. A slider. */
    }
    /* A toggle or a choice drops its names list from the label; a
     * toggle's two names are its off and on words. */
    if (nd) {
        snprintf(out->label, sizeof out->label, "%s", dlabel);
        if (out->kind == SHADER_PARAM_TOGGLE && !out->nnames && nd == 2) {
            out->nnames = 2;
            memcpy(out->name, dnames, sizeof out->name);
        }
    }
}

int shader_param_toggle_on(const struct nv2a_present_param *p, float v)
{
    return v >= (p->min + p->max) * 0.5f;
}

float shader_param_toggle_value(const struct nv2a_present_param *p, int on)
{
    return on ? p->max : p->min;
}

int shader_param_kind_index(const struct nv2a_present_param *p, float v)
{
    int count = int_count(p), i;
    if (count < 2)
        return 0;
    i = (int)roundf(v - p->min);
    return i < 0 ? 0 : i >= count ? count - 1 : i;
}

float shader_param_kind_value(const struct nv2a_present_param *p, int index)
{
    int count = int_count(p);
    if (count < 2)
        return p->min;
    if (index < 0)
        index = 0;
    if (index >= count)
        index = count - 1;
    return p->min + (float)index;
}

float shader_param_choice_next(const struct nv2a_present_param *p, int n, float v, int dir)
{
    int k;
    if (n < 1)
        return v;
    k = shader_param_kind_index(p, v) + (dir < 0 ? -1 : 1);
    return shader_param_kind_value(p, (k % n + n) % n);
}
