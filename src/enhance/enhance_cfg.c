/*
 * enhance_cfg.c - config.toml over config.default.toml under the
 * environment; see enhance_cfg.h.
 */
#include "enhance_cfg.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "recomp_exe_dir.h"

#define ENHANCE_MAX_BINDS 64
/* Slots in lookup order: the first that holds a key answers. */
#define SLOT_TITLE 0
#define SLOT_USER 1
#define SLOT_DEFAULTS 2
#define ENHANCE_FILES 3
#define CFG_PATH_MAX 1024

typedef struct {
    char key[64];
    recomp_env_id id;
    int warned;
} Bind;

static Bind s_binds[ENHANCE_MAX_BINDS];
static int s_n_binds;
static int s_builtin_bound;

static recomp_cfg *s_file[ENHANCE_FILES];
/* Per file entry: bit 0 read by a lookup, bit 1 type warning printed. */
static unsigned char *s_flags[ENHANCE_FILES];
/* The file a writer (the menu's Save) targets; empty under none. */
static char s_user_path[CFG_PATH_MAX];

static void bind_builtins(void)
{
    if (s_builtin_bound)
        return;
    s_builtin_bound = 1;
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    enhance_cfg_bind_env("render.scale", RENV_RENDER_SCALE);
    enhance_cfg_bind_env("render.aniso", RENV_RENDER_ANISO);
    enhance_cfg_bind_env("display.aspect", RENV_DISPLAY_ASPECT);
    enhance_cfg_bind_env("present.filter", RENV_PRESENT_FILTER);
    enhance_cfg_bind_env("present.fullscreen", RENV_PRESENT_FULLSCREEN);
    enhance_cfg_bind_env("present.pacing", RENV_PRESENT_PACING);
    enhance_cfg_bind_env("present.surface", RENV_PRESENT_SURFACE);
    enhance_cfg_bind_env("textures.pack", RENV_TEXTURES_PACK);
    enhance_cfg_bind_env("textures.font", RENV_TEXTURES_FONT);
    enhance_cfg_bind_env("textures.text", RENV_TEXTURES_TEXT);
#endif
}

int enhance_cfg_bind_env(const char *key, recomp_env_id id)
{
    int i;
    if (!key || strlen(key) >= sizeof s_binds[0].key)
        return 0;
    for (i = 0; i < s_n_binds; i++)
        if (!strcmp(s_binds[i].key, key)) {
            s_binds[i].id = id;
            s_binds[i].warned = 0;
            return 1;
        }
    if (s_n_binds == ENHANCE_MAX_BINDS)
        return 0;
    strcpy(s_binds[s_n_binds].key, key);
    s_binds[s_n_binds].id = id;
    s_binds[s_n_binds].warned = 0;
    s_n_binds++;
    return 1;
}

static Bind *bind_of(const char *key)
{
    int i;
    bind_builtins();
    for (i = 0; i < s_n_binds; i++)
        if (!strcmp(s_binds[i].key, key))
            return &s_binds[i];
    return NULL;
}

/* The env value for `key`, or NULL (unbound, unset or empty). */
static int file_of(const char *key, recomp_cfg_type *type);

/* A file key the environment overrides was still read: the user set it, and
 * an "unused key" line for it would send them looking for a typo. */
static const char *env_of(const char *key, Bind **out)
{
    Bind *b = bind_of(key);
    const char *v = b ? recomp_env(b->id) : NULL;
    recomp_cfg_type t;
    *out = b;
    if (!v || !*v)
        return NULL;
    file_of(key, &t);
    return v;
}

static void env_bad(Bind *b, const char *v, const char *want)
{
    if (b->warned)
        return;
    b->warned = 1;
    fprintf(stderr, "[ENHANCE] %s=%s is not %s; ignored\n",
            recomp_env_name(b->id), v, want);
}

/* ── files ─────────────────────────────────────────────────────────── */

void enhance_cfg_shutdown(void)
{
    int i;
    for (i = 0; i < ENHANCE_FILES; i++) {
        recomp_cfg_free(s_file[i]);
        s_file[i] = NULL;
        free(s_flags[i]);
        s_flags[i] = NULL;
    }
    s_user_path[0] = 0;
}

/* Whether `path` names anything. stat, not a trial fopen: a file the
 * player cannot read is still there, and must never count as absent to
 * the rename or the header, which would replace or truncate it. */
