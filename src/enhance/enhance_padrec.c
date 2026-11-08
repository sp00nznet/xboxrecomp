/*
 * enhance_padrec.c - pad recordings from the menu and the shortcuts, and
 * their strip (enhance_padrec.h, pad-recording-ui R3, R4, R6).
 */
#include "enhance_padrec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#include <sys/utime.h>
#else
#include <utime.h>
#endif

#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_menu.h"
#include "enhance_mod_status.h"
#include "enhance_text.h"
#include "file_browser.h"
#include "host_pad.h"
#include "kernel.h"
#include "mod.h"
#include "nv2a_overlay.h"
#include "nv2a_overlay_int.h"
#include "pad_script.h"
#include "recomp_exe_dir.h"
#include "shader_profile.h"

#define LOGP "[PADREC] "
#define ENDED_MS 5000u      /* R4: a finished playback's strip stays 5 s */
#define REFRESH_MS 100u     /* the strip's clock moves ten times a second */

static const char *const k_clock[] = { "auto", "poll", "flip", NULL };
static const char *const k_start[] = { "menu_close", "now", NULL };
static const char *const k_hud[] = { "auto", "off", NULL };

static int s_on_close = 1;
static int s_hud = PADREC_HUD_AUTO;
static char s_last[128];

/* The mod a requires key switches: its descriptor's enable setting
 * (mods.debug_menu.enable, an "off"/"on" choice that enhance_cfg_bool
 * would refuse as not a bool) or its mods.<name> key. -1 for any other key;
 * *idx the enable setting's index, -1 for mods.<name>. */
static int mod_of_key(const char *key, int *idx)
{
    int i, e;
    for (i = 0; i < xbox_mod_count(); i++) {
        const char *name = xbox_mod_name(i), *k;
        e = xbox_mod_enable_setting(i);
        k = e >= 0 ? xbox_mod_setting_key(i, (uint32_t)e) : NULL;
        if (k && !strcmp(k, key)) {
            *idx = e;
            return i;
        }
        if (name && !strncmp(key, "mods.", 5) && !strcmp(key + 5, name)) {
            *idx = -1;
            return i;
        }
    }
    return -1;
}

/* A file's requires key is on in this run. A mod's switch counts only
 * while the mod runs: a !<action> of a mod switched on for the next start
 * would be refused at its step. Any other key is a bool of the config. */
static int requires_on(const char *key)
{
    int idx, m = mod_of_key(key, &idx);
    if (m >= 0)
        return xbox_mod_enabled(m) && xbox_mod_state(m) == XBOX_MOD_ON;
    return enhance_cfg_bool(key, 0);
}

void enhance_padrec_start(void)
{
    int c = enhance_cfg_choice("padrec.clock", k_clock, 0);
    __atomic_store_n(&s_on_close, enhance_cfg_choice("padrec.start", k_start, 0) == 0,
                     __ATOMIC_RELAXED);
    __atomic_store_n(&s_hud, enhance_cfg_choice("overlay.padrec_hud", k_hud, PADREC_HUD_AUTO),
                     __ATOMIC_RELAXED);
    /* The env key is read by enhance_cfg too (bound), so the session takes
     * the resolved value and never reads RECOMP_PADREC_CLOCK past it. */
    padrec_set_clock_key(k_clock[c]);
    /* !<action> steps fire the actions a mod made pad-fireable, and a
     * record writes their fires (R5); requires reads the config. */
    padrec_set_action_hook(enhance_action_fire_by_pad);
    enhance_actions_set_fired_hook(padrec_action_fired);
    padrec_set_requires_hook(requires_on);
}

int enhance_padrec_start_on_close(void)
{
    return __atomic_load_n(&s_on_close, __ATOMIC_RELAXED);
}

static int choice_of(const char *const *k, const char *v)
{
    for (int i = 0; v && k[i]; i++)
        if (!strcmp(k[i], v))
            return i;
    return -1;
}

void enhance_padrec_set_key(const char *key, const char *v)
{
    int c;
    if (!key || !v)
        return;
    if (!strcmp(key, "padrec.start") && (c = choice_of(k_start, v)) >= 0)
        __atomic_store_n(&s_on_close, c == 0, __ATOMIC_RELAXED);
    else if (!strcmp(key, "overlay.padrec_hud") && (c = choice_of(k_hud, v)) >= 0)
        __atomic_store_n(&s_hud, c, __ATOMIC_RELAXED);
    else if (!strcmp(key, "padrec.clock") && (c = choice_of(k_clock, v)) >= 0)
        padrec_set_clock_key(k_clock[c]);  /* the next session's pick */
}

const char *enhance_padrec_last(void)
{
    return s_last;
}

int enhance_padrec_dir(char *out, size_t n)
{
    char base[FB_PATH_MAX];
    if (!enhance_cfg_user_dir(base, sizeof base))
        return 0;
    return fb_path_join(base, "recordings", out, n) == FB_OK;
}

/* ---- the folder --------------------------------------------------------- */

static int64_t file_mtime(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    struct _stat64 st;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, FB_PATH_MAX) || _wstat64(w, &st) != 0)
        return 0;
    return (int64_t)st.st_mtime;
#else
    struct stat st;
    return stat(path, &st) == 0 ? (int64_t)st.st_mtime : 0;
#endif
}

/* A rename writes a new file: it keeps the old one's time, which the list
 * sorts and dates by. */
static void file_set_mtime(const char *path, int64_t t)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    struct __utimbuf64 u;
    u.actime = u.modtime = (__time64_t)t;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, w, FB_PATH_MAX))
        _wutime64(w, &u);
