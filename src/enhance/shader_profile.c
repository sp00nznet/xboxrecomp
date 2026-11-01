/*
 * shader_profile.c - see shader_profile.h.
 */
#include "shader_profile.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <io.h>
#include <windows.h>
#else
#include <dirent.h>
#include <unistd.h>
#endif

#include "enhance_cfg.h"
#include "nv2a_present_chain.h"
#include "platform/recomp_exe_dir.h"
#include "recomp_cfg.h"

#define LOGP "[SHADER] "
#define PATH_CAP 1024
#define REF_DEPTH_MAX 16

#ifdef _WIN32
#define HOST_WIN 1
#else
#define HOST_WIN 0
#endif

static void seterr(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    if (!err || !errlen)
        return;
    va_start(ap, fmt);
    vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
}

static int file_exists(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && !(st.st_mode & S_IFDIR);
}

static int path_is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\'
        || (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

static const char *base_of(const char *p)
{
    const char *b = p;
    for (; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

/* path's directory into out ("." when it names none). */
static void dir_of(const char *path, char *out, size_t cap)
{
    size_t n = (size_t)(base_of(path) - path);
    if (n == 0) {
        snprintf(out, cap, ".");
        return;
    }
    if (n > 1)
        n--;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

/* The file stem of path (base name without one extension ext). */
static void stem_of(const char *path, const char *ext, char *out, size_t cap)
{
    const char *b = base_of(path);
    size_t n = strlen(b), e = strlen(ext);
    if (n > e && !strcmp(b + n - e, ext))
        n -= e;
    if (n >= cap)
        n = cap - 1;
    memcpy(out, b, n);
    out[n] = 0;
}

static void mkdir_parent(const char *path)
{
    char dir[PATH_CAP];
    size_t n;
    snprintf(dir, sizeof dir, "%s", path);
    n = strlen(dir);
    for (size_t i = 1; i < n; i++) {
        int sep = dir[i] == '/';
#ifdef _WIN32
        sep = sep || dir[i] == '\\';
        if (i == 2 && dir[1] == ':')
            continue;
#endif
        if (!sep || dir[i - 1] == '/' || dir[i - 1] == '\\')
            continue;
        dir[i] = 0;
#ifdef _WIN32
        _mkdir(dir);
#else
        mkdir(dir, 0755);
#endif
        dir[i] = path[i];
    }
}

static int replace_file(const char *tmp, const char *path)
{
#ifdef _WIN32
    return MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, path);
#endif
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *buf;
    long n;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET)) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf)
        buf[n] = 0;
    return buf;
}

/* ---- names and slugs ---------------------------------------------------- */

int shader_profile_slug(const char *name, char *out, size_t cap)
{
    size_t o = 0;
    int dash = 0;
    if (!cap)
        return 0;
    for (const unsigned char *s = (const unsigned char *)(name ? name : ""); *s; s++) {
        unsigned char c = *s;
        int keep = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char)(c - 'A' + 'a');
            keep = 1;
        }
        if (!keep) {
            dash = o > 0;
            continue;
        }
        if (dash && c != '-' && o + 1 < cap && o < SHADER_PROFILE_NAME_MAX)
            out[o++] = '-';
        dash = 0;
        if (c == '-' && o && out[o - 1] == '-')
            continue;
        if (o + 1 >= cap || o >= SHADER_PROFILE_NAME_MAX)
            break;
        out[o++] = (char)c;
    }
    while (o && out[o - 1] == '-')
        o--;
    out[o] = 0;
    /* A leading '-' would read as a flag on a command line. */
    if (out[0] == '-') {
        memmove(out, out + 1, o);
        o--;
    }
    return o > 0;
}

int shader_profile_slug_ok(const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (!n || n > SHADER_PROFILE_NAME_MAX || s[0] == '-' || s[n - 1] == '-')
        return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_'))
            return 0;
    }
    return 1;
}

static int utf8_ok(const unsigned char *s)
{
    while (*s) {
        int k = *s < 0x80 ? 0 : (*s & 0xE0) == 0xC0 ? 1 : (*s & 0xF0) == 0xE0 ? 2
              : (*s & 0xF8) == 0xF0 ? 3 : -1;
        if (k < 0 || (k == 1 && *s < 0xC2))
            return 0;
        s++;
        for (int i = 0; i < k; i++, s++)
            if ((*s & 0xC0) != 0x80)
                return 0;
    }
    return 1;
}

int shader_profile_name_ok(const char *name)
{
    char slug[SHADER_PROFILE_NAME_MAX + 1];
    size_t n = name ? strlen(name) : 0;
    if (!n || n > SHADER_PROFILE_NAME_MAX || !utf8_ok((const unsigned char *)name))
        return 0;
    for (size_t i = 0; i < n; i++)
        if ((unsigned char)name[i] < 0x20 || name[i] == 0x7f)
            return 0;
    return shader_profile_slug(name, slug, sizeof slug);
}

static int taken_has(const char *const *taken, int n, const char *s)
{
    for (int i = 0; i < n; i++)
        if (taken[i] && !strcmp(taken[i], s))
            return 1;
    return 0;
}

void shader_profile_slug_free(const char *name, const char *const *taken, int ntaken,
                              char *out, size_t cap)
{
    char base[SHADER_PROFILE_NAME_MAX + 1];
    if (!shader_profile_slug(name, base, sizeof base))
        snprintf(base, sizeof base, "profile");
    if (!taken_has(taken, ntaken, base)) {
        snprintf(out, cap, "%s", base);
        return;
    }
    for (int k = 2;; k++) {
        char suf[16];
        int sl = snprintf(suf, sizeof suf, "-%d", k);
        size_t bl = strlen(base);
        if (bl + (size_t)sl > SHADER_PROFILE_NAME_MAX)
            bl = SHADER_PROFILE_NAME_MAX - (size_t)sl;
        snprintf(out, cap, "%.*s%s", (int)bl, base, suf);
        if (!taken_has(taken, ntaken, out))
            return;
    }
}

/* ---- values ------------------------------------------------------------- */

static int value_find(const struct shader_profile *p, const char *key, const char *name)
{
    for (int i = 0; i < p->nvalues; i++)
        if (!strcmp(p->value[i].key, key) && !strcmp(p->value[i].name, name))
            return i;
    return -1;
}

int shader_profile_value_get(const struct shader_profile *p, const char *key,
                             const char *name, float *v)
{
    int i = value_find(p, key, name);
    if (i < 0)
        return 0;
    if (v)
        *v = p->value[i].v;
    return 1;
}

int shader_profile_value_set(struct shader_profile *p, const char *key, const char *name,
                             float v)
{
    int i = value_find(p, key, name);
    if (i < 0) {
        if (p->nvalues >= SHADER_PROFILE_VALUES_MAX || strlen(key) >= sizeof p->value[0].key
            || strlen(name) >= sizeof p->value[0].name)
            return 0;
        i = p->nvalues++;
        snprintf(p->value[i].key, sizeof p->value[i].key, "%s", key);
        snprintf(p->value[i].name, sizeof p->value[i].name, "%s", name);
    }
    p->value[i].v = v;
    return 1;
}

void shader_profile_value_remove(struct shader_profile *p, const char *key, const char *name)
{
    int o = 0;
    for (int i = 0; i < p->nvalues; i++) {
        if (!strcmp(p->value[i].key, key) && (!name || !strcmp(p->value[i].name, name)))
            continue;
        p->value[o++] = p->value[i];
    }
    p->nvalues = o;
}

static int declares(const struct shader_profile_stage_params *sp, const char *name)
{
    for (int i = 0; i < sp->n; i++)
        if (sp->name[i] && !strcmp(sp->name[i], name))
            return 1;
    return 0;
}

