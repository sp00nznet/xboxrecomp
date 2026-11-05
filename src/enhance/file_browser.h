/*
 * file_browser.h - the in-overlay file browser's model (openspec change
 * overlay-revamp, D6): roots, a folder's listing, the player's file names
 * and the import and export file work. No UI here: the menu draws what the
 * model holds, and the ctest drives it on a temp folder.
 *
 * The model is pure apart from the one folder-listing callback (the host's
 * by default) and the file calls it has to make: resolving a link, the
 * import read and the export write. Paths are UTF-8 inside; on Windows
 * every call converts them to UTF-16 at the call (_wfopen, FindFirstFileW,
 * MoveFileExW), so a non-ASCII user name or folder works.
 *
 * Safety (D6), all here so the ctest covers it:
 *   - a typed name is sanitised before any path is built (fb_name_check);
 *   - a link to a folder opens only when its target stays under the same
 *     root, and ".." stops at the root after resolution, so a link never
 *     climbs out of a root;
 *   - an import over FB_IMPORT_CAP is refused before it is read;
 *   - an export onto an existing file asks first (struct fb_export);
 *   - every write goes to <name>.tmp in the same folder, flushed, then is
 *     renamed over the target, so a crash leaves the old file or the new.
 */
#ifndef FILE_BROWSER_H
#define FILE_BROWSER_H

#include <stddef.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FB_PATH_MAX 1024
#define FB_ENTRY_NAME_MAX 256    /* bytes of UTF-8 with the NUL */
#define FB_ROOTS_MAX 32          /* the four roots plus 26 drive letters */
#define FB_LABEL_MAX 32
#define FB_EXTS_MAX 8
#define FB_EXT_MAX 16
#define FB_NAME_MAX 64           /* a typed name, bytes of UTF-8 before the extension */
#define FB_IMPORT_CAP (256u * 1024u)
#define FB_REASON_MAX 96

/* Results. A listing's error is the errno it failed with (EACCES and EPERM
 * are the "cannot be read" case the menu shows; on macOS, TCC's refusal
 * is EPERM); the rest are these. */
#define FB_OK 0
#define FB_E_ROOT (-1)       /* ".." at the root, or a link out of it */
#define FB_E_NOT_DIR (-2)
#define FB_E_TOO_BIG (-3)    /* an import over FB_IMPORT_CAP */
#define FB_E_IO (-4)         /* errno has the reason */
#define FB_E_RANGE (-5)      /* an index or a path too long */
#define FB_E_EXISTS (-6)     /* fb_write_new: the name is taken */
#define FB_E_NAME (-7)       /* a stem fb_name_check refuses */

/* root kinds */
enum fb_root_kind {
    FB_ROOT_EXPORTS, /* <user dir>/exports/<game>, created when opened */
    FB_ROOT_CONFIG,  /* the user config dir */
    FB_ROOT_SHADERS, /* <user dir>/shader-profiles */
    FB_ROOT_HOME,    /* $HOME, %USERPROFILE%, or the Unix home under Wine */
    FB_ROOT_DRIVE,   /* a Windows drive letter (not under Wine) */
    FB_ROOT_OTHER
};

struct fb_root {
    char label[FB_LABEL_MAX];
    char path[FB_PATH_MAX]; /* as given */
    char real[FB_PATH_MAX]; /* resolved when opened; "" before */
    int kind;               /* enum fb_root_kind */
    int create;             /* make the folder when it is opened */
};

/* One row. A link is listed with is_link set; link_ok says whether it may
 * be opened (a folder link whose target is under the root, or a file link
 * likewise). A link that cannot be resolved is listed, never opened. */
struct fb_entry {
    char name[FB_ENTRY_NAME_MAX];
    uint64_t size;
    uint8_t is_dir;  /* for a link, the target's */
    uint8_t is_link;
    uint8_t link_ok; /* 1 for a plain entry */
    uint8_t hidden;  /* the host's hidden flag (Windows); dot names are hidden anyway */
};

/* The listing callback: call emit once per entry of dir ("." and ".."
 * included or not, the model drops them) and return 0, or the errno the
 * folder failed with. emit returns nonzero to stop. The host's is
 * fb_list_host. */
typedef int (*fb_emit_fn)(void *ctx, const struct fb_entry *e);
typedef int (*fb_list_fn)(const char *dir, fb_emit_fn emit, void *ctx, void *user);

int fb_list_host(const char *dir, fb_emit_fn emit, void *ctx, void *user);