#else
    struct utimbuf u;
    u.actime = u.modtime = (time_t)t;
    utime(path, &u);
#endif
}

struct list_ctx {
    struct enhance_padrec_file *out;
    int max, n;
    const char *dir;
};

static int ends_with_pad(const char *name)
{
    size_t n = strlen(name);
    const char *e;
    if (n < 5)
        return 0;
    e = name + n - 4;
    return e[0] == '.' && (e[1] | 32) == 'p' && (e[2] | 32) == 'a' && (e[3] | 32) == 'd';
}

static int list_emit(void *ctx, const struct fb_entry *e)
{
    struct list_ctx *c = ctx;
    char path[FB_PATH_MAX];
    if (e->is_dir || e->hidden || e->name[0] == '.' || !ends_with_pad(e->name))
        return 0;
    int64_t t;
    int at = c->n;
    if (strlen(e->name) >= sizeof c->out[0].name || c->max <= 0)
        return 0;
    t = fb_path_join(c->dir, e->name, path, sizeof path) == FB_OK ? file_mtime(path) : 0;
    /* Full: the newest max are kept, so the oldest gives way. */
    if (c->n >= c->max) {
        at = 0;
        for (int i = 1; i < c->n; i++)
            if (c->out[i].mtime < c->out[at].mtime)
                at = i;
        if (c->out[at].mtime >= t)
            return 0;
    } else {
        c->n++;
    }
    snprintf(c->out[at].name, sizeof c->out[0].name, "%s", e->name);
    c->out[at].mtime = t;
    return 0;
}

static int newest_first(const void *a, const void *b)
{
    const struct enhance_padrec_file *x = a, *y = b;
    if (x->mtime != y->mtime)
        return x->mtime < y->mtime ? 1 : -1;
    return strcmp(y->name, x->name);
}

int enhance_padrec_list(struct enhance_padrec_file *out, int max)
{
    char dir[FB_PATH_MAX];
    struct list_ctx c = { out, max, 0, dir };
    int rc;
    if (!enhance_padrec_dir(dir, sizeof dir))
        return -1;
    if (!fb_exists(dir))
        return 0;
    rc = fb_list_host(dir, list_emit, &c, NULL);
    if (rc != 0)
        return -1;
    qsort(out, (size_t)c.n, sizeof *out, newest_first);
    return c.n;
}

/* ---- starting ------------------------------------------------------------ */

/* The host side of a rename and a delete, UTF-8 paths on Windows too. */
static int host_rename(const char *from, const char *to)
{
#ifdef _WIN32
    wchar_t wf[FB_PATH_MAX], wt[FB_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, from, -1, wf, FB_PATH_MAX)
        || !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, to, -1, wt, FB_PATH_MAX))
        return -1;
    /* No REPLACE_EXISTING: the caller checked the name is free or is this
     * file under another case, which MoveFileExW renames in place. */
    return MoveFileExW(wf, wt, 0) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

static int host_remove(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, w, FB_PATH_MAX))
        return -1;
    return DeleteFileW(w) ? 0 : -1;
#else
    return remove(path);
#endif
}

/* a and b name one file: a case-only rename on a case-insensitive volume
 * (macOS, Windows), where the new name already "exists". */
static int same_file(const char *a, const char *b)
{
#ifdef _WIN32
    /* NTFS has no inode to compare. Both paths share the recordings
     * folder, so the same name under another (ASCII) case is the same file
     * there; any other taken name is refused before anything is written.
     * In a folder a user made case-sensitive, MoveFileExW then fails
     * rather than replacing. */
    return _stricmp(a, b) == 0;
#else
    struct stat sa, sb;
    return stat(a, &sa) == 0 && stat(b, &sb) == 0 && sa.st_dev == sb.st_dev
           && sa.st_ino == sb.st_ino;
#endif
}

void enhance_padrec_default_stem(time_t t, char *out, size_t n)
{
    char slug[33];
    if (!shader_profile_slug(xbox_path_title_name(), slug, sizeof slug))
        snprintf(slug, sizeof slug, "pad");
    fb_default_name(slug, t, "", out, n);
}

/* The folder made, and a file's path in it. 0, or -1 with why. */
static int file_path(const char *name, char *path, size_t pn, char *why, size_t n)
{
    char dir[FB_PATH_MAX];
    if (!enhance_padrec_dir(dir, sizeof dir)) {
        snprintf(why, n, "No user folder (RECOMP_CONFIG=none)");
        return -1;
    }
    if (!name || !name[0] || fb_path_join(dir, name, path, pn) != FB_OK) {
        snprintf(why, n, "No such recording");
        return -1;
    }
    return 0;
}

/* The file a record is writing is never renamed, deleted or exported
 * half-written: s_last names it while the session runs. Under an env key
 * the page is read only (env set): an export still reads. */
static int busy(const char *name, int env, char *why, size_t n)
{
    PadrecStatus st;
    const char *key = env ? padrec_env_driven() : NULL;
    if (key) {
        snprintf(why, n, "Driven by %s", key);
        return 1;
    }
    padrec_status(&st);
    if ((st.state == PADREC_REC || st.state == PADREC_ARMED_REC) && !strcmp(name, s_last)) {
        snprintf(why, n, "Stop the record first");
        return 1;
    }
    return 0;
}

static int read_named(const char *name, char **text, size_t *len, char *why, size_t n)
{
    char path[FB_PATH_MAX];
    int rc;
    if (file_path(name, path, sizeof path, why, n) < 0)
        return -1;
    rc = fb_read_file(path, (size_t)PAD_FILE_MAX, text, len);
    if (rc == FB_E_TOO_BIG)
        snprintf(why, n, "Over %ld MB", PAD_FILE_MAX >> 20);
    else if (rc != FB_OK)
        snprintf(why, n, "%s cannot be read", name);
    return rc == FB_OK ? 0 : -1;
}