int shader_profile_owner(const struct shader_profile *p,
                         const struct shader_profile_stage_params *sp, const char *name)
{
    for (int s = 0; s < p->nstages; s++)
        if (declares(&sp[s], name))
            return s;
    return -1;
}

void shader_profile_rehome(struct shader_profile *p, const struct shader_profile_stage_params *sp,
                           const char *file)
{
    char keys[SHADER_PROFILE_STAGES_MAX][64];
    for (int s = 0; s < p->nstages; s++)
        shader_profile_stage_key(p->stage[s], keys[s], sizeof keys[s]);
    for (int i = 0; i < p->nvalues; i++) {
        char name[64];
        int owner = shader_profile_owner(p, sp, p->value[i].name), from = -1;
        float v = 0;
        if (owner < 0)
            continue;
        snprintf(name, sizeof name, "%s", p->value[i].name);
        /* The owner's table first, else the other stages' in stage order. */
        if (shader_profile_value_get(p, keys[owner], name, &v))
            from = owner;
        for (int s = 0; from < 0 && s < p->nstages; s++)
            if (shader_profile_value_get(p, keys[s], name, &v))
                from = s;
        if (from < 0)
            continue;
        for (int s = 0; s < p->nstages; s++) {
            float w;
            if (s == from || strcmp(keys[s], keys[from]) == 0
                || !shader_profile_value_get(p, keys[s], name, &w))
                continue;
            if (w != v)
                fprintf(stderr, LOGP "%s: %s is %g under [params.%s] and %g under"
                        " [params.%s]; one value per name, %g kept\n", file ? file : "profile",
                        name, (double)v, keys[from], (double)w, keys[s], (double)v);
            shader_profile_value_remove(p, keys[s], name);
        }
        if (strcmp(keys[from], keys[owner])) {
            shader_profile_value_remove(p, keys[from], name);
            shader_profile_value_set(p, keys[owner], name, v);
        }
        /* The removals moved rows; this row's name is settled either way,
         * so start over until a pass changes nothing. */
        if (value_find(p, keys[owner], name) != i) {
            i = -1;
            continue;
        }
    }
}

/* ---- the file ----------------------------------------------------------- */

#define PARAMS_PREFIX "params."

/* "params.<key>.<name>": key and name split, 0 when it is not that shape. */
static int params_key(const char *k, char *key, size_t kcap, const char **name)
{
    const char *rest, *dot;
    if (strncmp(k, PARAMS_PREFIX, sizeof PARAMS_PREFIX - 1))
        return 0;
    rest = k + sizeof PARAMS_PREFIX - 1;
    dot = strchr(rest, '.');
    if (!dot || dot == rest || !dot[1] || strchr(dot + 1, '.') || (size_t)(dot - rest) >= kcap
        || strlen(dot + 1) >= 64)
        return 0;
    memcpy(key, rest, (size_t)(dot - rest));
    key[dot - rest] = 0;
    *name = dot + 1;
    return 1;
}

/* Cut s to at most max bytes at a UTF-8 character boundary. */
/* src cut to at most max bytes at a character boundary, into out (at least
 * max + 1 bytes): byte max of a longer src says whether a character spans
 * the cut. */
static void utf8_cut(char *out, const char *src, size_t max)
{
    size_t n = strlen(src);
    if (n > max) {
        n = max;
        while (n && ((unsigned char)src[n] & 0xC0) == 0x80)
            n--;
    }
    memcpy(out, src, n);
    out[n] = 0;
}

int shader_profile_parse(const char *text, const char *file, const char *slug,
                         struct shader_profile *p, char *err, size_t errlen)
{
    char e[256];
    recomp_cfg *c;
    long long ver;
    size_t ns;
    int newer;
    const char *k;

    memset(p, 0, sizeof *p);
    if (!file)
        file = "profile";
    c = recomp_cfg_load_string(text ? text : "", file, e, sizeof e);
    if (!c) {
        seterr(err, errlen, "%s", e);
        return SHADER_PROFILE_BROKEN;
    }
    if (recomp_cfg_type_of(c, "version") == RCFG_NONE) {
        seterr(err, errlen, "%s: no version (version = %d)", file, SHADER_PROFILE_VERSION);
        recomp_cfg_free(c);
        return SHADER_PROFILE_BROKEN;
    }
    if (recomp_cfg_type_of(c, "version") != RCFG_INT
        || (ver = recomp_cfg_int(c, "version", 0)) < 1) {
        seterr(err, errlen, "%s:%d: version is not a positive integer", file,
               recomp_cfg_line(c, "version"));
        recomp_cfg_free(c);
        return SHADER_PROFILE_BROKEN;
    }
    p->version = ver > 1000 ? 1000 : (int)ver;
    newer = ver > SHADER_PROFILE_VERSION;

    if (recomp_cfg_type_of(c, "name") != RCFG_NONE) {
        if (recomp_cfg_type_of(c, "name") != RCFG_STRING && !newer) {
            seterr(err, errlen, "%s:%d: name is not a string", file, recomp_cfg_line(c, "name"));
            recomp_cfg_free(c);
            return SHADER_PROFILE_BROKEN;
        }
        snprintf(p->name, sizeof p->name, "%s", recomp_cfg_string(c, "name", ""));
        if (strlen(recomp_cfg_string(c, "name", "")) > SHADER_PROFILE_NAME_MAX) {
            utf8_cut(p->name, recomp_cfg_string(c, "name", ""), SHADER_PROFILE_NAME_MAX);
            fprintf(stderr, LOGP "%s:%d: name is longer than %d bytes; cut to \"%s\"\n", file,
                    recomp_cfg_line(c, "name"), SHADER_PROFILE_NAME_MAX, p->name);
        }
    }
    if (slug && *slug)
        snprintf(p->slug, sizeof p->slug, "%s", slug);
    else if (!shader_profile_slug(p->name, p->slug, sizeof p->slug))
        snprintf(p->slug, sizeof p->slug, "profile");
    if (!p->name[0])
        snprintf(p->name, sizeof p->name, "%s", p->slug);

    ns = recomp_cfg_array_len(c, "stages");
    if (recomp_cfg_type_of(c, "stages") != RCFG_ARRAY || !ns) {
        recomp_cfg_free(c);
        if (newer)
            return SHADER_PROFILE_NEWER;
        seterr(err, errlen, "%s: stages is missing or empty (stages = [\"fsr\", ...])", file);
        return SHADER_PROFILE_BROKEN;
    }
    if (ns > SHADER_PROFILE_STAGES_MAX) {
        fprintf(stderr, LOGP "%s:%d: %zu stages; the limit is %d, the first %d taken\n", file,
                recomp_cfg_line(c, "stages"), ns, SHADER_PROFILE_STAGES_MAX,
                SHADER_PROFILE_STAGES_MAX);
        ns = SHADER_PROFILE_STAGES_MAX;
    }
    for (size_t i = 0; i < ns; i++) {
        const char *s = recomp_cfg_array_string(c, "stages", i, NULL);
        if (recomp_cfg_array_type(c, "stages", i) != RCFG_STRING || !s || !*s
            || strlen(s) >= SHADER_PROFILE_REF_MAX) {
            if (newer)
                break;
            seterr(err, errlen, "%s:%d: stage %zu is not a preset name or path", file,
                   recomp_cfg_line(c, "stages"), i + 1);
            recomp_cfg_free(c);
            return SHADER_PROFILE_BROKEN;
        }
        snprintf(p->stage[p->nstages++], SHADER_PROFILE_REF_MAX, "%s", s);
    }

