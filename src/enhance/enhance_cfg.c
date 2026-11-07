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

#define ENHANCE_MAX_BINDS 128
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

#define ENHANCE_MAX_CODE_DEFS 16

typedef struct {
    char key[64];
    char value[64];
} CodeDef;

static Bind s_binds[ENHANCE_MAX_BINDS];
/* enhance_cfg_set_code_default; not cleared by enhance_cfg_shutdown, as a
 * game sets it before the init that reloads the files. */
static CodeDef s_code_defs[ENHANCE_MAX_CODE_DEFS];
static int s_n_code_defs;
static int s_n_binds;
static int s_builtin_bound;

static recomp_cfg *s_file[ENHANCE_FILES];
/* Per file entry: bit 0 read by a lookup, bit 1 type warning printed. */
static unsigned char *s_flags[ENHANCE_FILES];
/* The path each slot is read from, set even when the file is absent, so a
 * writer's first Save can bring the new file in (enhance_cfg_patch). */
static char s_path[ENHANCE_FILES][CFG_PATH_MAX];
/* Copies a refresh replaced, with their flags. A caller may hold a string
 * or a file pointer from a lookup (the header lets it keep the values), and
 * a stray reader during a swap then marks the old flags, never freed
 * memory; so both live until enhance_cfg_shutdown, one pair per Save, a
 * few KB each. */
typedef struct {
    recomp_cfg *cfg;
    unsigned char *flags;
} Retired;
static Retired *s_retired;
static size_t s_n_retired, s_cap_retired;
/* The file a writer (the menu's Save) targets; empty under none. */
static char s_user_path[CFG_PATH_MAX];
static char s_user_dir[CFG_PATH_MAX];   /* enhance_cfg_user_dir */