int enhance_padrec_check(const char *text, size_t len, PadFileInfo *info, char *why, size_t n)
{
    return pad_file_check(text, len, xbox_path_title_id(), nv2a_overlay_game(), info, why, n);
}

int enhance_padrec_import(const char *src_name, const char *text, size_t len, char *out,
                          size_t on, char *why, size_t n)
{
    char dir[FB_PATH_MAX], stem[FB_ENTRY_NAME_MAX];
    size_t sl;
    int rc;
    const char *env = padrec_env_driven();

    if (env) {
        snprintf(why, n, "Driven by %s", env);
        return -1;
    }
    if (!enhance_padrec_dir(dir, sizeof dir)) {
        snprintf(why, n, "No user folder (RECOMP_CONFIG=none)");
        return -1;
    }
    if (fb_mkdirs(dir) != 0) {
        snprintf(why, n, "The recordings folder cannot be made");
        return -1;
    }
    snprintf(stem, sizeof stem, "%s", src_name ? src_name : "");
    sl = strlen(stem);
    if (ends_with_pad(stem))
        stem[sl - 4] = 0;
    rc = fb_import_unique(dir, stem, ".pad", text, len, out, on);
    /* A name the browser listed but fb_name_check refuses (a reserved name
     * made on Linux, a long one): the file still comes in, named as a new
     * record is. */
    if (rc == FB_E_NAME) {
        enhance_padrec_default_stem(time(NULL), stem, sizeof stem);
        rc = fb_import_unique(dir, stem, ".pad", text, len, out, on);
    }
    if (rc != FB_OK) {
        snprintf(why, n, "It cannot be copied into the recordings folder");
        return -1;
    }
    return 0;
}

int enhance_padrec_read(const char *name, char **text, size_t *len, char *why, size_t n)
{
    if (busy(name, 0, why, n))
        return -1;
    return read_named(name, text, len, why, n);
}

int enhance_padrec_info(const char *name, PadFileInfo *info, char *why, size_t n)
{
    char *text = NULL;
    size_t len = 0;
    int rc;
    if (read_named(name, &text, &len, why, n) < 0)
        return -1;
    /* Title 0: Info shows another game's file as it is, to say why it was
     * refused or what it holds. */
    rc = pad_file_check(text, len, 0, NULL, info, why, n);
    free(text);
    return rc;
}

int enhance_padrec_rename(const char *name, const char *typed, char *out, size_t on,
                          char *why, size_t n)
{
    char from[FB_PATH_MAX], to[FB_PATH_MAX], nn[FB_NAME_MAX + FB_EXT_MAX + 1];
    char *text = NULL, *next = NULL;
    const char *reason = NULL;
    size_t len = 0;
    PadHeader *h = NULL;
    int64_t when;
    int rc = -1, same;

    if (busy(name, 1, why, n))
        return -1;
    if (fb_name_check(typed, ".pad", nn, sizeof nn, &reason)) {
        snprintf(why, n, "%s", reason ? reason : "Not a file name");
        return -1;
    }
    if (!strcmp(nn, name)) {
        snprintf(out, on, "%s", name);
        return 0;
    }
    if (file_path(name, from, sizeof from, why, n) < 0
        || file_path(nn, to, sizeof to, why, n) < 0)
        return -1;
    same = fb_exists(to) && same_file(from, to);
    if (fb_exists(to) && !same) {
        snprintf(why, n, "%s is taken", nn);
        return -1;
    }
    if (read_named(name, &text, &len, why, n) < 0)
        return -1;
    when = file_mtime(from);
    /* The header's name is what the strip and the page's "playing" show,
     * so it follows the file's. A file with no header (an env record) is
     * moved as it is. */
    if (!(h = malloc(sizeof *h))) {
        snprintf(why, n, "Out of memory");
        goto out;
    }
    if (pad_header_read(text, h) == PAD_HDR_OK && h->present) {
        char stem[sizeof nn];
        snprintf(stem, sizeof stem, "%.*s", (int)(strlen(nn) - 4), nn);
        pad_header_set_text(h, "name", stem);
        if (!(next = pad_header_replace(text, h))) {
            snprintf(why, n, "Out of memory");
            goto out;
        }
    }
    if (same) {
        /* One file under two cases: rewrite it in place, then rename. */
        if (fb_write_atomic(from, next ? next : text, next ? strlen(next) : len) != FB_OK
            || host_rename(from, to) != 0) {
            snprintf(why, n, "%s cannot be renamed", name);
            goto out;
        }
        if (when)
            file_set_mtime(to, when);
    } else {
        int w = fb_write_new(to, next ? next : text, next ? strlen(next) : len);
        if (w == FB_E_EXISTS) {
            snprintf(why, n, "%s is taken", nn);
            goto out;
        }
        if (w != FB_OK) {
            snprintf(why, n, "%s cannot be written", nn);
            goto out;
        }
        if (when)
            file_set_mtime(to, when);
        /* Both names for a moment, never neither: the old goes once the
         * new one is whole. */
        if (host_remove(from) != 0) {
            host_remove(to);
            snprintf(why, n, "%s cannot be renamed", name);
            goto out;
        }
    }
    if (!strcmp(s_last, name))
        snprintf(s_last, sizeof s_last, "%s", nn);
    padrec_path_moved(from, to);            /* Play again follows */
    snprintf(out, on, "%s", nn);
    rc = 0;
out:
    free(h);
    free(next);
    free(text);
    return rc;
}