    for (size_t i = 0; (k = recomp_cfg_key_at(c, i)); i++) {
        char key[64];
        const char *name;
        recomp_cfg_type t;
        if (!strcmp(k, "version") || !strcmp(k, "name") || !strcmp(k, "stages"))
            continue;
        if (!strncmp(k, PARAMS_PREFIX, sizeof PARAMS_PREFIX - 1)) {
            t = recomp_cfg_type_of(c, k);
            if (!params_key(k, key, sizeof key, &name) || (t != RCFG_FLOAT && t != RCFG_INT)) {
                fprintf(stderr, LOGP "%s:%d: %s is not params.<preset>.<param> = <number>;"
                        " skipped\n", file, recomp_cfg_line(c, k), k);
                continue;
            }
            if (!shader_profile_value_set(p, key, name, (float)recomp_cfg_float(c, k, 0)))
                fprintf(stderr, LOGP "%s:%d: more than %d values; %s skipped\n", file,
                        recomp_cfg_line(c, k), SHADER_PROFILE_VALUES_MAX, k);
            continue;
        }
        /* [param_kinds] belongs to the Shaders page, which reads the file
         * again (shader_param_kinds_parse_profile); Save keeps it like any
         * key it does not own. */
        if (!strncmp(k, "param_kinds.", 12))
            continue;
        /* Kept: Save patches only the keys it owns. */
        fprintf(stderr, LOGP "%s:%d: unknown key %s\n", file, recomp_cfg_line(c, k), k);
    }
    recomp_cfg_free(c);
    return newer ? SHADER_PROFILE_NEWER : SHADER_PROFILE_OK;
}

int shader_profile_load(const char *path, struct shader_profile *p, char *err, size_t errlen)
{
    char slug[SHADER_PROFILE_NAME_MAX + 8], *text;
    int r;
    stem_of(path, ".toml", slug, sizeof slug);
    text = read_text(path);
    if (!text) {
        memset(p, 0, sizeof *p);
        seterr(err, errlen, "%s: cannot read", path);
        return SHADER_PROFILE_BROKEN;
    }
    r = shader_profile_parse(text, path, slug, p, err, errlen);
    free(text);
    return r;
}

static const char k_header[] =
    "# xboxrecomp shader profile (docs/runtime/config.md, \"Shader profiles\")\n";

int shader_profile_save(const char *path, const struct shader_profile *p, char *err,
                        size_t errlen)
{
    recomp_cfg *old = NULL;
    const char **keys = NULL, **vals = NULL;
    char (*kbuf)[160] = NULL, (*vbuf)[32] = NULL;
    char name_q[SHADER_PROFILE_NAME_MAX * 6 + 8], *stages_q = NULL, e[512];
    size_t n = 0, cap, nold = 0, sq;
    int rc = -1;

    if (p->nstages < 1 || p->nstages > SHADER_PROFILE_STAGES_MAX) {
        seterr(err, errlen, "%s: %d stages (1 to %d)", path, p->nstages,
               SHADER_PROFILE_STAGES_MAX);
        return -1;
    }
    if (file_exists(path)) {
        old = recomp_cfg_load_file(path, e, sizeof e);
        if (!old) {
            seterr(err, errlen, "%s does not parse; not written (%s)", path, e);
            return -1;
        }
        if (recomp_cfg_int(old, "version", SHADER_PROFILE_VERSION) > SHADER_PROFILE_VERSION) {
            seterr(err, errlen, "%s is of a newer format; not written", path);
            recomp_cfg_free(old);
            return -1;
        }
        nold = recomp_cfg_count(old);
    } else {
        FILE *f;
        mkdir_parent(path);
        f = fopen(path, "wb");
        if (!f || fputs(k_header, f) < 0) {
            seterr(err, errlen, "%s: cannot write", path);
            if (f)
                fclose(f);
            return -1;
        }
        fclose(f);
    }

    cap = 3 + (size_t)p->nvalues + nold;
    keys = calloc(cap, sizeof *keys);
    vals = calloc(cap, sizeof *vals);
    kbuf = calloc(cap, sizeof *kbuf);
    vbuf = calloc(cap, sizeof *vbuf);
    sq = (size_t)p->nstages * (SHADER_PROFILE_REF_MAX * 2 + 8) + 8;
    stages_q = malloc(sq);
    if (!keys || !vals || !kbuf || !vbuf || !stages_q) {
        seterr(err, errlen, "out of memory");
        goto out;
    }

    keys[n] = "version";
    vals[n++] = "1";
    if (!recomp_cfg_quote(p->name[0] ? p->name : p->slug, name_q, sizeof name_q)) {
        seterr(err, errlen, "%s: name does not fit", path);
        goto out;
    }
    keys[n] = "name";
    vals[n++] = name_q;
    {
        size_t o = 0;
        stages_q[o++] = '[';
        for (int s = 0; s < p->nstages; s++) {
            if (s) {
                stages_q[o++] = ',';
                stages_q[o++] = ' ';
            }
            if (!recomp_cfg_quote(p->stage[s], stages_q + o, sq - o - 2)) {
                seterr(err, errlen, "%s: stage %d does not fit", path, s + 1);
                goto out;
            }
            o += strlen(stages_q + o);
        }
        stages_q[o++] = ']';
        stages_q[o] = 0;
    }
    keys[n] = "stages";
    vals[n++] = stages_q;
    for (int i = 0; i < p->nvalues; i++) {
        snprintf(kbuf[n], sizeof kbuf[n], PARAMS_PREFIX "%s.%s", p->value[i].key,
                 p->value[i].name);
        snprintf(vbuf[n], sizeof vbuf[n], "%.6g", (double)p->value[i].v);
        keys[n] = kbuf[n];
        vals[n] = vbuf[n];
        n++;
    }
    /* Differences only: a value the profile no longer holds leaves the file. */
    for (size_t i = 0; i < nold; i++) {
        const char *k = recomp_cfg_key_at(old, i), *name;
        char key[64];
        if (!params_key(k, key, sizeof key, &name) || value_find(p, key, name) >= 0)
            continue;
        snprintf(kbuf[n], sizeof kbuf[n], "%s", k);
        keys[n] = kbuf[n];
        vals[n] = NULL;
        n++;
    }
    rc = recomp_cfg_patch(path, keys, vals, n, err, errlen);
    if (rc > 0)
        rc = -1;   /* a key the file holds as a [table]: the file needs a hand fix */
out:
    recomp_cfg_free(old);
    free(keys);
    free(vals);
    free(kbuf);
    free(vbuf);
    free(stages_q);
    return rc;
}

int shader_profile_dir(char *out, size_t cap)
{
    char dir[PATH_CAP];
    if (!enhance_cfg_user_dir(dir, sizeof dir))
        return 0;
    snprintf(out, cap, "%s/shader-profiles", dir);
    return 1;
}

void shader_profile_path(const char *dir, const char *slug, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.toml", dir, slug);
}

int shader_profile_rename(const char *dir, const char *from, const char *to, char *err,
                          size_t errlen)
{
    char a[PATH_CAP], b[PATH_CAP];
    shader_profile_path(dir, from, a, sizeof a);
    shader_profile_path(dir, to, b, sizeof b);
    if (!shader_profile_slug_ok(to)) {
        seterr(err, errlen, "\"%s\" is not a profile file name", to);
        return -1;
    }
    if (file_exists(b)) {
        seterr(err, errlen, "%s exists", b);
        return -1;
    }
    if (rename(a, b) != 0) {
        seterr(err, errlen, "cannot rename %s to %s", a, b);
        return -1;
    }
    return 0;
}

int shader_profile_delete(const char *dir, const char *slug, char *err, size_t errlen)
{
    char a[PATH_CAP];
    shader_profile_path(dir, slug, a, sizeof a);
    if (remove(a) != 0) {
        seterr(err, errlen, "cannot remove %s", a);
        return -1;
    }
    return 0;
}

/* The names in dir that end in ext, one level, files only. A heap array of
 * heap strings (free with names_free); the count. */