static int file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Load `path` into slot `i`. 1 loaded, 0 absent (when optional), -1 error. */
static int load_slot(int i, const char *path, int optional)
{
    char err[512];
    recomp_cfg *c;
    if (optional && !file_exists(path))
        return 0;
    c = recomp_cfg_load_file(path, err, sizeof err);
    if (!c) {
        fprintf(stderr, "[ENHANCE] config error: %s (file ignored)\n", err);
        return -1;
    }
    s_file[i] = c;
    s_flags[i] = (unsigned char *)calloc(recomp_cfg_count(c) + 1, 1);
    return 1;
}

static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

/* The part of `path` after its last separator. */
static const char *base_of(const char *path)
{
    const char *b = path, *p;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    return b;
}

/* `path`'s directory into `out` ("." when it names none). */
static void dir_of(const char *path, char *out, size_t cap)
{
    size_t n = (size_t)(base_of(path) - path);
    if (n == 0) {
        snprintf(out, cap, ".");
        return;
    }
    if (n > 1)
        n--;                            /* drop the separator, keep a lone "/" */
    if (n >= cap)
        n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

/* Move `from` to `to` only while `to` is absent. POSIX rename() replaces
 * the target, so a config.toml that appeared since the check (another
 * launch, the player) would be lost; link() fails with EEXIST instead.
 * A file system without hard links falls back to rename(). Windows'
 * rename() already refuses an existing target. 0 moved, -1 errno set. */
static int move_no_replace(const char *from, const char *to)
{
#ifndef _WIN32
    if (link(from, to) == 0)
        return unlink(from) == 0 || errno == ENOENT ? 0 : -1;
    if (errno == EEXIST || errno == ENOENT)
        return -1;
#endif
    return rename(from, to);
}

/* The one-time rename of an earlier version's enhance.toml (same keys) to
 * a missing config.toml beside it. Only a file named config.toml is a
 * candidate, so an explicit RECOMP_CONFIG with another name never moves a
 * file. 0 nothing to do or renamed, -1 the rename failed (the caller then
 * leaves the directory alone, so the next run retries). */
static int migrate(const char *path)
{
    char dir[CFG_PATH_MAX], old[CFG_PATH_MAX + 16];
    if (strcmp(base_of(path), "config.toml") != 0 || file_exists(path))
        return 0;
    dir_of(path, dir, sizeof dir);
    snprintf(old, sizeof old, "%s/enhance.toml", dir);
    if (!file_exists(old))
        return 0;
    if (move_no_replace(old, path) != 0) {
        /* A second launch racing this one: it moved the file first. */
        if (file_exists(path))
            return 0;
        fprintf(stderr, "[ENHANCE] config: cannot rename %s to config.toml (%s); left as is\n",
                old, strerror(errno));
        return -1;
    }
    fprintf(stderr, "[ENHANCE] config: renamed %s to config.toml (one-time migration)\n", old);
    return 0;
}

/* An absent user file becomes a comment header, so a player finds a file
 * to edit that says where every other key comes from, and a writer always
 * patches an existing file. Never a directory: a missing one means the
 * data root is not set up, and the run reads the defaults alone. */
static void write_header(const char *path, const char *title)
{
    char dir[CFG_PATH_MAX];
    FILE *f;
    if (file_exists(path))
        return;
    dir_of(path, dir, sizeof dir);
    if (!is_dir(dir))
        return;
    /* "x": create only. A launch racing this one may have just renamed
     * or written the file; that one wins and nothing is said. */
    f = fopen(path, "wbx");
    if (!f) {
        if (errno == EEXIST)
            return;
        fprintf(stderr, "[ENHANCE] config: cannot create %s (%s)\n", path, strerror(errno));
        return;
    }
    fprintf(f, "# %s settings: only the keys you change go here. Every other key\n"
               "# comes from config.default.toml shipped with the game, which is read,\n"
               "# never copied, so a new version's keys apply to you too.\n"
               "# Environment variables (RECOMP_*, config/launch.env in a bundle)\n"
               "# override this file.\n",
            title && *title ? title : "Your");
    fclose(f);
}

/* config.default.toml beside the executable; in a macOS app bundle the
 * read-only files sit in Contents/Resources, next to Contents/MacOS. */
static void defaults_path(char *out, size_t cap)
{
    snprintf(out, cap, "%s/config.default.toml", recomp_exe_dir());
#ifdef __APPLE__
    if (!file_exists(out)) {
        char alt[CFG_PATH_MAX + 32];
        snprintf(alt, sizeof alt, "%s/../Resources/config.default.toml", recomp_exe_dir());
        if (file_exists(alt))
            snprintf(out, cap, "%s", alt);
    }
#endif
}

int enhance_cfg_init(const char *root, const char *title)
{
    const char *over;
    char path[CFG_PATH_MAX], tpath[CFG_PATH_MAX], upath[CFG_PATH_MAX];
    int loaded = 0, bad = 0, r, i, listed = 0, have_title;

    enhance_cfg_shutdown();
    bind_builtins();
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    over = recomp_env(RENV_CONFIG);
#else
    over = NULL;
#endif
    if (over && *over && !strcmp(over, "none")) {
        /* The golden pin: code defaults and the environment, whatever a
         * shipped config.default.toml turns on. Nothing is written. */
        fprintf(stderr, "[ENHANCE] config: none (RECOMP_CONFIG)\n");
        goto env_lines;
    }

    defaults_path(path, sizeof path);
    r = load_slot(SLOT_DEFAULTS, path, 1);
    loaded += r > 0;
    bad |= r < 0;

    if (over && *over) {
        snprintf(s_user_path, sizeof s_user_path, "%s", over);
        if (migrate(over) == 0)
            write_header(over, title);
        r = load_slot(SLOT_USER, over, 0);
        loaded += r > 0;
        bad |= r < 0;
    } else {
        int mt = 0, mu;
        if (!root || !*root)
            root = ".";
        have_title = title && *title;
        if (have_title) {
            snprintf(tpath, sizeof tpath, "%s/%s/config.toml", root, title);
            mt = migrate(tpath);
        }
        snprintf(upath, sizeof upath, "%s/config/config.toml", root);
        mu = migrate(upath);
        /* One header, on the file a writer would target. */
        snprintf(path, sizeof path, "%s/%s", root, have_title ? title : "");
        if (have_title && is_dir(path)) {
            if (mt == 0)
                write_header(tpath, title);
        } else if (mu == 0) {
            write_header(upath, title);
        }
        if (have_title) {
            r = load_slot(SLOT_TITLE, tpath, 1);
            loaded += r > 0;
            bad |= r < 0;
        }
        r = load_slot(SLOT_USER, upath, 1);
        loaded += r > 0;
        bad |= r < 0;
        snprintf(s_user_path, sizeof s_user_path, "%s",
                 have_title && s_file[SLOT_TITLE] ? tpath : upath);
    }

    /* Lowest layer first, each file with its own count, so the layering is
     * visible in every run log. */
    fprintf(stderr, "[ENHANCE] config: ");
    for (i = ENHANCE_FILES - 1; i >= 0; i--)
        if (s_file[i]) {
            size_t n = recomp_cfg_count(s_file[i]);
            fprintf(stderr, "%s%s (%zu key%s)", listed++ ? ", " : "",
                    recomp_cfg_name(s_file[i]), n, n == 1 ? "" : "s");
        }
    fprintf(stderr, "%s\n", listed ? "" : "none");
env_lines:
    for (i = 0; i < s_n_binds; i++) {
        const char *v = recomp_env(s_binds[i].id);
        if (v && *v)
            fprintf(stderr, "[ENHANCE] %s = %s from %s\n", s_binds[i].key, v,
                    recomp_env_name(s_binds[i].id));
    }
    return bad ? -1 : loaded;
}

const char *enhance_cfg_user_path(void)
{
    return s_user_path[0] ? s_user_path : NULL;
}

const recomp_cfg *enhance_cfg_defaults(void)
{
    return s_file[SLOT_DEFAULTS];
}

const recomp_cfg *enhance_cfg_file(int i)
{
    return i >= 0 && i < ENHANCE_FILES ? s_file[i] : NULL;
}

/* The first file holding `key`: its slot, or -1. Marks the entry read. */
static int file_of(const char *key, recomp_cfg_type *type)
{
    int i;
    for (i = 0; i < ENHANCE_FILES; i++) {
        long k = recomp_cfg_find(s_file[i], key);
        if (k >= 0) {
            if (s_flags[i])
                s_flags[i][k] |= 1;
            *type = recomp_cfg_type_of(s_file[i], key);
            return i;
        }
    }
    return -1;
}

const recomp_cfg *enhance_cfg_lookup(const char *key)
{
    recomp_cfg_type t;
    int f = file_of(key, &t);
    return f < 0 ? NULL : s_file[f];
}

static void file_bad(int f, const char *key, const char *want)
{
    long k = recomp_cfg_find(s_file[f], key);
    if (!s_flags[f] || (s_flags[f][k] & 2))
        return;
    s_flags[f][k] |= 2;
    fprintf(stderr, "[ENHANCE] %s:%d: %s should be %s; ignored\n",
            recomp_cfg_name(s_file[f]), recomp_cfg_line(s_file[f], key), key, want);
}

/* ── lookups ───────────────────────────────────────────────────────── */

const char *enhance_cfg_string(const char *key, const char *def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v)
        return v;
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_STRING) {
        file_bad(f, key, "a string");
        return def;
    }
    return recomp_cfg_string(s_file[f], key, def);
}