int enhance_padrec_delete(const char *name, char *why, size_t n)
{
    char path[FB_PATH_MAX];
    if (busy(name, 1, why, n) || file_path(name, path, sizeof path, why, n) < 0)
        return -1;
    if (host_remove(path) != 0) {
        snprintf(why, n, "%s cannot be deleted", name);
        return -1;
    }
    if (!strcmp(s_last, name))
        s_last[0] = 0;
    padrec_path_moved(path, NULL);
    return 0;
}

/* A new record's file in the folder: "<game>-<yyyymmdd-hhmm>", then -2,
 * -3 within the minute. The empty file takes the name as an import does
 * (R5): two records in one minute, or names apart only by case on a
 * case-insensitive volume, never share a file. Its name and path, and the
 * header's name (the stem). 0, or -1 with why. */
static int record_file(char *name, size_t nn, char *path, size_t pn, char *stem, size_t sn,
                       char *why, size_t n)
{
    char dir[FB_PATH_MAX];
    if (!enhance_padrec_dir(dir, sizeof dir)) {
        snprintf(why, n, "No user folder (RECOMP_CONFIG=none)");
        return -1;
    }
    if (fb_mkdirs(dir) != 0) {
        snprintf(why, n, "The recordings folder cannot be made");
        return -1;
    }
    enhance_padrec_default_stem(time(NULL), stem, sn);
    if (fb_import_unique(dir, stem, ".pad", "", 0, name, nn) != FB_OK
        || fb_path_join(dir, name, path, pn) != FB_OK) {
        snprintf(why, n, "No free file name in the recordings folder");
        return -1;
    }
    /* The header's name is the stem: the strip and the page's "playing"
     * match compare it with the file name up to its extension. */
    snprintf(stem, sn, "%.*s", (int)(strlen(name) - 4), name);
    return 0;
}

int enhance_padrec_record(int armed, const char *start_point, char *why, size_t n)
{
    char name[FB_NAME_MAX + FB_EXT_MAX + 1], stem[FB_NAME_MAX + 1], path[FB_PATH_MAX];
    PadrecMeta m;
    const char *env = padrec_env_driven();

    if (env) {
        snprintf(why, n, "Driven by %s", env);
        return -1;
    }
    if (record_file(name, sizeof name, path, sizeof path, stem, sizeof stem, why, n) < 0)
        return -1;
    memset(&m, 0, sizeof m);
    m.title = nv2a_overlay_game();
    m.title_id = xbox_path_title_id();
    m.name = stem;
    m.start_point = start_point;
    if (padrec_record_start(path, &m, armed ? PADREC_ARMED : 0, why, n) < 0) {
        host_remove(path);
        return -1;
    }
    snprintf(s_last, sizeof s_last, "%s", name);
    return 0;
}

int enhance_padrec_play_file(const char *name, int armed, char *why, size_t n)
{
    char dir[FB_PATH_MAX], path[FB_PATH_MAX];
    const char *one[1];
    const char *env = padrec_env_driven();

    if (env) {
        snprintf(why, n, "Driven by %s", env);
        return -1;
    }
    if (!name || !name[0]) {
        snprintf(why, n, "No recording yet");
        return -1;
    }
    if (!enhance_padrec_dir(dir, sizeof dir) || fb_path_join(dir, name, path, sizeof path) != FB_OK) {
        snprintf(why, n, "No user folder (RECOMP_CONFIG=none)");
        return -1;
    }
    one[0] = path;
    padrec_set_title(xbox_path_title_id(), nv2a_overlay_game());
    if (padrec_play(one, 1, armed ? PADREC_ARMED : 0, why, n) < 0)
        return -1;
    if (name != s_last)
        snprintf(s_last, sizeof s_last, "%s", name);
    return 0;
}

int enhance_padrec_again(int armed, char *why, size_t n)
{
    padrec_set_title(xbox_path_title_id(), nv2a_overlay_game());
    return padrec_again(armed ? PADREC_ARMED : 0, why, n);
}

/* ---- setups and chains (R6) ---------------------------------------------- */

int enhance_padrec_setups_dir(char *out, size_t n)
{
    snprintf(out, n, "%s/pad-setups", recomp_exe_dir());
    if (fb_exists(out))
        return 1;
#ifdef __APPLE__
    /* A .app keeps what the CLI stages beside the exe in Resources. */
    snprintf(out, n, "%s/../Resources/pad-setups", recomp_exe_dir());
    if (fb_exists(out))
        return 1;
#endif
    return 0;
}

struct setup_ctx {
    struct enhance_padrec_setup *out;
    int max, n, shipped;
    const char *dir;
};

/* A recording whose header says kind = "setup". Only the header is
 * needed, but the file is read whole (at most PAD_FILE_MAX): the folder
 * holds the player's own files, and this runs on the page's request. */
static int user_setup(const char *path)
{
    char *text = NULL;
    size_t len = 0;
    PadHeader *h;
    const char *k;
    int yes = 0;
    if (fb_read_file(path, (size_t)PAD_FILE_MAX, &text, &len) != FB_OK)
        return 0;
    h = malloc(sizeof *h);
    if (h && pad_header_read(text, h) == PAD_HDR_OK && (k = pad_header_get(h, "kind")))
        yes = !strcmp(k, "setup");
    free(h);
    free(text);
    return yes;
}