static int names_in(const char *dir, const char *ext, char ***out)
{
    char **v = NULL;
    int n = 0, cap = 0;
    size_t el = strlen(ext);
#ifdef _WIN32
    char pat[PATH_CAP];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s/*%s", dir, ext);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            const char *nm = fd.cFileName;
            size_t l = strlen(nm);
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || l <= el
                || strcmp(nm + l - el, ext))
                continue;
#else
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        {
            char full[PATH_CAP];
            const char *nm = de->d_name;
            size_t l = strlen(nm);
            if (l <= el || strcmp(nm + l - el, ext))
                continue;
            snprintf(full, sizeof full, "%s/%s", dir, nm);
            if (!file_exists(full))
                continue;
#endif
            if (n == cap) {
                char **nv = realloc(v, (size_t)(cap = cap ? cap * 2 : 16) * sizeof *v);
                if (!nv)
                    break;
                v = nv;
            }
            v[n] = malloc(l + 1);
            if (!v[n])
                break;
            memcpy(v[n++], nm, l + 1);
#ifdef _WIN32
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
        }
    }
    if (d)
        closedir(d);
#endif
    *out = v;
    return n;
}

static void names_free(char **v, int n)
{
    for (int i = 0; i < n; i++)
        free(v[i]);
    free(v);
}

static int fold_cmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = (unsigned char)*a, y = (unsigned char)*b;
        if (x >= 'A' && x <= 'Z')
            x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z')
            y += 'a' - 'A';
        if (x != y || !x)
            return x - y;
    }
}

static int entry_cmp(const void *a, const void *b)
{
    const struct shader_profile_entry *x = a, *y = b;
    int c = fold_cmp(x->name, y->name);
    return c ? c : strcmp(x->slug, y->slug);
}

int shader_profile_list(const char *dir, struct shader_profile_entry *out, int cap)
{
    char **nm;
    int n = names_in(dir, ".toml", &nm), m = 0;
    struct shader_profile_entry *all;
    struct shader_profile *p;

    if (!n) {
        names_free(nm, n);
        return 0;
    }
    all = calloc((size_t)n, sizeof *all);
    p = malloc(sizeof *p);
    if (!all || !p) {
        free(all);
        free(p);
        names_free(nm, n);
        return 0;
    }
    for (int i = 0; i < n; i++) {
        struct shader_profile_entry *e = &all[m];
        char path[PATH_CAP], slug[256];
        stem_of(nm[i], ".toml", slug, sizeof slug);
        if (!shader_profile_slug_ok(slug)) {
            fprintf(stderr, LOGP "%s/%s: the file name is not a profile slug (a-z, 0-9, - and"
                    " _, up to %d); not listed\n", dir, nm[i], SHADER_PROFILE_NAME_MAX);
            continue;
        }
        snprintf(path, sizeof path, "%s/%s", dir, nm[i]);
        e->state = shader_profile_load(path, p, e->err, sizeof e->err);
        snprintf(e->slug, sizeof e->slug, "%s", slug);
        snprintf(e->name, sizeof e->name, "%s", p->name[0] ? p->name : slug);
        e->nstages = p->nstages;
        for (int s = 0; s < p->nstages; s++)
            snprintf(e->stage[s], sizeof e->stage[s], "%s", p->stage[s]);
        m++;
    }
    qsort(all, (size_t)m, sizeof *all, entry_cmp);
    if (m > cap) {
        fprintf(stderr, LOGP "%s: %d profiles; the menu lists the first %d by name\n", dir, m,
                cap);
        m = cap;
    }
    memcpy(out, all, (size_t)m * sizeof *all);
    free(all);
    free(p);
    names_free(nm, n);
    return m;
}

static int str_cmp(const void *a, const void *b)
{
    return strcmp(*(char *const *)a, *(char *const *)b);
}

static int is_bundled_file(const char *path, const char *stem, const char *exe_dir);

int shader_profile_list_presets(const char *dir, char (*stem)[64], int cap)
{
    char **nm;
    int n = names_in(dir, ".slangp", &nm), m = 0;
    qsort(nm, (size_t)n, sizeof *nm, str_cmp);
    for (int i = 0; i < n && m < cap; i++) {
        char s[256];
        stem_of(nm[i], ".slangp", s, sizeof s);
        if (strlen(s) >= 64)
            continue;
        if (shader_profile_bundled(s) >= 0) {
            char path[PATH_CAP];
            shader_profile_path_join(dir, nm[i], 0, path, sizeof path);
            /* Listed already under its bundled name. */
            if (is_bundled_file(path, s, NULL))
                continue;
            fprintf(stderr, LOGP "%s/%s: name clashes with the bundled preset; not listed\n", dir,
                    nm[i]);
            continue;
        }
        snprintf(stem[m++], 64, "%s", s);
    }
    names_free(nm, n);
    return m;
}

/* ---- stages ------------------------------------------------------------- */

int shader_profile_bundled(const char *ref)
{
    for (int i = 0; ref && nv2a_present_chain_names[i]; i++)
        if (!strcmp(ref, nv2a_present_chain_names[i]))
            return i;
    return -1;
}

void shader_profile_stage_key(const char *ref, char *out, size_t cap)
{
    if (ref && !strncmp(ref, "user:", 5))
        ref += 5;
    nv2a_present_chain_preset_key(ref, out, cap);
}

/* a and b name one file. */
static int same_file(const char *a, const char *b)
{
#ifdef _WIN32
    char fa[PATH_CAP], fb[PATH_CAP];
    return _fullpath(fa, a, sizeof fa) && _fullpath(fb, b, sizeof fb) && !_stricmp(fa, fb);
#else
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev
        && sa.st_ino == sb.st_ino;
#endif
}

/* The bundled preset itself at path: <root>/shaders/ is the bundled folder
 * when the game runs from its own directory with root ".". */
static int is_bundled_file(const char *path, const char *stem, const char *exe_dir)
{
    char b[PATH_CAP];
    return shader_profile_stage_path(stem, NULL, exe_dir, b, sizeof b, NULL, 0) > 0
        && same_file(path, b);
}

/* A file found under <root>/shaders/ must not take a bundled name, unless
 * it is that bundled preset. */
static int user_clash(const char *path, const char *exe_dir, char *err, size_t errlen)
{
    char s[256];
    stem_of(path, ".slangp", s, sizeof s);
    if (shader_profile_bundled(s) < 0 || is_bundled_file(path, s, exe_dir))
        return 0;
    seterr(err, errlen, "%s: name clashes with the bundled preset", path);
    fprintf(stderr, LOGP "%s\n", err);
    return 1;
}

int shader_profile_stage_path(const char *ref, const char *root, const char *exe_dir,
                              char *out, size_t cap, char *err, size_t errlen)
{
    if (!exe_dir)
        exe_dir = recomp_exe_dir();
    if (!root || !*root)
        root = ".";
    if (!ref || !*ref) {
        seterr(err, errlen, "empty stage");
        return -1;
    }
    if (shader_profile_bundled(ref) >= 0) {
        snprintf(out, cap, "%s/shaders/%s.slangp", exe_dir, ref);
        if (file_exists(out))
            return 1;
#ifdef __APPLE__
        snprintf(out, cap, "%s/../Resources/shaders/%s.slangp", exe_dir, ref);
        if (file_exists(out))
            return 1;
#endif
        seterr(err, errlen, "%s: no shaders/%s.slangp beside the executable (%s)", ref, ref,
               exe_dir);
        return -1;
    }
    if (!strncmp(ref, "user:", 5)) {
        const char *stem = ref + 5;
        if (!*stem || strchr(stem, '/') || strchr(stem, '\\')) {
            seterr(err, errlen, "%s: not user:<file stem>", ref);
            return -1;
        }
        snprintf(out, cap, "%s/shaders/%s.slangp", root, stem);
        if (user_clash(out, exe_dir, err, errlen))
            return -1;
        if (file_exists(out))
            return 1;
        seterr(err, errlen, "%s: no such file (%s)", ref, out);
        return -1;
    }
    if (path_is_absolute(ref)) {
        snprintf(out, cap, "%s", ref);
        if (file_exists(out))
            return 1;
        seterr(err, errlen, "%s: no such file", ref);
        return -1;
    }
    snprintf(out, cap, "%s/shaders/%s", root, ref);
    if (file_exists(out))
        return user_clash(out, exe_dir, err, errlen) ? -1 : 1;
    snprintf(out, cap, "%s/%s", exe_dir, ref);
    if (file_exists(out))
        return 1;
    seterr(err, errlen, "%s: no such file in %s/shaders or %s", ref, root, exe_dir);
    return -1;
}