struct file_browser {
    struct fb_root roots[FB_ROOTS_MAX];
    int nroots;
    int root;               /* the open root, -1 for the root list */
    char cwd[FB_PATH_MAX];  /* resolved, always under roots[root].real */
    struct fb_entry *entries;
    int n, cap;
    int err;                /* the last listing's errno (0 when it listed) */
    char exts[FB_EXTS_MAX][FB_EXT_MAX]; /* with the dot; none: every file */
    int nexts;
    int show_hidden;        /* dot names and Windows hidden/system entries */
    fb_list_fn list;        /* fb_list_host when NULL */
    void *list_user;
};

void fb_init(struct file_browser *fb);
void fb_free(struct file_browser *fb);

/* The file extensions to list, comma separated (".toml,.xsv"); NULL or ""
 * lists every file. Folders are always listed. */
void fb_set_filter(struct file_browser *fb, const char *exts);

/* Append a root; the index, or -1 when full or the path is too long. */
int fb_root_add(struct file_browser *fb, const char *label, const char *path, int kind,
                int create);

/* The standard roots: Exports (<user_dir>/exports/<game>), Config
 * (user_dir), Shader profiles (<user_dir>/shader-profiles), Home, and on
 * Windows outside Wine one root per drive letter. user_dir NULL (no user
 * config, RECOMP_CONFIG=none) leaves the first three out; game NULL or ""
 * puts exports straight under <user_dir>/exports. The root count. */
int fb_add_default_roots(struct file_browser *fb, const char *user_dir, const char *game);

/* Open root i at its top and list it. FB_OK, an errno from the listing
 * (fb->err too; the root is then open with no rows) or FB_E_RANGE. A root
 * whose folder is missing (and not create) lists as ENOENT. */
int fb_open_root(struct file_browser *fb, int i);

/* Back to the root list (root -1, no rows). */
void fb_close_root(struct file_browser *fb);

/* Open the folder row i: FB_OK or the listing's errno, FB_E_NOT_DIR, or
 * FB_E_ROOT for a link that leaves the root. */
int fb_enter(struct file_browser *fb, int i);

/* One folder up: FB_OK (listed), FB_E_ROOT at the root (the UI then goes
 * back to the root list), or the listing's errno. */
int fb_up(struct file_browser *fb);
int fb_at_root(const struct file_browser *fb);

/* List cwd again (after an export, say). */
int fb_refresh(struct file_browser *fb);

/* <cwd>/<row i's name> into out. FB_OK or FB_E_RANGE. */
int fb_entry_path(const struct file_browser *fb, int i, char *out, size_t n);

/* ---- names ---- */

/* Check a typed name and build "<stem><ext>" into out. A trailing ext
 * (any case) on name is taken off first, so "foo.toml" and "foo" both give
 * "foo.toml". A trailing dot or space is trimmed (Windows drops them).
 * Refused, on every host since a file exported on the Mac may be imported
 * on Windows: an empty name, "." and "..", a leading dot, '/', '\\', ':', the other
 * characters Windows refuses (* ? " < > |), control characters, bad UTF-8,
 * the reserved CON, PRN, AUX, NUL, COM1-COM9 and LPT1-LPT9 with or without
 * an extension, and a stem over FB_NAME_MAX bytes. 0, or -1 with *reason
 * (a static string the field shows). */
int fb_name_check(const char *name, const char *ext, char *out, size_t n, const char **reason);

/* The default export name, "<kind>-<yyyymmdd-hhmm><ext>", at local time t.
 * The minute in it means a repeat export a minute later never asks to
 * replace. */
void fb_default_name(const char *kind, time_t t, const char *ext, char *out, size_t n);

/* A name in dir that is not taken: "<stem><ext>", then "<stem>-2<ext>",
 * "<stem>-3<ext>"... The stem goes through fb_name_check first. A hint
 * for the UI only: the name may be taken before it is written, so an
 * import writes through fb_import_unique. FB_OK, FB_E_NAME or FB_E_RANGE. */
int fb_unique_name(const char *dir, const char *stem, const char *ext, char *out, size_t n);

/* <dir><sep><name> into out; FB_OK or FB_E_RANGE. */
int fb_path_join(const char *dir, const char *name, char *out, size_t n);

/* ---- files ---- */

/* 1 when path names something (a file or a folder), 0 when not. */
int fb_exists(const char *path);

/* Make dir and its parents. 0, or -1 with errno set. */
int fb_mkdirs(const char *path);