static int setup_emit(void *ctx, const struct fb_entry *e)
{
    struct setup_ctx *c = ctx;
    char path[FB_PATH_MAX];
    if (e->is_dir || e->hidden || e->name[0] == '.' || !ends_with_pad(e->name)
        || strlen(e->name) >= sizeof c->out[0].name || c->n >= c->max)
        return 0;
    if (!c->shipped
        && (fb_path_join(c->dir, e->name, path, sizeof path) != FB_OK || !user_setup(path)))
        return 0;
    snprintf(c->out[c->n].name, sizeof c->out[0].name, "%s", e->name);
    c->out[c->n].shipped = c->shipped;
    c->n++;
    return 0;
}

static int setup_by_name(const void *a, const void *b)
{
    const struct enhance_padrec_setup *x = a, *y = b;
    if (x->shipped != y->shipped)
        return y->shipped - x->shipped;
    return strcmp(x->name, y->name);
}

int enhance_padrec_setups(struct enhance_padrec_setup *out, int max)
{
    char dir[FB_PATH_MAX];
    struct setup_ctx c = { out, max, 0, 1, dir };
    if (max <= 0)
        return 0;
    if (enhance_padrec_setups_dir(dir, sizeof dir))
        fb_list_host(dir, setup_emit, &c, NULL);
    c.shipped = 0;
    if (enhance_padrec_dir(dir, sizeof dir) && fb_exists(dir))
        fb_list_host(dir, setup_emit, &c, NULL);
    qsort(out, (size_t)c.n, sizeof *out, setup_by_name);
    return c.n;
}

static int setup_path(const struct enhance_padrec_setup *s, char *path, size_t pn, char *why,
                      size_t n)
{
    char dir[FB_PATH_MAX];
    if (!s->shipped)
        return file_path(s->name, path, pn, why, n);
    if (!enhance_padrec_setups_dir(dir, sizeof dir)
        || fb_path_join(dir, s->name, path, pn) != FB_OK) {
        snprintf(why, n, "The setup %s is gone", s->name);
        return -1;
    }
    return 0;
}

int enhance_padrec_read_setup(const struct enhance_padrec_setup *s, char **text, size_t *len,
                              char *why, size_t n)
{
    char path[FB_PATH_MAX];
    int rc;
    if (setup_path(s, path, sizeof path, why, n) < 0)
        return -1;
    rc = fb_read_file(path, (size_t)PAD_FILE_MAX, text, len);
    if (rc == FB_E_TOO_BIG)
        snprintf(why, n, "Over %ld MB", PAD_FILE_MAX >> 20);
    else if (rc != FB_OK)
        snprintf(why, n, "%s cannot be read", s->name);
    return rc == FB_OK ? 0 : -1;
}

/* Append the !<action> names of the file at path to out (", " joined,
 * each once). The names added; -1 when it cannot be read. */
static int actions_of(const char *path, char *out, size_t on)
{
    char names[PAD_ACTION_MAX][PAD_ACTION_NAME];
    char *text = NULL;
    size_t len = 0;
    int k, added = 0;
    if (fb_read_file(path, (size_t)PAD_FILE_MAX, &text, &len) != FB_OK)
        return -1;
    k = pad_script_action_names(text, names, PAD_ACTION_MAX);
    free(text);
    for (int i = 0; i < k && i < PAD_ACTION_MAX; i++) {
        size_t at = strlen(out), nl = strlen(names[i]);
        const char *p = out;
        int dup = 0;
        /* Once each across the chain: a name already listed is skipped. */
        while ((p = strstr(p, names[i])) != NULL) {
            if ((p == out || p[-1] == ' ') && (p[nl] == ',' || !p[nl])) {
                dup = 1;
                break;
            }
            p += nl;
        }
        if (dup)
            continue;
        snprintf(out + at, on > at ? on - at : 0, "%s%s", at ? ", " : "", names[i]);
        added++;
    }
    return added;
}

int enhance_padrec_chain_actions(const struct enhance_padrec_setup *setup, const char *name,
                                 char *out, size_t on, char *why, size_t n)
{
    char path[FB_PATH_MAX];
    int k, total = 0;
    if (on)
        out[0] = 0;
    if (setup && !setup->shipped) {
        if (setup_path(setup, path, sizeof path, why, n) < 0)
            return -1;
        if ((k = actions_of(path, out, on)) < 0) {
            snprintf(why, n, "%s cannot be read", setup->name);
            return -1;
        }
        total += k;
    }
    if (name && name[0]) {
        if (file_path(name, path, sizeof path, why, n) < 0)
            return -1;
        if ((k = actions_of(path, out, on)) < 0) {
            snprintf(why, n, "%s cannot be read", name);
            return -1;
        }
        total += k;
    }
    return total;
}