static void bind_builtins(void)
{
    if (s_builtin_bound)
        return;
    s_builtin_bound = 1;
    /* Main-table keys: the kernel reads them without the enhance layer. */
    enhance_cfg_bind_env("storage.ram_cache", RENV_STORAGE_RAM_CACHE);
    enhance_cfg_bind_env("system.language", RENV_SYSTEM_LANGUAGE);
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    enhance_cfg_bind_env("render.scale", RENV_RENDER_SCALE);
    enhance_cfg_bind_env("render.aniso", RENV_RENDER_ANISO);
    enhance_cfg_bind_env("render.snap_2d", RENV_RENDER_SNAP_2D);
    enhance_cfg_bind_env("display.aspect", RENV_DISPLAY_ASPECT);
    enhance_cfg_bind_env("display.movie", RENV_DISPLAY_MOVIE);
    enhance_cfg_bind_env("display.hud_aspect", RENV_DISPLAY_HUD_ASPECT);
    enhance_cfg_bind_env("display.hud_margin", RENV_DISPLAY_HUD_MARGIN);
    enhance_cfg_bind_env("display.overscan", RENV_DISPLAY_OVERSCAN);
    enhance_cfg_bind_env("present.filter", RENV_PRESENT_FILTER);
    enhance_cfg_bind_env("present.fullscreen", RENV_PRESENT_FULLSCREEN);
    enhance_cfg_bind_env("present.pacing", RENV_PRESENT_PACING);
    enhance_cfg_bind_env("present.surface", RENV_PRESENT_SURFACE);
    enhance_cfg_bind_env("present.shader", RENV_PRESENT_SHADER);
    enhance_cfg_bind_env("present.shader_lib", RENV_PRESENT_SHADER_LIB);
    enhance_cfg_bind_env("present.profile", RENV_PRESENT_PROFILE);
    enhance_cfg_bind_env("textures.pack", RENV_TEXTURES_PACK);
    enhance_cfg_bind_env("textures.font", RENV_TEXTURES_FONT);
    enhance_cfg_bind_env("textures.text", RENV_TEXTURES_TEXT);
    enhance_cfg_bind_env("textures.world", RENV_TEXTURES_WORLD);
    enhance_cfg_bind_env("input.pad_square_left", RENV_INPUT_PAD_SQUARE_LEFT);
    enhance_cfg_bind_env("input.pad_square_right", RENV_INPUT_PAD_SQUARE_RIGHT);
    enhance_cfg_bind_env("overlay.stats", RENV_OVERLAY_STATS);
    enhance_cfg_bind_env("overlay.ui_scale", RENV_OVERLAY_UI_SCALE);
    enhance_cfg_bind_env("overlay.ui_fit", RENV_OVERLAY_UI_FIT);
    enhance_cfg_bind_env("menu.enabled", RENV_MENU);
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
    size_t r;
    for (i = 0; i < ENHANCE_FILES; i++) {
        recomp_cfg_free(s_file[i]);
        s_file[i] = NULL;
        free(s_flags[i]);
        s_flags[i] = NULL;
        s_path[i][0] = 0;
    }
    for (r = 0; r < s_n_retired; r++) {
        recomp_cfg_free(s_retired[r].cfg);
        free(s_retired[r].flags);
    }
    free(s_retired);
    s_retired = NULL;
    s_n_retired = s_cap_retired = 0;
    s_user_path[0] = 0;
    s_user_dir[0] = 0;
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
    snprintf(s_path[i], sizeof s_path[i], "%s", path);
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
        dir_of(over, s_user_dir, sizeof s_user_dir);
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
        snprintf(s_user_dir, sizeof s_user_dir, "%s/config", root);
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

static int grow_retired(void)
{
    size_t cap = s_cap_retired ? s_cap_retired * 2 : 8;
    Retired *r = (Retired *)realloc(s_retired, cap * sizeof *r);
    if (!r)
        return 0;
    s_retired = r;
    s_cap_retired = cap;
    return 1;
}

/* Read slot `i` again from its file after a write, keeping what the old
 * copy recorded per key (read, type warning printed): a key the run read
 * stays read, so the unused-key report does not turn on it. A file that no
 * longer parses keeps the old copy (the write itself parsed the text, so
 * only a hand edit in between gets here). 1 when the copy was replaced. */
static int refresh_slot(int i)
{
    char err[512];
    recomp_cfg *c, *old = s_file[i];
    unsigned char *fl;
    size_t k;
    if (!file_exists(s_path[i]))
        return 0;
    c = recomp_cfg_load_file(s_path[i], err, sizeof err);
    if (!c) {
        fprintf(stderr, "[ENHANCE] config error: %s (kept as loaded)\n", err);
        return 0;
    }
    fl = (unsigned char *)calloc(recomp_cfg_count(c) + 1, 1);
    if (!fl || (old && s_n_retired == s_cap_retired && !grow_retired())) {
        fprintf(stderr, "[ENHANCE] config: out of memory re-reading %s (kept as loaded)\n",
                s_path[i]);
        free(fl);
        recomp_cfg_free(c);
        return 0;
    }
    for (k = 0; old && s_flags[i] && k < recomp_cfg_count(old); k++) {
        long j = recomp_cfg_find(c, recomp_cfg_key_at(old, k));
        if (j >= 0)
            fl[j] = s_flags[i][k];
    }
    if (old) {
        s_retired[s_n_retired].cfg = old;
        s_retired[s_n_retired].flags = s_flags[i];
        s_n_retired++;
    } else {
        free(s_flags[i]);
    }
    s_file[i] = c;
    s_flags[i] = fl;
    return 1;
}

int enhance_cfg_patch(const char *path, const char *const *keys,
                      const char *const *values, size_t n, char *err, size_t errlen)
{
    int rc = recomp_cfg_patch(path, keys, values, n, err, errlen), i;
    size_t k;
    if (rc < 0 || !path || !n)
        return rc;
    /* The defaults ship read-only and are never a writer's target. */
    for (i = 0; i < SLOT_DEFAULTS; i++) {
        if (!s_path[i][0] || strcmp(s_path[i], path) || !refresh_slot(i))
            continue;
        /* A key the writer just set is in use, though the old copy never
         * held it: a first Save of a mod's key (config.toml starts as a
         * header) must not list it under the Mods page's unknown keys. */
        for (k = 0; k < n; k++) {
            long j = values[k] ? recomp_cfg_find(s_file[i], keys[k]) : -1;
            if (j >= 0)
                s_flags[i][j] |= 1;
        }
    }
    return rc;
}

const char *enhance_cfg_user_path(void)
{
    return s_user_path[0] ? s_user_path : NULL;
}

int enhance_cfg_user_dir(char *out, size_t n)
{
    if (!s_user_dir[0] || !n)
        return 0;
    snprintf(out, n, "%s", s_user_dir);
    return 1;
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

/* ── code defaults ─────────────────────────────────────────────────── */

int enhance_cfg_set_code_default(const char *key, const char *value)
{
    int i;
    if (!key || strlen(key) >= sizeof s_code_defs[0].key
        || (value && strlen(value) >= sizeof s_code_defs[0].value))
        return 0;
    for (i = 0; i < s_n_code_defs; i++)
        if (!strcmp(s_code_defs[i].key, key))
            break;
    if (!value) {
        if (i < s_n_code_defs)
            s_code_defs[i] = s_code_defs[--s_n_code_defs];
        return 1;
    }
    if (i == s_n_code_defs) {
        if (s_n_code_defs == ENHANCE_MAX_CODE_DEFS)
            return 0;
        s_n_code_defs++;
        snprintf(s_code_defs[i].key, sizeof s_code_defs[i].key, "%s", key);
    }
    snprintf(s_code_defs[i].value, sizeof s_code_defs[i].value, "%s", value);
    return 1;
}

const char *enhance_cfg_code_default(const char *key)
{
    int i;
    for (i = 0; key && i < s_n_code_defs; i++)
        if (!strcmp(s_code_defs[i].key, key))
            return s_code_defs[i].value;
    return NULL;
}

/* The code default parsed as a file value of the type would be: a bool is
 * true or false, an integer a whole C literal (as the env tier reads one),
 * a choice one of its names. 1 when `key` has one that converts. */
int enhance_cfg_code_default_bool(const char *key, int *out)
{
    const char *c = enhance_cfg_code_default(key);
    if (c && (!strcmp(c, "true") || !strcmp(c, "false"))) {
        *out = c[0] == 't';
        return 1;
    }
    return 0;
}

int enhance_cfg_code_default_int(const char *key, long long *out)
{
    const char *c = enhance_cfg_code_default(key);
    char *end;
    long long n;
    if (!c)
        return 0;
    errno = 0;
    n = strtoll(c, &end, 0);
    if (end == c || *end || errno == ERANGE)
        return 0;
    *out = n;
    return 1;
}

int enhance_cfg_code_default_float(const char *key, double *out)
{
    const char *c = enhance_cfg_code_default(key);
    return c && recomp_cfg_parse_double(c, out);
}

int enhance_cfg_code_default_choice(const char *key, const char *const *choices, int *out)
{
    const char *c = enhance_cfg_code_default(key);
    int i;
    for (i = 0; c && choices && choices[i]; i++)
        if (!strcmp(choices[i], c)) {
            *out = i;
            return 1;
        }
    return 0;
}

/* The caller's default, replaced by the game's code default when one is
 * set and converts. */
static const char *def_string(const char *key, const char *def)
{
    const char *c = enhance_cfg_code_default(key);
    return c ? c : def;
}

static long long def_int(const char *key, long long def)
{
    enhance_cfg_code_default_int(key, &def);
    return def;
}

static double def_float(const char *key, double def)
{
    enhance_cfg_code_default_float(key, &def);
    return def;
}

static int def_bool(const char *key, int def)
{
    enhance_cfg_code_default_bool(key, &def);
    return def;
}

static int def_choice(const char *key, const char *const *choices, int def)
{
    enhance_cfg_code_default_choice(key, choices, &def);
    return def;
}

/* ── lookups ───────────────────────────────────────────────────────── */

const char *enhance_cfg_string(const char *key, const char *def)
{
    Bind *b;
    recomp_cfg_type t;
    const char *v = env_of(key, &b);
    int f;
    def = def_string(key, def);
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
    def = def_int(key, def);
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
    def = def_float(key, def);
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
    def = def_bool(key, def);
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

    def = def_choice(key, choices, def);
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

static void mark_read(int f, const char *key)
{
    long k = recomp_cfg_find(s_file[f], key);
    if (k >= 0 && s_flags[f])
        s_flags[f][k] |= 1;
}

static int hit(struct enhance_cfg_hit *out, int n, int max, int f, const char *key, int row)
{
    if (n < max && recomp_cfg_find(s_file[f], key) >= 0) {
        out[n].file = s_file[f];
        out[n].key = key;
        out[n].row = row;
        n++;
    }
    return n;
}

int enhance_cfg_layered(const char *key, const char *const *aliases,
                        struct enhance_cfg_hit *out, int max)
{
    int n = 0, f;
    size_t a;
    if (!key)
        return 0;
    for (f = 0; f < ENHANCE_FILES; f++) {
        mark_read(f, key);
        for (a = 0; aliases && aliases[a]; a++)
            mark_read(f, aliases[a]);
    }
    for (f = SLOT_TITLE; f <= SLOT_USER; f++)
        n = hit(out, n, max, f, key, ENHANCE_ROW_USER);
    for (a = 0; aliases && aliases[a]; a++)
        for (f = SLOT_TITLE; f <= SLOT_USER; f++)
            n = hit(out, n, max, f, aliases[a], ENHANCE_ROW_USER_ALIAS);
    n = hit(out, n, max, SLOT_DEFAULTS, key, ENHANCE_ROW_DEFAULTS);
    for (a = 0; aliases && aliases[a]; a++)
        n = hit(out, n, max, SLOT_DEFAULTS, aliases[a], ENHANCE_ROW_DEFAULTS_ALIAS);
    return n;
}

const char *enhance_cfg_env(const char *key)
{
    Bind *b;
    return key ? env_of(key, &b) : NULL;
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

void enhance_cfg_mark_read_prefix(const char *prefix)
{
    size_t n = prefix ? strlen(prefix) : 0, k;
    int i;
    if (!n)
        return;
    for (i = 0; i < ENHANCE_FILES; i++)
        for (k = 0; s_flags[i] && k < recomp_cfg_count(s_file[i]); k++)
        {
            /* Only at a boundary: mod "fx" owns fx.glow and fx_speed but
             * not fxaa, which stays a typo the report can name. */
            const char *key = recomp_cfg_key_at(s_file[i], k);
            if (!strncmp(key, prefix, n) && (!key[n] || key[n] == '.' || key[n] == '_'))
                s_flags[i][k] |= 1;
        }
}

/* The mods' ids, for naming a stray key's owner (mod-settings B.11). */
#define OWNERS_MAX 64
static char s_owner[OWNERS_MAX][64];
static int s_nowner;

void enhance_cfg_owner_add(const char *id)
{
    if (!id) {
        s_nowner = 0;
        return;
    }
    if (s_nowner < OWNERS_MAX && *id && strlen(id) < sizeof s_owner[0])
        snprintf(s_owner[s_nowner++], sizeof s_owner[0], "%s", id);
}

const char *enhance_cfg_owner_of(const char *key)
{
    static const char *const pre[] = { "mods.", "input.controls.", "input.actions." };
    const char *best = NULL;
    size_t bn = 0, n, k;
    int i;

    if (!key)
        return NULL;
    /* A mod's keys sit under its tables or at its own prefix
     * ("camera.free_speed"); the boundary rule is mark_read_prefix's. */
    for (k = 0; k < sizeof pre / sizeof pre[0]; k++)
        if (!strncmp(key, pre[k], strlen(pre[k]))) {
            key += strlen(pre[k]);
            break;
        }
    for (i = 0; i < s_nowner; i++) {
        n = strlen(s_owner[i]);
        if (n > bn && !strncmp(key, s_owner[i], n) &&
            (!key[n] || key[n] == '.' || key[n] == '_')) {
            best = s_owner[i];
            bn = n;
        }
    }
    return best;
}

/* A key nothing read, in no higher file: what the report lists. */
static int unused_at(int i, size_t k)
{
    const char *key = recomp_cfg_key_at(s_file[i], k);
    int used = s_flags[i] ? s_flags[i][k] & 1 : 1, j;
    /* A key a higher file overrides (the title file over the user file,
     * either over the defaults) is used even if this copy was never read. */
    for (j = 0; !used && j < i; j++)
        used = recomp_cfg_find(s_file[j], key) >= 0;
    return !used;
}

int enhance_cfg_unused_of(const char *owner, const char **keys, int max)
{
    const char *o;
    int i, n = 0;
    size_t k;

    for (i = 0; owner && i < ENHANCE_FILES; i++)
        for (k = 0; k < recomp_cfg_count(s_file[i]); k++) {
            const char *key = recomp_cfg_key_at(s_file[i], k);
            if (!unused_at(i, k) || !(o = enhance_cfg_owner_of(key)) || strcmp(o, owner))
                continue;
            if (n < max && keys)
                keys[n] = key;
            n++;
        }
    return n < max ? n : max;
}

int enhance_cfg_report_unused(void)
{
    int i, n = 0;
    size_t k;
    for (i = 0; i < ENHANCE_FILES; i++)
        for (k = 0; k < recomp_cfg_count(s_file[i]); k++) {
            const char *key = recomp_cfg_key_at(s_file[i], k), *o;
            if (!unused_at(i, k))
                continue;
            o = enhance_cfg_owner_of(key);
            if (o)
                fprintf(stderr, "[ENHANCE] unused key %s, %s:%d (mod %s)\n", key,
                        recomp_cfg_name(s_file[i]), recomp_cfg_line(s_file[i], key), o);
            else
                fprintf(stderr, "[ENHANCE] unused key %s, %s:%d\n", key,
                        recomp_cfg_name(s_file[i]), recomp_cfg_line(s_file[i], key));
            n++;
        }
    return n;
}