/* ---- paths -------------------------------------------------------------- */

/* Collapse "." and "x/.." segments and doubled separators in place; p uses
 * '/' only. A leading "/", "//" (UNC) or "C:/" stays. */
static void path_norm(char *p)
{
    char *seg[256];
    int ns = 0;
    size_t lead = 0, o;
    char *s, *q;

    if (p[0] && p[1] == ':' && p[2] == '/')
        lead = 3;
    else if (p[0] == '/' && p[1] == '/')
        lead = 2;
    else if (p[0] == '/')
        lead = 1;
    else if (p[0] && p[1] == ':')
        lead = 2;
    for (s = p + lead; *s;) {
        while (*s == '/')
            s++;
        if (!*s)
            break;
        q = s;
        while (*q && *q != '/')
            q++;
        if (*q)
            *q++ = 0;
        if (!strcmp(s, ".")) {
            /* nothing */
        } else if (!strcmp(s, "..") && ns && strcmp(seg[ns - 1], "..")) {
            ns--;
        } else if (!strcmp(s, "..") && lead) {
            /* above the root is the root */
        } else if (ns < (int)(sizeof seg / sizeof seg[0])) {
            seg[ns++] = s;
        }
        s = q;
    }
    o = lead;
    for (int i = 0; i < ns; i++) {
        size_t l = strlen(seg[i]);
        if (i)
            p[o++] = '/';
        memmove(p + o, seg[i], l);
        o += l;
    }
    if (!o && !lead)
        p[o++] = '.';
    p[o] = 0;
}

void shader_profile_path_join(const char *base, const char *rel, int win, char *out,
                              size_t cap)
{
    char *t;
    size_t n;
    if (!cap)
        return;
    if (path_is_absolute(rel) || !base || !*base)
        snprintf(out, cap, "%s", rel);
    else
        snprintf(out, cap, "%s/%s", base, rel);
    n = strlen(out);
    /* path_norm only shortens, so it works in place on a copy. */
    t = malloc(n + 1);
    if (!t)
        return;
    memcpy(t, out, n + 1);
    for (size_t i = 0; i < n && win; i++)
        if (t[i] == '\\')
            t[i] = '/';
    path_norm(t);
    snprintf(out, cap, "%s", t);
    free(t);
}

static void make_absolute(const char *p, char *out, size_t cap)
{
    char cwd[PATH_CAP];
    if (path_is_absolute(p)) {
        shader_profile_path_join(NULL, p, HOST_WIN, out, cap);
        return;
    }
#ifdef _WIN32
    if (!_getcwd(cwd, sizeof cwd))
#else
    if (!getcwd(cwd, sizeof cwd))
#endif
        snprintf(cwd, sizeof cwd, ".");
    shader_profile_path_join(cwd, p, HOST_WIN, out, cap);
}

/* ---- the .slangp reader -------------------------------------------------- */

struct kv {
    char *key, *val;
    int dir;      /* index into the preset's dirs: the file that set it */
};

struct slp {
    struct kv *kv;
    int n, cap;
    char **dirs;
    int ndirs;
};

static void slp_free(struct slp *s)
{
    for (int i = 0; i < s->n; i++) {
        free(s->kv[i].key);
        free(s->kv[i].val);
    }
    for (int i = 0; i < s->ndirs; i++)
        free(s->dirs[i]);
    free(s->kv);
    free(s->dirs);
    memset(s, 0, sizeof *s);
}

static char *dup_n(const char *s, size_t n)
{
    char *d = malloc(n + 1);
    if (d) {
        memcpy(d, s, n);
        d[n] = 0;
    }
    return d;
}

static const struct kv *slp_get(const struct slp *s, const char *key)
{
    for (int i = 0; i < s->n; i++)
        if (!strcmp(s->kv[i].key, key))
            return &s->kv[i];
    return NULL;
}

static int slp_set(struct slp *s, const char *key, size_t kl, const char *val, size_t vl, int dir)
{
    for (int i = 0; i < s->n; i++)
        if (strlen(s->kv[i].key) == kl && !memcmp(s->kv[i].key, key, kl)) {
            char *v = dup_n(val, vl);
            if (!v)
                return 0;
            free(s->kv[i].val);
            s->kv[i].val = v;
            s->kv[i].dir = dir;
            return 1;
        }
    if (s->n == s->cap) {
        int nc = s->cap ? s->cap * 2 : 32;
        struct kv *nk = realloc(s->kv, (size_t)nc * sizeof *nk);
        if (!nk)
            return 0;
        s->kv = nk;
        s->cap = nc;
    }
    s->kv[s->n].key = dup_n(key, kl);
    s->kv[s->n].val = dup_n(val, vl);
    s->kv[s->n].dir = dir;
    if (!s->kv[s->n].key || !s->kv[s->n].val)
        return 0;
    s->n++;
    return 1;
}