int enhance_padrec_play_chain(const struct enhance_padrec_setup *setup, const char *name,
                              int record, const char *start_point, int armed,
                              const char *confirmed, char *why, size_t n)
{
    char paths[2][FB_PATH_MAX], rec[FB_PATH_MAX];
    char rname[FB_NAME_MAX + FB_EXT_MAX + 1], stem[FB_NAME_MAX + 1];
    PadrecSeg segs[2];
    PadrecMeta m;
    int k = 0, rc;
    const char *env = padrec_env_driven();

    if (env) {
        snprintf(why, n, "Driven by %s", env);
        return -1;
    }
    if (setup) {
        if (setup_path(setup, paths[k], sizeof paths[k], why, n) < 0)
            return -1;
        segs[k].path = paths[k];
        segs[k].trusted = setup->shipped || confirmed;
        segs[k].allowed = setup->shipped ? NULL : confirmed;
        k++;
    }
    if (name && name[0]) {
        if (file_path(name, paths[k], sizeof paths[k], why, n) < 0)
            return -1;
        segs[k].path = paths[k];
        segs[k].trusted = confirmed != NULL;
        segs[k].allowed = confirmed;
        k++;
    }
    if (!k) {
        snprintf(why, n, "No recording yet");
        return -1;
    }
    memset(&m, 0, sizeof m);
    if (record) {
        if (record_file(rname, sizeof rname, rec, sizeof rec, stem, sizeof stem, why, n) < 0)
            return -1;
        m.title = nv2a_overlay_game();
        m.title_id = xbox_path_title_id();
        m.name = stem;
        m.start_point = start_point;
        m.setup = setup ? setup->name : NULL;
    }
    padrec_set_title(xbox_path_title_id(), nv2a_overlay_game());
    rc = padrec_play_chain(segs, k, armed ? PADREC_ARMED : 0, record ? rec : NULL, &m, why, n);
    if (rc < 0) {
        if (record)
            host_remove(rec);
        if (rc == PADREC_E_REQUIRES)
            snprintf(why, n, "It needs %s on", padrec_unmet());
        return rc;
    }
    if (record)
        snprintf(s_last, sizeof s_last, "%s", rname);
    else if (name && name[0] && name != s_last)
        snprintf(s_last, sizeof s_last, "%s", name);
    return 0;
}

/* The key names comes from the file, which may be an imported .pad or a
 * shared bug report: one click must not write an arbitrary key (a debug or
 * unsafe option) into the player's config. Only an installed mod's switch
 * is turned on here, as the Mods page would; any other key stays unmet and
 * the player sets it by hand if they trust it (review 1, F1). */
int enhance_padrec_require_turnable(const char *key)
{
    int idx;
    return key && key[0] && mod_of_key(key, &idx) >= 0;
}

int enhance_padrec_require_on(const char *key, char *why, size_t n)
{
    const char *path = enhance_cfg_user_path(), *val = "true";
    char dir[FB_PATH_MAX], err[256], lit[64];
    char *sep;
    int idx = -1, m = key ? mod_of_key(key, &idx) : -1, later = 0;
    if (!key || !key[0]) {
        snprintf(why, n, "No key");
        return -1;
    }
    if (m < 0) {
        snprintf(why, n, "%s is not a mod's switch: set it yourself if you trust it", key);
        return -1;
    }
    if (!path) {
        snprintf(why, n, "Off with RECOMP_CONFIG=none: set %s yourself", key);
        return -1;
    }
    snprintf(dir, sizeof dir, "%s", path);
    sep = strrchr(dir, '/');
#ifdef _WIN32
    if (strrchr(dir, '\\') > sep)
        sep = strrchr(dir, '\\');
#endif
    if (sep) {
        *sep = 0;
        fb_mkdirs(dir);     /* a failure is the patch's to name */
    }
    if (m >= 0) {
        /* Through the mod layer, as the Mods page's switch: it goes live
         * when the mod can (on_enable), and the file gets the setting's own
         * literal ("on" for a choice). */
        later = xbox_mod_set_enabled(m, 1) != XBOX_MOD_ON;
        if (idx >= 0 &&
            xbox_mod_setting_literal(m, (uint32_t)idx,
                                     xbox_mod_setting_value(m, (uint32_t)idx), lit, sizeof lit))
            val = lit;
    }
    if (enhance_cfg_patch(path, &key, &val, 1, err, sizeof err) != 0) {
        snprintf(why, n, "%s", err);
        return -1;
    }
    fprintf(stderr, LOGP "%s = %s for a recording's requires (%s)%s\n", key, val, path,
            later ? ", at restart" : "");
    return later;
}

int enhance_padrec_playable(const PadFileInfo *info, int rc, const char *rc_why, char *why,
                            size_t n)
{
    if (rc < 0 || !info) {
        snprintf(why, n, "%s", rc_why && *rc_why ? rc_why : "It cannot be read");
        return 0;
    }
    if (strcmp(pad_header_origin(&info->h), "session")) {
        snprintf(why, n, "Recorded from boot: it plays only with RECOMP_PAD_SCRIPT");
        return 0;
    }
    if (pad_header_title_check(&info->h, xbox_path_title_id(), nv2a_overlay_game(), why, n) < 0)
        return 0;
    return 1;
}

int enhance_padrec_live(int state)
{
    return state == PADREC_REC || state == PADREC_PLAY || state == PADREC_ARMED_REC
           || state == PADREC_ARMED_PLAY || state == PADREC_WAIT;
}

static void refuse(const char *what, const char *why)
{
    nv2a_overlay_toast("%s: %s", what, why);
    fprintf(stderr, LOGP "%s refused: %s\n", what, why);
}

void enhance_padrec_requests(int menu_open)
{
    int req = enhance_actions_padrec_request();
    PadrecStatus st;
    int live, rec;
    char why[160];

    if (!req || menu_open)
        return;
    why[0] = 0;
    padrec_status(&st);
    /* Stop first: F5 then F2 in one poll means "stop, then play". The
     * status shows the stopped session until the pad thread's next poll,
     * so a stop in the same request clears the guards below. */
    rec = st.state == PADREC_REC || st.state == PADREC_ARMED_REC;
    live = enhance_padrec_live(st.state) && !(req & 4);
    if (req & 4)
        padrec_stop();
    if (req & 1) {
        /* A toggle: a record running or armed ends with @end. The
         * shortcuts start at once (padrec.start is the menu's). A
         * playback is never replaced by a record, as on the page. */
        if (rec) {
            if (live)
                padrec_record_stop();
            return;     /* F1 with F2 or F5: the toggle only */
        }
        if (live)
            refuse("Record", "Stop the playback first");
        else if (enhance_padrec_record(0, NULL, why, sizeof why) < 0)
            refuse("Record", why);
        else
            live = rec = 1;
    }
    if (req & 2) {
        struct enhance_padrec_file f[1];
        const char *name = s_last;
        /* The last file played or recorded, else the newest. During a
         * record s_last is the file being written: it is never played. */
        if (!name[0] && enhance_padrec_list(f, 1) == 1)
            name = f[0].name;
        if (live)
            refuse("Play", rec ? "Stop the record first" : "Stop the playback first");
        else if (enhance_padrec_play_file(name, 0, why, sizeof why) < 0)
            refuse("Play", why);
    }
}