long long enhance_cfg_int(const char *key, long long def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        char *end;
        long long n;
        errno = 0;
        n = strtoll(v, &end, 0);
        if (end != v && !*end && errno != ERANGE)
            return n;
        env_bad(b, v, "an integer");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_INT) {
        file_bad(f, key, "an integer");
        return def;
    }
    return recomp_cfg_int(s_file[f], key, def);
}

double enhance_cfg_float(const char *key, double def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        double d;
        if (recomp_cfg_parse_double(v, &d))
            return d;
        env_bad(b, v, "a number");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_FLOAT && t != RCFG_INT) {
        file_bad(f, key, "a number");
        return def;
    }
    return recomp_cfg_float(s_file[f], key, def);
}

static int word_is(const char *v, const char *w)
{
    for (; *v && *w; v++, w++)
        if (tolower((unsigned char)*v) != *w)
            return 0;
    return !*v && !*w;
}

int enhance_cfg_bool(const char *key, int def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    if (v) {
        if (word_is(v, "1") || word_is(v, "true") || word_is(v, "yes") || word_is(v, "on"))
            return 1;
        if (word_is(v, "0") || word_is(v, "false") || word_is(v, "no") || word_is(v, "off"))
            return 0;
        env_bad(b, v, "a bool (1/0, true/false, yes/no, on/off)");
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    if (t != RCFG_BOOL) {
        file_bad(f, key, "true or false");
        return def;
    }
    return recomp_cfg_bool(s_file[f], key, def);
}

int enhance_cfg_choice(const char *key, const char *const *choices, int def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    char want[256];
    size_t n = 0;
    int i, f;

    want[0] = 0;
    for (i = 0; choices && choices[i] && n + 1 < sizeof want; i++) {
        int w = snprintf(want + n, sizeof want - n, "%s%s", i ? "|" : "one of ", choices[i]);
        if (w < 0)
            break;
        n += (size_t)w;
    }
    if (v) {
        for (i = 0; choices && choices[i]; i++)
            if (!strcmp(choices[i], v))
                return i;
        env_bad(b, v, want);
    }
    f = file_of(key, &t);
    if (f < 0)
        return def;
    i = recomp_cfg_choice(s_file[f], key, choices, -1);
    if (i < 0) {
        file_bad(f, key, want);
        return def;
    }
    return i;
}

enhance_cfg_source enhance_cfg_source_of(const char *key)
{
    Bind *b;
    int i;
    if (env_of(key, &b))
        return ENHANCE_SRC_ENV;
    for (i = 0; i < ENHANCE_FILES; i++)
        if (recomp_cfg_find(s_file[i], key) >= 0)
            return i == SLOT_DEFAULTS ? ENHANCE_SRC_DEFAULTS : ENHANCE_SRC_FILE;
    return ENHANCE_SRC_DEFAULT;
}

int enhance_cfg_report_unused(void)
{
    int i, n = 0;
    size_t k;
    for (i = 0; i < ENHANCE_FILES; i++)
        for (k = 0; k < recomp_cfg_count(s_file[i]); k++) {
            const char *key = recomp_cfg_key_at(s_file[i], k);
            int used = s_flags[i] ? s_flags[i][k] & 1 : 1, j;
            /* A key a higher file overrides (the title file over the user
             * file, either over the defaults) is used even if this copy
             * was never read. */
            for (j = 0; !used && j < i; j++)
                used = recomp_cfg_find(s_file[j], key) >= 0;
            if (!used) {
                fprintf(stderr, "[ENHANCE] unused key %s, %s:%d\n", key,
                        recomp_cfg_name(s_file[i]), recomp_cfg_line(s_file[i], key));
                n++;
            }
        }
    return n;
}