static int slp_dir(struct slp *s, const char *dir)
{
    char **nd = realloc(s->dirs, (size_t)(s->ndirs + 1) * sizeof *nd);
    if (!nd)
        return -1;
    s->dirs = nd;
    s->dirs[s->ndirs] = dup_n(dir, strlen(dir));
    return s->dirs[s->ndirs] ? s->ndirs++ : -1;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

/* A value: "quoted" whole, else up to a comment, trimmed. */
static void value_span(const char *v, const char *end, const char **vs, size_t *vl)
{
    while (v < end && is_space(*v))
        v++;
    if (v < end && *v == '"') {
        const char *q = memchr(v + 1, '"', (size_t)(end - v - 1));
        *vs = v + 1;
        *vl = q ? (size_t)(q - v - 1) : (size_t)(end - v - 1);
        return;
    }
    *vs = v;
    for (const char *c = v; c < end; c++)
        if (*c == '#' || (*c == '/' && c + 1 < end && c[1] == '/')) {
            end = c;
            break;
        }
    while (end > v && is_space(end[-1]))
        end--;
    *vl = (size_t)(end - v);
}

/* Read path into s, its #reference files first so its own lines override
 * theirs (how our wrappers set warpX). stack: the files being read, for
 * the cycle check. */
static int slp_read(struct slp *s, const char *path, int depth, char **stack, char *err,
                    size_t errlen)
{
    char dir[PATH_CAP], *text;
    int di, pass, ok = 1;

    for (int i = 0; i < depth; i++)
        if (!strcmp(stack[i], path)) {
            seterr(err, errlen, "%s: #reference cycle", path);
            return 0;
        }
    if (depth >= REF_DEPTH_MAX) {
        seterr(err, errlen, "%s: #reference nested deeper than %d", path, REF_DEPTH_MAX);
        return 0;
    }
    text = read_text(path);
    if (!text) {
        seterr(err, errlen, "%s: cannot read", path);
        return 0;
    }
    stack[depth] = (char *)path;
    dir_of(path, dir, sizeof dir);
    di = slp_dir(s, dir);
    if (di < 0) {
        free(text);
        seterr(err, errlen, "out of memory");
        return 0;
    }
    for (pass = 0; pass < 2 && ok; pass++) {
        for (const char *l = text; *l && ok;) {
            const char *e = strchr(l, '\n'), *b = l, *eq;
            if (!e)
                e = l + strlen(l);
            while (b < e && is_space(*b))
                b++;
            if (!strncmp(b, "#reference", 10) && pass == 0) {
                const char *vs;
                size_t vl;
                char rel[PATH_CAP], full[PATH_CAP];
                value_span(b + 10, e, &vs, &vl);
                snprintf(rel, sizeof rel, "%.*s", (int)vl, vs);
                shader_profile_path_join(dir, rel, HOST_WIN, full, sizeof full);
                ok = slp_read(s, full, depth + 1, stack, err, errlen);
            } else if (pass == 1 && b < e && *b != '#' && !(b[0] == '/' && b[1] == '/')
                       && (eq = memchr(b, '=', (size_t)(e - b)))) {
                const char *ke = eq, *vs;
                size_t vl;
                while (ke > b && is_space(ke[-1]))
                    ke--;
                value_span(eq + 1, e, &vs, &vl);
                if (ke > b && !slp_set(s, b, (size_t)(ke - b), vs, vl, di)) {
                    seterr(err, errlen, "out of memory");
                    ok = 0;
                }
            }
            l = *e ? e + 1 : e;
        }
    }
    free(text);
    return ok;
}

/* The per-pass settings copied through (librashader's preset keys, each
 * followed by the pass index). */
static const char *const k_pass_keys[] = {
    "shader", "filter_linear", "scale_type", "scale_type_x", "scale_type_y", "scale",
    "scale_x", "scale_y", "wrap_mode", "mipmap_input", "float_framebuffer",
    "srgb_framebuffer", "frame_count_mod", "alias", NULL,
};
static const char *const k_tex_suffixes[] = { "_linear", "_wrap_mode", "_mipmap", NULL };

/* key = <pass key><index>: the pass key's index in k_pass_keys and the
 * pass index, or -1. */
static int pass_key(const char *key, int *pass)
{
    size_t n = strlen(key), d = n;
    while (d && key[d - 1] >= '0' && key[d - 1] <= '9')
        d--;
    if (d == n || d == 0 || n - d > 4)
        return -1;
    for (int i = 0; k_pass_keys[i]; i++)
        if (strlen(k_pass_keys[i]) == d && !memcmp(key, k_pass_keys[i], d)) {
            *pass = atoi(key + d);
            return i;
        }
    return -1;
}

/* name in a ';'-separated list. */
static int list_has(const char *list, const char *name)
{
    size_t nl = strlen(name);
    for (const char *s = list; s && *s;) {
        const char *e = strchr(s, ';');
        size_t l = e ? (size_t)(e - s) : strlen(s);
        if (l == nl && !memcmp(s, name, l))
            return 1;
        s = e ? e + 1 : NULL;
    }
    return 0;
}

static int tex_key(const struct slp *s, const char *key)
{
    const struct kv *t = slp_get(s, "textures");
    char name[128];
    if (!t)
        return 0;
    if (list_has(t->val, key))
        return 1;
    for (int i = 0; k_tex_suffixes[i]; i++) {
        size_t n = strlen(key), sl = strlen(k_tex_suffixes[i]);
        if (n > sl && n - sl < sizeof name && !strcmp(key + n - sl, k_tex_suffixes[i])) {
            memcpy(name, key, n - sl);
            name[n - sl] = 0;
            if (list_has(t->val, name))
                return 1;
        }
    }
    return 0;
}

/* pass_key, unless the preset's parameters list names key: a parameter
 * called scale1 or alias2 is a value, not a pass setting. */
static int slp_pass_key(const struct slp *s, const char *key, int *pass)
{
    const struct kv *pl = slp_get(s, "parameters");
    if (pl && list_has(pl->val, key))
        return -1;
    return pass_key(key, pass);
}

/* A preset-level "name = <number>" line: a parameter value. */
static int value_line(const struct slp *s, const struct kv *kv)
{
    int pass;
    double d;
    if (!strcmp(kv->key, "shaders") || !strcmp(kv->key, "textures")
        || !strcmp(kv->key, "parameters") || !strcmp(kv->key, "feedback_pass")
        || slp_pass_key(s, kv->key, &pass) >= 0 || tex_key(s, kv->key))
        return 0;
    return recomp_cfg_parse_double(kv->val, &d);
}

/* ---- the shader scan ---------------------------------------------------- */

#define SCAN_SEEN_MAX 64
struct scan {
    char pass_index[64];   /* the first PassOutputN-style name read, or "" */
    int original;          /* Original* read */
    /* The files read, by path hash: an #include met twice (a diamond, or
     * a file including itself) is read once, not once per path to it. */
    unsigned long long seen[SCAN_SEEN_MAX];
    int nseen;
};

static int ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')
        || c == '_';
}

static int digits_after(const char *id, size_t l, const char *pre)
{
    size_t p = strlen(pre);
    if (l <= p || memcmp(id, pre, p))
        return 0;
    for (size_t i = p; i < l; i++)
        if (id[i] < '0' || id[i] > '9')
            return 0;
    return 1;
}

/* A type a uniform member or a sampler is declared with. */
static int decl_type(const char *id, size_t l)
{
    static const char *const k[] = { "vec2", "vec3", "vec4", "float", "int", "uint",
                                     "mat4", "sampler2D", NULL };
    for (int i = 0; k[i]; i++)
        if (strlen(k[i]) == l && !memcmp(id, k[i], l))
            return 1;
    return 0;
}

static void scan_text(const char *t, struct scan *sc)
{
    const char *prev = NULL;
    size_t prevl = 0;
    for (const char *s = t; *s;) {
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n')
                s++;
        } else if (s[0] == '/' && s[1] == '*') {
            const char *e = strstr(s + 2, "*/");
            s = e ? e + 2 : s + strlen(s);
        } else if (ident_char(*s) && !(*s >= '0' && *s <= '9')) {
            const char *b = s;
            size_t l;
            while (ident_char(*s))
                s++;
            l = (size_t)(s - b);
            {
                /* "vec4 OriginalSize;" in every UBO is a declaration, not a
                 * read: only a use flags the stage. */
                const char *n = s;
                int decl;
                while (*n == ' ' || *n == '\t')
                    n++;
                decl = prev && decl_type(prev, prevl) && *n == ';';
                if (!decl && !sc->pass_index[0]
                    && (digits_after(b, l, "PassOutput") || digits_after(b, l, "PassOutputSize")
                        || digits_after(b, l, "PassFeedback")
                        || digits_after(b, l, "PassFeedbackSize")))
                    snprintf(sc->pass_index, sizeof sc->pass_index, "%.*s", (int)l, b);
                if (!decl && l >= 8 && !memcmp(b, "Original", 8))
                    sc->original = 1;
            }
            prev = b;
            prevl = l;
        } else {
            s++;
        }
    }
}

/* A pass's .slang and its #include files. */
static void scan_file(const char *path, int depth, struct scan *sc)
{
    char dir[PATH_CAP], *text;
    unsigned long long h = 1469598103934665603ull;   /* FNV-1a */
    for (const char *p = path; *p; p++)
        h = (h ^ (unsigned char)*p) * 1099511628211ull;
    for (int i = 0; i < sc->nseen; i++)
        if (sc->seen[i] == h)
            return;
    if (depth > REF_DEPTH_MAX || !(text = read_text(path)))
        return;
    if (sc->nseen < SCAN_SEEN_MAX)
        sc->seen[sc->nseen++] = h;
    dir_of(path, dir, sizeof dir);
    scan_text(text, sc);
    for (const char *l = text; *l;) {
        const char *e = strchr(l, '\n'), *b = l;
        if (!e)
            e = l + strlen(l);
        while (b < e && is_space(*b))
            b++;
        if (!strncmp(b, "#include", 8)) {
            const char *q = memchr(b, '"', (size_t)(e - b)), *q2;
            if (q && (q2 = memchr(q + 1, '"', (size_t)(e - q - 1)))) {
                char rel[PATH_CAP], full[PATH_CAP];
                snprintf(rel, sizeof rel, "%.*s", (int)(q2 - q - 1), q + 1);
                shader_profile_path_join(dir, rel, HOST_WIN, full, sizeof full);
                scan_file(full, depth + 1, sc);
            }
        }
        l = *e ? e + 1 : e;
    }
    free(text);
}