void enhance_padrec_menu_closed(void)
{
    padrec_release();
}

/* ---- the strip (R4) ------------------------------------------------------ */

static void clock_text(uint32_t ms, char *out, size_t n)
{
    uint32_t s = ms / 1000;
    if (s >= 3600)
        snprintf(out, n, "%u:%02u:%02u", s / 3600, s / 60 % 60, s % 60);
    else
        snprintf(out, n, "%u:%02u", s / 60, s % 60);
}

static const char *state_word(int state)
{
    switch (state) {
    case PADREC_REC: return "REC";
    case PADREC_PLAY: return "PLAY";
    case PADREC_ARMED_REC:
    case PADREC_ARMED_PLAY: return "ARMED";
    case PADREC_WAIT: return "WAIT";
    case PADREC_ENDED: return "ENDED";
    case PADREC_STOPPED: return "STOPPED";
    default: return "";
    }
}

void enhance_padrec_strip_text(const PadrecStatus *st, char *head, size_t hn, char *times,
                               size_t tn)
{
    char el[16], tot[16];
    const char unit = st->clock == 1 ? 'f' : st->clock == 2 ? 'g' : 't';
    int play = st->nsteps > 0;

    /* A chain names its place, setup first (R4): "PLAY  2/2 my-run". */
    if (st->segs > 1)
        snprintf(head, hn, "%s  %d/%d %s", state_word(st->state), st->seg, st->segs, st->name);
    else
        snprintf(head, hn, "%s  %s", state_word(st->state), st->name);
    clock_text(st->ms, el, sizeof el);
    if (st->state == PADREC_ARMED_REC || st->state == PADREC_ARMED_PLAY) {
        snprintf(times, tn, "Starts when the menu closes");
    } else if (play && st->count && st->ms) {
        /* The total at the rate the session has run at so far: a game at
         * 30 fps on a poll clock is not mistaken for 60. */
        clock_text((uint32_t)((uint64_t)st->total * st->ms / st->count), tot, sizeof tot);
        snprintf(times, tn, "%s / %s  \xC2\xB7  step %d/%d  \xC2\xB7  %c%u", el, tot, st->step,
                 st->nsteps, unit, st->count);
    } else if (play) {
        snprintf(times, tn, "%s  \xC2\xB7  step %d/%d", el, st->step, st->nsteps);
    } else {
        snprintf(times, tn, "%s  \xC2\xB7  %c%u", el, unit, st->count);
    }
}