/* Read the file row i of the open folder for an import: its path is
 * resolved again (a link's link_ok from the listing is not trusted) and
 * must be under the root, and the resolved path is opened without
 * following a link. FB_E_ROOT out of the root, FB_E_NOT_DIR for a folder
 * row (sic: not a file), else as fb_read_file. */
int fb_import_entry(const struct file_browser *fb, int i, size_t cap, char **out, size_t *len);

/* Read a file of at most cap bytes (FB_IMPORT_CAP for an import) into a
 * malloc'd, NUL-terminated buffer. A larger file is refused from its size
 * before any byte is read: FB_E_TOO_BIG. Anything but a regular file (a
 * folder, a FIFO, a device) is refused without waiting on it. FB_OK,
 * FB_E_TOO_BIG or FB_E_IO. */
int fb_read_file(const char *path, size_t cap, char **out, size_t *len);

/* Write data to path through a fresh "<path>.<pid>-<k>.tmp" in the same
 * folder (made with O_EXCL and O_NOFOLLOW, CREATE_NEW on Windows, so an
 * existing file or link of that name is never written through): written,
 * flushed and synced, then renamed over path (MoveFileExW with
 * MOVEFILE_REPLACE_EXISTING on Windows). On failure the tmp is removed and
 * path is untouched. FB_OK or FB_E_IO. */
int fb_write_atomic(const char *path, const void *data, size_t len);

/* The same, committed only while path is absent (link + unlink, or an
 * exclusive rename; MoveFileExW without REPLACE_EXISTING): FB_E_EXISTS
 * when it is taken, with nothing written. */
int fb_write_new(const char *path, const void *data, size_t len);

/* An import's write under a name not taken: stem through fb_name_check,
 * then "<stem><ext>", "<stem>-2<ext>"... each tried with fb_write_new, so
 * there is no gap between the choice and the write. The name used into
 * out. FB_OK, FB_E_NAME, FB_E_RANGE or FB_E_IO. */
int fb_import_unique(const char *dir, const char *stem, const char *ext, const void *data,
                     size_t len, char *out, size_t n);

/* ---- export ---- */

/* An export's state: begin it with a folder and a typed name. The name is
 * checked (FB_EXP_REFUSED with the reason, the field stays open; a folder
 * of that name is refused too); a free
 * name is FB_EXP_READY; a taken one is FB_EXP_CONFIRM ("Replace <name>?")
 * with the focus on No. Answering yes makes it READY, no CANCELLED. */
enum fb_export_state {
    FB_EXP_REFUSED,
    FB_EXP_READY,
    FB_EXP_CONFIRM,
    FB_EXP_CANCELLED,
    FB_EXP_DONE,
    FB_EXP_FAILED
};

struct fb_export {
    int state;              /* enum fb_export_state */
    int focus_yes;          /* the confirm's focus: 0 is No */
    char name[FB_NAME_MAX + FB_EXT_MAX + 1];
    char path[FB_PATH_MAX];
    char prompt[FB_REASON_MAX + FB_NAME_MAX + FB_EXT_MAX];
    const char *reason;     /* FB_EXP_REFUSED and FB_EXP_FAILED */
};

int fb_export_begin(struct fb_export *ex, const char *dir, const char *name, const char *ext);
void fb_export_answer(struct fb_export *ex, int yes);
/* Write data when READY: DONE or FAILED (reason set). The new state. */
int fb_export_write(struct fb_export *ex, const void *data, size_t len);

/* ---- host homes ---- */

/* A Unix path to a DOS one under Wine (kernel32's wine_get_dos_file_name):
 * write it into out and return 1, or 0. */
typedef int (*fb_dos_path_fn)(const char *unix_path, char *out, size_t n);

/* The Home root's folder, from what the host gave: under Wine the Unix
 * home (unix_home, Wine passes the host environment through) through
 * to_dos ("Z:\home\deck"; Z: is the Unix root /), else userprofile
 * (Windows), else unix_home (POSIX). 1 with out set, 0 when none is known.
 * Pure: fb_home_dir feeds it the host's values. */
int fb_home_pick(int under_wine, const char *unix_home, const char *userprofile,
                 fb_dos_path_fn to_dos, char *out, size_t n);
int fb_home_dir(char *out, size_t n);

/* 1 for a Windows build running under Wine or Proton. */
int fb_under_wine(void);

#ifdef __cplusplus
}
#endif

#endif