/* ---- the composer ------------------------------------------------------- */

struct buf {
    char *p;
    size_t n, cap;
    int oom;
};

static void bput(struct buf *b, const char *fmt, ...)
{
    va_list ap;
    int need;
    if (b->oom)
        return;
    va_start(ap, fmt);
    need = vsnprintf(b->p ? b->p + b->n : NULL, b->p ? b->cap - b->n : 0, fmt, ap);
    va_end(ap);
    if (need < 0) {
        b->oom = 1;
        return;
    }
    if (!b->p || b->n + (size_t)need + 1 > b->cap) {
        size_t nc = b->cap ? b->cap : 1024;
        char *np;
        while (nc < b->n + (size_t)need + 1)
            nc *= 2;
        np = realloc(b->p, nc);
        if (!np) {
            b->oom = 1;
            return;
        }
        b->p = np;
        b->cap = nc;
        va_start(ap, fmt);
        vsnprintf(b->p + b->n, b->cap - b->n, fmt, ap);
        va_end(ap);
    }
    b->n += (size_t)need;
}

/* A preset path value made absolute against the file that named it. */
static void abs_value(const struct slp *s, const struct kv *kv, char *out, size_t cap)
{
    char t[PATH_CAP];
    shader_profile_path_join(s->dirs[kv->dir], kv->val, HOST_WIN, t, sizeof t);
    make_absolute(t, out, cap);
}

static int quotable(const char *p)
{
    return !strchr(p, '"') && !strchr(p, '\n');
}

/* Whether a pass hands on a picture of another size than its input. */
static int pass_scales(const struct slp *s, int i, int last)
{
    char k[32];
    const struct kv *t, *sc;
    double f = 1;
    snprintf(k, sizeof k, "scale_type%d", i);
    t = slp_get(s, k);
    if (!t) {
        snprintf(k, sizeof k, "scale_type_x%d", i);
        t = slp_get(s, k);
    }
    if (!t)
        return last;   /* a preset's last pass draws at the viewport */
    if (strcmp(t->val, "source"))
        return 1;      /* viewport, absolute */
    snprintf(k, sizeof k, "scale%d", i);
    sc = slp_get(s, k);
    if (!sc) {
        snprintf(k, sizeof k, "scale_x%d", i);
        sc = slp_get(s, k);
    }
    if (sc && !recomp_cfg_parse_double(sc->val, &f))
        f = 1;
    return f != 1;
}