int enhance_padrec_strip_compose(mu_Context *ctx, const PadrecStatus *st, uint32_t out_w,
                                 uint32_t out_h, int top)
{
    const struct nv2a_ovl_theme *t = &nv2a_ovl_theme_default;
    float s = nv2a_ovl_ui_scale(out_h);
    mu_Font ft = OVL_FONT(OVL_FONT_BODY, nv2a_ovl_px(s, t->font_body), 0);
    mu_Font fl = OVL_FONT(OVL_FONT_SMALL, nv2a_ovl_px(s, t->font_small), 0);
    int pad = nv2a_ovl_px(s, t->pad), gap = nv2a_ovl_px(s, t->gap);
    int radius = nv2a_ovl_px(s, t->radius), shadow = nv2a_ovl_px(s, t->shadow) / 2;
    int th = nv2a_ovl_text_height(ft), lh = nv2a_ovl_text_height(fl);
    int dot = th / 3 > 2 ? th / 3 : 2, dev = enhance_mod_status_device();
    int live = st->state == PADREC_REC || st->state == PADREC_PLAY || st->state == PADREC_WAIT
               || st->state == PADREC_ARMED_REC || st->state == PADREC_ARMED_PLAY;
    const char *hint = live ? "{act:padrec.stop} Stop" : "";
    char head[PADREC_NAME_MAX + 16], times[96];
    int w, h, x, y, lines = 2, bar = st->nsteps > 0 && st->total > 0;
    mu_Color dc;

    if (!ctx || st->state == PADREC_IDLE)
        return 0;
    enhance_padrec_strip_text(st, head, sizeof head, times, sizeof times);
    w = dot + gap + enhance_text_width(ft, head, XBOX_PAD_FAMILY_AUTO, dev);
    {
        int tw = enhance_text_width(fl, times, XBOX_PAD_FAMILY_AUTO, dev);
        int hw = hint[0] ? enhance_text_width(fl, hint, XBOX_PAD_FAMILY_AUTO, dev) + 2 * gap : 0;
        if (tw + hw > w)
            w = tw + hw;
    }
    if (st->warn[0]) {
        int ww = enhance_text_width(fl, st->warn, XBOX_PAD_FAMILY_AUTO, dev);
        if (ww > w)
            w = ww;
        lines++;
    }
    w += 2 * pad;
    if (w > (int)out_w * 9 / 10)
        w = (int)out_w * 9 / 10;
    h = 2 * pad * 3 / 4 + th + (lines - 1) * (lh + gap / 2) + (bar ? gap : 0);
    /* Bottom centre, in the title-safe area, clear of the toasts' corner;
     * at the top while the menu is open, which covers the bottom. */
    x = ((int)out_w - w) / 2;
    y = top ? (int)out_h * 5 / 100 : (int)out_h - (int)out_h * 5 / 100 - h;
    {
        mu_Rect r = mu_rect(x, y, w, h);
        int ly = y + pad * 3 / 4;
        nv2a_ovl_ui_shadow(ctx, r, radius, shadow, t->shadow_alpha);
        nv2a_ovl_ui_rrect(ctx, r, radius, 1.0f, t->bg);
        dc = st->state == PADREC_REC ? t->bad
             : st->state == PADREC_WAIT || st->state == PADREC_ARMED_REC
                     || st->state == PADREC_ARMED_PLAY ? t->warn
             : st->state == PADREC_PLAY ? t->good : t->text_dim;
        nv2a_ovl_ui_rrect(ctx, mu_rect(x + pad, ly + (th - dot) / 2, dot, dot), dot / 2, 1.0f, dc);
        enhance_text_draw(ctx, ft, head, x + pad + dot + gap, ly, t->text, XBOX_PAD_FAMILY_AUTO,
                          dev);
        ly += th + gap / 2;
        enhance_text_draw(ctx, fl, times, x + pad, ly, t->text_dim, XBOX_PAD_FAMILY_AUTO, dev);
        if (hint[0]) {
            int hw = enhance_text_width(fl, hint, XBOX_PAD_FAMILY_AUTO, dev);
            enhance_text_draw(ctx, fl, hint, x + w - pad - hw, ly, t->text_dim,
                              XBOX_PAD_FAMILY_AUTO, dev);
        }
        ly += lh + gap / 2;
        if (st->warn[0]) {
            enhance_text_draw(ctx, fl, st->warn, x + pad, ly, t->warn, XBOX_PAD_FAMILY_AUTO, dev);
            ly += lh + gap / 2;
        }
        if (bar) {
            int bw = w - 2 * pad, bh = gap / 2 > 2 ? gap / 2 : 2;
            uint32_t c = st->count < st->total ? st->count : st->total;
            int fw = (int)((uint64_t)bw * c / st->total);
            nv2a_ovl_ui_rrect(ctx, mu_rect(x + pad, ly, bw, bh), bh / 2, 1.0f, t->bg_row);
            if (fw > 0)
                nv2a_ovl_ui_rrect(ctx, mu_rect(x + pad, ly, fw, bh), bh / 2, 1.0f, t->accent);
        }
    }
    return 1;
}

/* The tick's state: the flip thread is its only caller. */
static uint32_t s_seen_serial;
static int s_seen_state = -1;
static uint64_t s_state_t, s_drawn_t;
static int s_shown, s_shown_top;
static uint32_t s_stop_gen, s_seen_gen;

static void hide(void)
{
    if (s_shown)
        nv2a_overlay_hide(NV2A_OVL_PADREC);
    s_shown = 0;
}

void enhance_padrec_strip_tick(int stopping)
{
    PadrecStatus st;
    uint64_t now;
    uint32_t gen;
    int px, py, w, h, top, ended;
    mu_Context *ctx;

    if (stopping) {
        __atomic_add_fetch(&s_stop_gen, 1, __ATOMIC_SEQ_CST);
        nv2a_overlay_hide(NV2A_OVL_PADREC);
        return;
    }
    gen = __atomic_load_n(&s_stop_gen, __ATOMIC_SEQ_CST);
    if (gen != s_seen_gen) {
        s_seen_gen = gen;
        s_shown = 0;
    }
    /* One load until a session starts. */
    if (!padrec_any() || __atomic_load_n(&s_hud, __ATOMIC_RELAXED) == PADREC_HUD_OFF
        || !nv2a_overlay_armed())
        return;
    padrec_status(&st);
    now = xbox_InputNowMs();
    if (st.serial != s_seen_serial || st.state != s_seen_state) {
        s_seen_serial = st.serial;
        s_seen_state = st.state;
        s_state_t = now;
        s_drawn_t = 0;
    }
    ended = st.state == PADREC_ENDED || st.state == PADREC_STOPPED;
    if (st.state == PADREC_IDLE || (ended && now - s_state_t >= ENDED_MS)) {
        hide();
        return;
    }
    top = enhance_menu_is_open();
    if (s_shown && top == s_shown_top && now - s_drawn_t < REFRESH_MS)
        return;
    if (!nv2a_overlay_layer_armed(NV2A_OVL_PADREC)
        && nv2a_overlay_arm(1u << NV2A_OVL_PADREC, 1080) < 0)
        return;
    if (!nv2a_overlay_menu_trylock())
        return;
    nv2a_overlay_placement(&px, &py, &w, &h);
    if (w <= 0 || h <= 0)
        w = 1920, h = 1080;
    ctx = nv2a_overlay_ui_begin(NV2A_OVL_PADREC);
    if (ctx) {
        int n = enhance_padrec_strip_compose(ctx, &st, (uint32_t)w, (uint32_t)h, top);
        nv2a_overlay_ui_publish(NV2A_OVL_PADREC);
        if (!n || __atomic_load_n(&s_stop_gen, __ATOMIC_SEQ_CST) != gen)
            nv2a_overlay_hide(NV2A_OVL_PADREC);
        s_shown = n > 0;
    }
    nv2a_overlay_menu_unlock();
    s_shown_top = top;
    s_drawn_t = now ? now : 1;
}