char *shader_profile_compose_text(const char *const *paths, const char *const *labels, int n,
                                  const char *slug, struct shader_profile_compose *info,
                                  char *err, size_t errlen)
{
    struct slp st[SHADER_PROFILE_STAGES_MAX];
    int npass[SHADER_PROFILE_STAGES_MAX], total = 0, ok = 1, scaled = 0;
    struct shader_profile_compose inf;
    struct buf b = { 0 };
    char e[SHADER_PROFILE_ERR_MAX + 256] = "";
    const char *who = slug && *slug ? slug : "~unsaved";

    memset(st, 0, sizeof st);
    memset(&inf, 0, sizeof inf);
    if (n < 1 || n > SHADER_PROFILE_STAGES_MAX) {
        snprintf(e, sizeof e, "%d stages; 1 to %d", n, SHADER_PROFILE_STAGES_MAX);
        ok = 0;
    }
    /* Read each stage; count and check its passes. */
    for (int i = 0; ok && i < n; i++) {
        char *stack[REF_DEPTH_MAX], abs[PATH_CAP], why[512] = "";
        const char *label = labels && labels[i] ? labels[i] : paths[i];
        const struct kv *ns;
        make_absolute(paths[i], abs, sizeof abs);
        if (!slp_read(&st[i], abs, 0, stack, why, sizeof why)) {
            snprintf(e, sizeof e, "stage %d (%s): %s", i + 1, label, why);
            ok = 0;
            break;
        }
        ns = slp_get(&st[i], "shaders");
        npass[i] = ns ? atoi(ns->val) : 0;
        if (npass[i] < 1) {
            snprintf(e, sizeof e, "stage %d (%s): no shaders = <count>", i + 1, label);
            ok = 0;
            break;
        }
        inf.stage[i].first_pass = total;
        inf.stage[i].passes = npass[i];
        if (slp_get(&st[i], "feedback_pass"))
            inf.stage[i].flags |= SHADER_PROFILE_NO_FEEDBACK;
        total += npass[i];
        if (total > SHADER_PROFILE_PASSES_MAX) {
            snprintf(e, sizeof e, "%d passes or more; the limit is %d", total,
                     SHADER_PROFILE_PASSES_MAX);
            ok = 0;
            break;
        }
        if (scaled)
            inf.stage[i].flags |= SHADER_PROFILE_SCALED;
        for (int p = 0; ok && p < npass[i]; p++) {
            char k[32], sh[PATH_CAP];
            const struct kv *kv;
            struct scan sc = { 0 };
            snprintf(k, sizeof k, "shader%d", p);
            kv = slp_get(&st[i], k);
            if (!kv) {
                snprintf(e, sizeof e, "stage %d (%s): no shader%d", i + 1, label, p);
                ok = 0;
                break;
            }
            abs_value(&st[i], kv, sh, sizeof sh);
            if (!file_exists(sh)) {
                snprintf(e, sizeof e, "stage %d (%s): %s: no such file", i + 1, label, sh);
                ok = 0;
                break;
            }
            scan_file(sh, 0, &sc);
            if (i > 0 && sc.pass_index[0]) {
                snprintf(e, sizeof e, "stage %d (%s) uses %s, which renumbering breaks", i + 1,
                         label, sc.pass_index);
                ok = 0;
                break;
            }
            if (i > 0 && sc.original)
                inf.stage[i].flags |= SHADER_PROFILE_ORIGINAL;
            if (pass_scales(&st[i], p, p == npass[i] - 1))
                scaled = 1;
        }
        /* Aliases and textures are sampled by name in the sources. */
        for (int j = 0; ok && j < i; j++) {
            const struct kv *tj = slp_get(&st[j], "textures"), *ti = slp_get(&st[i], "textures");
            for (int a = 0; ok && a < st[i].n; a++) {
                int pa;
                if (slp_pass_key(&st[i], st[i].kv[a].key, &pa) != 13 || !st[i].kv[a].val[0])
                    continue;
                for (int c = 0; c < st[j].n; c++) {
                    int pc;
                    if (slp_pass_key(&st[j], st[j].kv[c].key, &pc) == 13
                        && !strcmp(st[j].kv[c].val, st[i].kv[a].val)) {
                        snprintf(e, sizeof e, "stage %d (%s) reuses alias \"%s\" from stage %d",
                                 i + 1, label, st[i].kv[a].val, j + 1);
                        ok = 0;
                        break;
                    }
                }
            }
            for (const char *s = ti ? ti->val : NULL; ok && tj && s && *s;) {
                const char *x = strchr(s, ';');
                char name[128];
                snprintf(name, sizeof name, "%.*s", (int)(x ? (size_t)(x - s) : strlen(s)), s);
                if (name[0] && list_has(tj->val, name)) {
                    snprintf(e, sizeof e, "stage %d (%s) reuses texture \"%s\" from stage %d",
                             i + 1, label, name, j + 1);
                    ok = 0;
                }
                s = x ? x + 1 : NULL;
            }
        }
    }
    inf.passes = total;
    inf.nstages = n;

    if (ok) {
        int base = 0, any_list = 0, ntex = 0;
        bput(&b, "# composed by xboxrecomp from shader profile %s; rewritten at each compose\n",
             who);
        bput(&b, "shaders = %d\n", total);
        for (int i = 0; ok && i < n; i++) {
            const char *label = labels && labels[i] ? labels[i] : paths[i];
            bput(&b, "\n# stage %d: %s\n", i + 1, label);
            for (int p = 0; ok && p < npass[i]; p++) {
                int has_type = 0;
                for (int a = 0; a < st[i].n; a++) {
                    const struct kv *kv = &st[i].kv[a];
                    int pa, ki = slp_pass_key(&st[i], kv->key, &pa);
                    if (ki < 0 || pa != p)
                        continue;
                    if (ki == 0) {
                        char sh[PATH_CAP];
                        abs_value(&st[i], kv, sh, sizeof sh);
                        if (!quotable(sh)) {
                            snprintf(e, sizeof e, "stage %d (%s): %s has a quote in its path",
                                     i + 1, label, sh);
                            ok = 0;
                            break;
                        }
                        bput(&b, "shader%d = \"%s\"\n", base + p, sh);
                        continue;
                    }
                    if (ki >= 2 && ki <= 4)
                        has_type = 1;
                    bput(&b, "%s%d = \"%s\"\n", k_pass_keys[ki], base + p, kv->val);
                }
                /* Alone, a preset's last pass draws at the viewport; inside
                 * a chain it would draw at its source's size. */
                if (!has_type && p == npass[i] - 1 && i < n - 1)
                    bput(&b, "scale_type%d = \"viewport\"\n", base + p);
            }
            base += npass[i];
        }
        /* Textures, merged: their names are distinct (checked above). */
        for (int i = 0; i < n; i++)
            if (slp_get(&st[i], "textures"))
                ntex++;
        if (ntex) {
            int first = 1;
            bput(&b, "\ntextures = \"");
            for (int i = 0; i < n; i++) {
                const struct kv *t = slp_get(&st[i], "textures");
                if (t && t->val[0]) {
                    bput(&b, "%s%s", first ? "" : ";", t->val);
                    first = 0;
                }
            }
            bput(&b, "\"\n");
            for (int i = 0; ok && i < n; i++)
                for (int a = 0; a < st[i].n; a++) {
                    const struct kv *kv = &st[i].kv[a];
                    const struct kv *t = slp_get(&st[i], "textures");
                    if (!tex_key(&st[i], kv->key))
                        continue;
                    if (t && list_has(t->val, kv->key)) {
                        char tp[PATH_CAP];
                        abs_value(&st[i], kv, tp, sizeof tp);
                        if (!quotable(tp)) {
                            snprintf(e, sizeof e, "stage %d: %s has a quote in its path", i + 1,
                                     tp);
                            ok = 0;
                            break;
                        }
                        bput(&b, "%s = \"%s\"\n", kv->key, tp);
                    } else {
                        bput(&b, "%s = \"%s\"\n", kv->key, kv->val);
                    }
                }
        }
        /* The parameters lists merged in stage order, without duplicates. */
        for (int i = 0; i < n; i++)
            any_list |= slp_get(&st[i], "parameters") != NULL;
        if (ok && any_list) {
            struct buf l = { 0 };
            for (int i = 0; i < n; i++) {
                const struct kv *pl = slp_get(&st[i], "parameters");
                for (const char *s = pl ? pl->val : NULL; s && *s;) {
                    const char *x = strchr(s, ';');
                    char name[128];
                    snprintf(name, sizeof name, "%.*s",
                             (int)(x ? (size_t)(x - s) : strlen(s)), s);
                    if (name[0] && !(l.p && list_has(l.p, name)))
                        bput(&l, "%s%s", l.n ? ";" : "", name);
                    s = x ? x + 1 : NULL;
                }
            }
            bput(&b, "\nparameters = \"%s\"\n", l.p ? l.p : "");
            b.oom |= l.oom;
            free(l.p);
        }
        /* Each value from the earliest stage that sets it: one value per
         * name in a librashader chain. */
        if (ok) {
            struct buf seen = { 0 };
            int head = 0;
            for (int i = 0; i < n; i++)
                for (int a = 0; a < st[i].n; a++) {
                    const struct kv *kv = &st[i].kv[a];
                    if (!value_line(&st[i], kv) || (seen.p && list_has(seen.p, kv->key)))
                        continue;
                    if (!head++)
                        bput(&b, "\n");
                    bput(&b, "%s = \"%s\"\n", kv->key, kv->val);
                    bput(&seen, "%s%s", seen.n ? ";" : "", kv->key);
                }
            b.oom |= seen.oom;
            free(seen.p);
        }
        if (b.oom) {
            snprintf(e, sizeof e, "out of memory");
            ok = 0;
        }
    }
    for (int i = 0; i < SHADER_PROFILE_STAGES_MAX; i++)
        slp_free(&st[i]);
    if (info)
        *info = inf;
    if (!ok) {
        free(b.p);
        seterr(err, errlen, "%s", e);
        fprintf(stderr, LOGP "profile %s: %s\n", who, e);
        return NULL;
    }
    return b.p;
}

int shader_profile_compose(const char *const *paths, const char *const *labels, int n,
                           const char *slug, const char *out_path,
                           struct shader_profile_compose *info, char *err, size_t errlen)
{
    char tmp[PATH_CAP];
    struct shader_profile_compose own;
    char *text = shader_profile_compose_text(paths, labels, n, slug, info ? info : &own, err,
                                             errlen);
    FILE *f;
    size_t len;
    int bad;
    if (!text)
        return -1;
    if (!info)
        info = &own;
    /* A chain has one feedback_pass, in the preset's numbering; a stage's
     * own would point at the wrong pass, so it goes, and the stage may
     * look different from its preset alone. */
    for (int i = 0; i < info->nstages; i++)
        if (info->stage[i].flags & SHADER_PROFILE_NO_FEEDBACK)
            fprintf(stderr, LOGP "profile %s: stage %d (%s): its feedback_pass is dropped\n",
                    slug && *slug ? slug : "~unsaved", i + 1,
                    labels && labels[i] ? labels[i] : paths[i]);
    mkdir_parent(out_path);
    snprintf(tmp, sizeof tmp, "%s.tmp", out_path);
    f = fopen(tmp, "wb");
    len = strlen(text);
    bad = !f || fwrite(text, 1, len, f) != len;
    /* On the disk before the rename, as recomp_cfg's Save does: a delayed
     * allocation filesystem can land the rename first and leave an empty
     * preset for librashader after a power loss. */
    if (f && !bad) {
        bad = fflush(f) != 0;
#ifdef _WIN32
        bad = bad || !FlushFileBuffers((HANDLE)_get_osfhandle(_fileno(f)));
#else
        bad = bad || fsync(fileno(f)) != 0;
#endif
    }
    if (f && fclose(f))
        bad = 1;
    free(text);
    if (bad || replace_file(tmp, out_path) != 0) {
        remove(tmp);
        seterr(err, errlen, "%s: cannot write", out_path);
        fprintf(stderr, LOGP "profile %s: %s\n", slug && *slug ? slug : "~unsaved", err);
        return -1;
    }
    return 0;
}

void shader_profile_cache_path(const char *cache_root, const char *slug, char *out, size_t cap)
{
    snprintf(out, cap, "%s/slangp/%s.slangp", cache_root, slug && *slug ? slug : "~unsaved");
}
