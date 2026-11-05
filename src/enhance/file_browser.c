/*
 * file_browser.c - the in-overlay file browser's model (file_browser.h,
 * openspec change overlay-revamp D6). Paths are UTF-8 here and UTF-16 at
 * every Windows call, so the ANSI code page never touches a path.
 */
#ifdef _WIN32
/* GetFinalPathNameByHandleW is Vista and later. */
#if !defined(_WIN32_WINNT) || _WIN32_WINNT < 0x0600
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif
#include <windows.h>
#include <io.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <pwd.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/syscall.h>
#endif
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "file_browser.h"

#ifdef _WIN32
#define FB_SEP '\\'
#else
#define FB_SEP '/'
#endif

static int is_sep(char c)
{
#ifdef _WIN32
    return c == '\\' || c == '/';
#else
    return c == '/';
#endif
}

static int lower(int c)
{
    return c >= 'A' && c <= 'Z' ? c + 32 : c;
}

/* ASCII case folding: the sort order, extensions and reserved names. A
 * non-ASCII byte compares as itself. */
static int casecmp(const char *a, const char *b)
{
    for (;; a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
}

static int casecmp_n(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        int x = lower((unsigned char)*a), y = lower((unsigned char)*b);
        if (x != y || !x)
            return x - y;
    }
    return 0;
}

static int copy(char *out, size_t n, const char *s)
{
    size_t l = strlen(s);
    if (l >= n)
        return FB_E_RANGE;
    memcpy(out, s, l + 1);
    return FB_OK;
}

/* ---- Windows: UTF-8 in, UTF-16 at the call ---- */

#ifdef _WIN32
static int to_w(const char *s, wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0;
}

static int from_w(const wchar_t *w, char *s, int cap)
{
    /* a lone surrogate (NTFS allows one) fails rather than turn into U+FFFD,
     * a name that would then open some other file */
    return WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, w, -1, s, cap, NULL, NULL) > 0;
}

static int err_from_win(DWORD e)
{
    switch (e) {
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
    case ERROR_NOT_READY:
        return ENOENT;
    case ERROR_DIRECTORY:
        return ENOTDIR;
    case ERROR_FILENAME_EXCED_RANGE:
        return ENAMETOOLONG;
    default:
        return EIO;
    }
}
#endif

/* ---- paths ---- */

int fb_path_join(const char *dir, const char *name, char *out, size_t n)
{
    size_t d = strlen(dir), l = strlen(name);
    int sep = d > 0 && !is_sep(dir[d - 1]);
    if (d + (size_t)sep + l >= n)
        return FB_E_RANGE;
    memcpy(out, dir, d);
    if (sep)
        out[d++] = FB_SEP;
    memcpy(out + d, name, l + 1);
    return FB_OK;
}

/* The real path of path: every link resolved, "." and ".." gone. 0, or -1
 * with errno set. Windows asks the handle (GetFinalPathNameByHandleW, which
 * follows symlinks and junctions) and drops the \\?\ prefix it adds. */
static int resolve(const char *path, char *out, size_t n)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX], r[FB_PATH_MAX];
    const wchar_t *p = r;
    HANDLE h;
    DWORD len;
    if (!to_w(path, w, FB_PATH_MAX)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    h = CreateFileW(w, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        errno = err_from_win(GetLastError());
        return -1;
    }
    len = GetFinalPathNameByHandleW(h, r, FB_PATH_MAX, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    CloseHandle(h);
    if (!len || len >= FB_PATH_MAX) {
        errno = len ? ENAMETOOLONG : EIO;
        return -1;
    }
    if (!wcsncmp(p, L"\\\\?\\UNC\\", 8)) {
        p += 6; /* keep "\\" before the server */
        r[6] = L'\\';
    } else if (!wcsncmp(p, L"\\\\?\\", 4)) {
        p += 4;
    }
    if (!from_w(p, out, (int)n)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
#else
    char r[PATH_MAX];
    if (!realpath(path, r))
        return -1;
    if (strlen(r) >= n) {
        errno = ENAMETOOLONG;
        return -1;
    }
    strcpy(out, r);
    return 0;
#endif
}

/* path is root or below it (both resolved). Windows paths compare without
 * case, as the file system does. */
static int under_root(const char *root, const char *path)
{
    size_t r = strlen(root);
    if (!r)
        return 0; /* a root that never resolved holds nothing */
#ifdef _WIN32
    if (casecmp_n(root, path, r))
        return 0;
#else
    if (strncmp(root, path, r))
        return 0;
#endif
    return !path[r] || (r && is_sep(root[r - 1])) || is_sep(path[r]);
}

static int same_path(const char *a, const char *b)
{
#ifdef _WIN32
    return !casecmp(a, b);
#else
    return !strcmp(a, b);
#endif
}

int fb_exists(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    return to_w(path, w, FB_PATH_MAX) && GetFileAttributesW(w) != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st;
    /* lstat: a dangling link is a name that is taken too */
    return lstat(path, &st) == 0;
#endif
}

static int mkdir_one(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    if (!to_w(path, w, FB_PATH_MAX)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS)
        return 0;
    errno = err_from_win(GetLastError());
    return -1;
#else
    if (mkdir(path, 0755) == 0 || errno == EEXIST)
        return 0;
    return -1;
#endif
}

int fb_mkdirs(const char *path)
{
    char buf[FB_PATH_MAX];
    size_t i = 0, l;
    if (copy(buf, sizeof buf, path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    l = strlen(buf);
    while (l > 1 && is_sep(buf[l - 1]))
        buf[--l] = 0;
#ifdef _WIN32
    /* past "C:\" or "\\server\share\" */
    if (l >= 2 && buf[1] == ':')
        i = 3;
    else if (l >= 2 && is_sep(buf[0]) && is_sep(buf[1])) {
        int seps = 0;
        for (i = 2; i < l && seps < 2; i++)
            seps += is_sep(buf[i]);
    }
#endif
    for (i = i ? i : 1; i < l; i++) {
        if (!is_sep(buf[i]))
            continue;
        buf[i] = 0;
        if (mkdir_one(buf) < 0 && !fb_exists(buf))
            return -1;
        buf[i] = FB_SEP;
    }
    return mkdir_one(buf) < 0 && !fb_exists(buf) ? -1 : 0;
}

/* ---- the host's listing ---- */

int fb_list_host(const char *dir, fb_emit_fn emit, void *ctx, void *user)
{
    struct fb_entry e;
    (void)user;
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    char pat[FB_PATH_MAX];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    if (fb_path_join(dir, "*", pat, sizeof pat) || !to_w(pat, w, FB_PATH_MAX))
        return ENAMETOOLONG;
    h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD le = GetLastError();
        /* an empty drive root has no "." to find */
        return le == ERROR_FILE_NOT_FOUND ? 0 : err_from_win(le);
    }
    do {
        memset(&e, 0, sizeof e);
        /* a name too long for the row, or not valid UTF-16, is left out: it
         * could not be opened by its UTF-8 */
        if (!from_w(fd.cFileName, e.name, sizeof e.name))
            continue;
        e.is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.is_link = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
                    (fd.dwReserved0 == IO_REPARSE_TAG_SYMLINK ||
                     fd.dwReserved0 == IO_REPARSE_TAG_MOUNT_POINT);
        e.hidden = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        e.size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
        e.link_ok = 1;
        if (emit(ctx, &e))
            break;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return 0;
#else
    char path[FB_PATH_MAX];
    struct dirent *d;
    int err = 0;
    DIR *dp = opendir(dir);
    if (!dp)
        return errno ? errno : EIO;
    for (;;) {
        struct stat st;
        /* readdir's NULL is the end or an error, told apart by errno */
        errno = 0;
        if (!(d = readdir(dp))) {
            err = errno;
            break;
        }
        memset(&e, 0, sizeof e);
        if (copy(e.name, sizeof e.name, d->d_name) ||
            fb_path_join(dir, d->d_name, path, sizeof path) || lstat(path, &st))
            continue;
        e.link_ok = 1;
        if (S_ISLNK(st.st_mode)) {
            e.is_link = 1;
            /* the target's kind; a dangling link stays a file row */
            if (stat(path, &st))
                memset(&st, 0, sizeof st);
        }
        e.is_dir = S_ISDIR(st.st_mode) != 0;
        e.size = (uint64_t)st.st_size;
        if (emit(ctx, &e))
            break;
    }
    closedir(dp);
    return err;
#endif
}

/* ---- the model ---- */

void fb_init(struct file_browser *fb)
{
    memset(fb, 0, sizeof *fb);
    fb->root = -1;
}

void fb_free(struct file_browser *fb)
{
    free(fb->entries);
    fb->entries = NULL;
    fb->n = fb->cap = 0;
}

void fb_set_filter(struct file_browser *fb, const char *exts)
{
    fb->nexts = 0;
    while (exts && *exts && fb->nexts < FB_EXTS_MAX) {
        const char *c = strchr(exts, ',');
        size_t l = c ? (size_t)(c - exts) : strlen(exts);
        while (l && *exts == ' ')
            exts++, l--;
        while (l && exts[l - 1] == ' ')
            l--;
        if (l && l < FB_EXT_MAX) {
            memcpy(fb->exts[fb->nexts], exts, l);
            fb->exts[fb->nexts++][l] = 0;
        }
        exts = c ? c + 1 : NULL;
    }
}

int fb_root_add(struct file_browser *fb, const char *label, const char *path, int kind,
                int create)
{
    struct fb_root *r;
    if (fb->nroots >= FB_ROOTS_MAX || strlen(path) >= FB_PATH_MAX)
        return -1;
    r = &fb->roots[fb->nroots];
    memset(r, 0, sizeof *r);
    snprintf(r->label, sizeof r->label, "%s", label);
    strcpy(r->path, path);
    r->kind = kind;
    r->create = create;
    return fb->nroots++;
}

int fb_add_default_roots(struct file_browser *fb, const char *user_dir, const char *game)
{
    char p[FB_PATH_MAX], q[FB_PATH_MAX];
    if (user_dir && *user_dir) {
        if (!fb_path_join(user_dir, "exports", p, sizeof p)) {
            if (game && *game && !fb_path_join(p, game, q, sizeof q))
                fb_root_add(fb, "Exports", q, FB_ROOT_EXPORTS, 1);
            else if (!game || !*game)
                fb_root_add(fb, "Exports", p, FB_ROOT_EXPORTS, 1);
        }
        fb_root_add(fb, "Config", user_dir, FB_ROOT_CONFIG, 0);
        if (!fb_path_join(user_dir, "shader-profiles", p, sizeof p))
            fb_root_add(fb, "Shader profiles", p, FB_ROOT_SHADERS, 0);
    }
    if (fb_home_dir(p, sizeof p))
        fb_root_add(fb, "Home", p, FB_ROOT_HOME, 0);
#ifdef _WIN32
    /* Under Wine Z: is the Unix root and Home already reaches the player's
     * files; the drive list there is Wine's own C: and the Z: alias. */
    if (!fb_under_wine()) {
        DWORD m = GetLogicalDrives();
        int i;
        for (i = 0; i < 26; i++) {
            if (m & (1u << i)) {
                char lab[4] = {(char)('A' + i), ':', 0, 0};
                char root[4] = {(char)('A' + i), ':', '\\', 0};
                fb_root_add(fb, lab, root, FB_ROOT_DRIVE, 0);
            }
        }
    }
#endif
    return fb->nroots;
}

static int want_ext(const struct file_browser *fb, const char *name)
{
    size_t l = strlen(name);
    int i;
    if (!fb->nexts)
        return 1;
    for (i = 0; i < fb->nexts; i++) {
        size_t e = strlen(fb->exts[i]);
        if (l > e && !casecmp(name + l - e, fb->exts[i]))
            return 1;
    }
    return 0;
}

struct collect {
    struct file_browser *fb;
    int oom;
};

/* A row's link check: the target, resolved, must be under the open root. */
static int link_inside(const struct file_browser *fb, const char *name)
{
    char p[FB_PATH_MAX], r[FB_PATH_MAX];
    if (fb_path_join(fb->cwd, name, p, sizeof p) || resolve(p, r, sizeof r))
        return 0;
    return under_root(fb->roots[fb->root].real, r);
}

static int collect_one(void *ctx, const struct fb_entry *e)
{
    struct collect *c = ctx;
    struct file_browser *fb = c->fb;
    struct fb_entry *row;
    if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
        return 0;
    if (!fb->show_hidden && (e->name[0] == '.' || e->hidden))
        return 0;
    if (!e->is_dir && !want_ext(fb, e->name))
        return 0;
    if (fb->n == fb->cap) {
        int cap = fb->cap ? fb->cap * 2 : 64;
        void *m = realloc(fb->entries, (size_t)cap * sizeof *fb->entries);
        if (!m) {
            c->oom = 1;
            return 1;
        }
        fb->entries = m;
        fb->cap = cap;
    }
    row = &fb->entries[fb->n++];
    *row = *e;
    row->name[FB_ENTRY_NAME_MAX - 1] = 0;
    row->link_ok = e->is_link ? (uint8_t)link_inside(fb, e->name) : 1;
    return 0;
}

/* Folders first, then by name without case; equal names by bytes, so the
 * order is total. */
static int row_cmp(const void *a, const void *b)
{
    const struct fb_entry *x = a, *y = b;
    int c;
    if (x->is_dir != y->is_dir)
        return x->is_dir ? -1 : 1;
    c = casecmp(x->name, y->name);
    return c ? c : strcmp(x->name, y->name);
}

int fb_refresh(struct file_browser *fb)
{
    struct collect c = {fb, 0};
    fb_list_fn list = fb->list ? fb->list : fb_list_host;
    fb->n = 0;
    if (fb->root < 0) {
        fb->err = 0;
        return FB_OK;
    }
    /* a root that was missing when opened: it may be there now */
    if (!fb->roots[fb->root].real[0]) {
        struct fb_root *r = &fb->roots[fb->root];
        if (resolve(r->path, r->real, sizeof r->real)) {
            r->real[0] = 0;
            return fb->err = errno ? errno : ENOENT;
        }
        strcpy(fb->cwd, r->real);
    }
    fb->err = list(fb->cwd, collect_one, &c, fb->list_user);
    if (!fb->err && c.oom)
        fb->err = ENOMEM;
    if (fb->err)
        fb->n = 0;
    else if (fb->n > 1)
        qsort(fb->entries, (size_t)fb->n, sizeof *fb->entries, row_cmp);
    return fb->err;
}

int fb_open_root(struct file_browser *fb, int i)
{
    struct fb_root *r;
    if (i < 0 || i >= fb->nroots)
        return FB_E_RANGE;
    r = &fb->roots[i];
    if (r->create)
        fb_mkdirs(r->path); /* a failure shows as the listing's error */
    fb->root = i;
    fb->n = 0;
    if (resolve(r->path, r->real, sizeof r->real)) {
        fb->err = errno ? errno : ENOENT;
        r->real[0] = 0;
        snprintf(fb->cwd, sizeof fb->cwd, "%s", r->path);
        return fb->err;
    }
    strcpy(fb->cwd, r->real);
    return fb_refresh(fb);
}

void fb_close_root(struct file_browser *fb)
{
    fb->root = -1;
    fb->n = 0;
    fb->err = 0;
    fb->cwd[0] = 0;
}

int fb_at_root(const struct file_browser *fb)
{
    return fb->root < 0 || !fb->roots[fb->root].real[0] ||
           same_path(fb->cwd, fb->roots[fb->root].real);
}

int fb_enter(struct file_browser *fb, int i)
{
    char p[FB_PATH_MAX], r[FB_PATH_MAX];
    if (fb->root < 0 || i < 0 || i >= fb->n)
        return FB_E_RANGE;
    if (!fb->entries[i].is_dir)
        return FB_E_NOT_DIR;
    if (fb_path_join(fb->cwd, fb->entries[i].name, p, sizeof p))
        return FB_E_RANGE;
    /* Every folder is resolved, not only the rows listed as links: one may
     * have become a link since the listing. */
    if (resolve(p, r, sizeof r))
        return errno ? errno : ENOENT;
    if (!under_root(fb->roots[fb->root].real, r))
        return FB_E_ROOT;
    strcpy(fb->cwd, r);
    return fb_refresh(fb);
}

int fb_up(struct file_browser *fb)
{
    size_t l, keep;
    if (fb_at_root(fb))
        return FB_E_ROOT;
    l = strlen(fb->cwd);
    while (l > 1 && is_sep(fb->cwd[l - 1]))
        l--;
    while (l > 0 && !is_sep(fb->cwd[l - 1]))
        l--;
    /* keep the separator of "/" or "C:\" */
    keep = l;
    while (keep > 1 && is_sep(fb->cwd[keep - 1]))
        keep--;
#ifdef _WIN32
    if (keep == 2 && fb->cwd[1] == ':')
        keep = 3;
#endif
    if (keep == 0)
        keep = 1;
    fb->cwd[keep] = 0;
    /* cwd is resolved and under the root, so its parent is too */
    return fb_refresh(fb);
}

int fb_entry_path(const struct file_browser *fb, int i, char *out, size_t n)
{
    if (fb->root < 0 || i < 0 || i >= fb->n)
        return FB_E_RANGE;
    return fb_path_join(fb->cwd, fb->entries[i].name, out, n);
}

/* ---- names ---- */

/* Bytes of one UTF-8 sequence at s, 0 when it is not one (overlong,
 * surrogate, past U+10FFFF, truncated). */
static int utf8_len(const unsigned char *s)
{
    unsigned c = s[0], cp;
    int n, i;
    if (c < 0x80)
        return 1;
    if (c >= 0xc2 && c <= 0xdf)
        n = 2, cp = c & 0x1f;
    else if (c >= 0xe0 && c <= 0xef)
        n = 3, cp = c & 0x0f;
    else if (c >= 0xf0 && c <= 0xf4)
        n = 4, cp = c & 0x07;
    else
        return 0;
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xc0) != 0x80)
            return 0;
        cp = (cp << 6) | (s[i] & 0x3f);
    }
    if ((n == 3 && (cp < 0x800 || (cp >= 0xd800 && cp <= 0xdfff))) ||
        (n == 4 && (cp < 0x10000 || cp > 0x10ffff)))
        return 0;
    return n;
}

/* Windows' reserved device names: refused with any extension ("nul.toml")
 * and with trailing spaces before it. COM and LPT take 1-9 and the
 * superscripts ¹ ² ³, which Windows also reserves. */
static int reserved(const char *stem)
{
    static const char *const names[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
    char base[FB_NAME_MAX + 1];
    size_t l = strcspn(stem, ".");
    unsigned i;
    if (l > FB_NAME_MAX)
        return 0;
    memcpy(base, stem, l);
    base[l] = 0;
    while (l && base[l - 1] == ' ')
        base[--l] = 0;
    for (i = 0; i < sizeof names / sizeof names[0]; i++)
        if (!casecmp(base, names[i]))
            return 1;
    if (l >= 4 && (!casecmp_n(base, "COM", 3) || !casecmp_n(base, "LPT", 3))) {
        const char *d = base + 3;
        if (l == 4 && d[0] >= '1' && d[0] <= '9')
            return 1;
        if (l == 5 && (!strcmp(d, "\xc2\xb9") || !strcmp(d, "\xc2\xb2") || !strcmp(d, "\xc2\xb3")))
            return 1;
    }
    return 0;
}

int fb_name_check(const char *name, const char *ext, char *out, size_t n, const char **reason)
{
    char stem[512];
    size_t l, e = ext ? strlen(ext) : 0, i;
    const char *why = NULL;
    if (!name || (l = strlen(name)) >= sizeof stem) {
        why = name ? "Too long (64 bytes at most)" : "Type a name";
        goto refuse;
    }
    memcpy(stem, name, l + 1);
    if (!strcmp(stem, ".") || !strcmp(stem, "..")) {
        why = "\".\" and \"..\" are not names";
        goto refuse;
    }
    /* Windows drops a trailing dot or space, so "a." and "a" would be the
     * same file there. Trimmed before the extension comes off ("a.toml "
     * is a.toml) and after it ("a..toml"). */
    while (l && (stem[l - 1] == '.' || stem[l - 1] == ' '))
        stem[--l] = 0;
    if (e && l > e && !casecmp(stem + l - e, ext))
        stem[l -= e] = 0;
    while (l && (stem[l - 1] == '.' || stem[l - 1] == ' '))
        stem[--l] = 0;
    if (!l) {
        why = "Type a name";
        goto refuse;
    }
    /* a dot name is hidden on POSIX, so the export would seem lost */
    if (stem[0] == '.') {
        why = "A name cannot start with a dot";
        goto refuse;
    }
    for (i = 0; i < l;) {
        unsigned char c = (unsigned char)stem[i];
        int k;
        if (c < 0x20 || c == 0x7f) {
            why = "No control characters";
            goto refuse;
        }
        if (c == '/' || c == '\\') {
            why = "No / or \\ in a name";
            goto refuse;
        }
        if (c == ':') {
            why = "No : in a name";
            goto refuse;
        }
        if (strchr("*?\"<>|", c)) {
            why = "No * ? \" < > or | in a name";
            goto refuse;
        }
        if (!(k = utf8_len((const unsigned char *)stem + i))) {
            why = "Not valid text";
            goto refuse;
        }
        i += (size_t)k;
    }
    if (l > FB_NAME_MAX) {
        why = "Too long (64 bytes at most)";
        goto refuse;
    }
    if (reserved(stem)) {
        why = "Reserved by Windows";
        goto refuse;
    }
    if (l + e >= n) {
        why = "Too long (64 bytes at most)";
        goto refuse;
    }
    memcpy(out, stem, l);
    if (e)
        memcpy(out + l, ext, e);
    out[l + e] = 0;
    if (reason)
        *reason = NULL;
    return 0;
refuse:
    if (n)
        out[0] = 0;
    if (reason)
        *reason = why;
    return -1;
}

void fb_default_name(const char *kind, time_t t, const char *ext, char *out, size_t n)
{
    struct tm tm;
    char stamp[32];
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M", &tm);
    snprintf(out, n, "%s-%s%s", kind, stamp, ext ? ext : "");
}

/* stem (checked, no extension) with "-i" for i >= 2, and ext. */
static int nth_name(const char *stem, const char *ext, int i, char *out, size_t n)
{
    int w = i == 1 ? snprintf(out, n, "%s%s", stem, ext) : snprintf(out, n, "%s-%d%s", stem, i, ext);
    return w < 0 || (size_t)w >= n ? FB_E_RANGE : FB_OK;
}

/* The typed stem through fb_name_check, without the extension it adds. */
static int checked_stem(const char *stem, const char *ext, char *out, size_t n)
{
    if (fb_name_check(stem, ext, out, n, NULL))
        return FB_E_NAME;
    out[strlen(out) - strlen(ext)] = 0;
    return FB_OK;
}

int fb_unique_name(const char *dir, const char *stem, const char *ext, char *out, size_t n)
{
    char base[FB_NAME_MAX + FB_EXT_MAX + 1], name[FB_ENTRY_NAME_MAX], p[FB_PATH_MAX];
    int i;
    if (!ext)
        ext = "";
    if (checked_stem(stem, ext, base, sizeof base))
        return FB_E_NAME;
    for (i = 1; i < 1000; i++) {
        if (nth_name(base, ext, i, name, sizeof name) || fb_path_join(dir, name, p, sizeof p))
            return FB_E_RANGE;
        if (!fb_exists(p))
            return copy(out, n, name);
    }
    return FB_E_RANGE;
}

/* ---- files ---- */

/* The import read. The file is opened non-blocking and checked to be a
 * regular file before anything waits on it: a FIFO named *.toml would
 * otherwise hang the overlay's thread in open or read. nofollow refuses a
 * link at the last component (a resolved path that was swapped since). */
static int read_file(const char *path, size_t cap, char **out, size_t *len, int nofollow)
{
    FILE *f;
    char *buf;
    size_t got;
    uint64_t size;
#ifdef _WIN32
    struct _stat64 st;
    wchar_t w[FB_PATH_MAX];
    (void)nofollow; /* the resolved path came from the handle itself */
    if (!to_w(path, w, FB_PATH_MAX)) {
        errno = ENAMETOOLONG;
        return FB_E_IO;
    }
    f = _wfopen(w, L"rb");
    if (!f)
        return FB_E_IO;
    if (_fstat64(_fileno(f), &st)) {
        fclose(f);
        return FB_E_IO;
    }
    if (!(st.st_mode & _S_IFREG)) {
        fclose(f);
        errno = (st.st_mode & _S_IFDIR) ? EISDIR : EINVAL;
        return FB_E_IO;
    }
    size = (uint64_t)st.st_size;
#else
    struct stat st;
    int fl, fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | (nofollow ? O_NOFOLLOW : 0));
    if (fd < 0)
        return FB_E_IO;
    if (fstat(fd, &st)) {
        close(fd);
        return FB_E_IO;
    }
    if (!S_ISREG(st.st_mode)) {
        close(fd);
        errno = S_ISDIR(st.st_mode) ? EISDIR : EINVAL;
        return FB_E_IO;
    }
    size = (uint64_t)st.st_size;
    fl = fcntl(fd, F_GETFL);
    if (fl < 0 || fcntl(fd, F_SETFL, fl & ~O_NONBLOCK) < 0 || !(f = fdopen(fd, "rb"))) {
        int e = errno;
        close(fd);
        errno = e;
        return FB_E_IO;
    }
#endif
    /* the size of what is open, so a swap after a check cannot slip by */
    if (size > (uint64_t)cap) {
        fclose(f);
        return FB_E_TOO_BIG;
    }
    buf = malloc(cap + 2);
    if (!buf) {
        fclose(f);
        errno = ENOMEM;
        return FB_E_IO;
    }
    /* one byte more than the cap: a file that grew past it is refused too */
    got = fread(buf, 1, cap + 1, f);
    if (ferror(f)) {
        fclose(f);
        free(buf);
        errno = EIO;
        return FB_E_IO;
    }
    fclose(f);
    if (got > cap) {
        free(buf);
        return FB_E_TOO_BIG;
    }
    buf[got] = 0;
    *out = buf;
    if (len)
        *len = got;
    return FB_OK;
}

int fb_read_file(const char *path, size_t cap, char **out, size_t *len)
{
    return read_file(path, cap, out, len, 0);
}

int fb_import_entry(const struct file_browser *fb, int i, size_t cap, char **out, size_t *len)
{
    char p[FB_PATH_MAX], r[FB_PATH_MAX];
    if (fb->root < 0 || i < 0 || i >= fb->n)
        return FB_E_RANGE;
    if (fb->entries[i].is_dir)
        return FB_E_NOT_DIR;
    if (fb_path_join(fb->cwd, fb->entries[i].name, p, sizeof p))
        return FB_E_RANGE;
    /* resolved now, not trusted from the listing's link_ok */
    if (resolve(p, r, sizeof r))
        return FB_E_IO;
    if (!under_root(fb->roots[fb->root].real, r))
        return FB_E_ROOT;
    return read_file(r, cap, out, len, 1);
}

#ifndef _WIN32
/* tmp to path only while path is absent. link() fails with EEXIST
 * atomically; a file system without hard links (exFAT, some FUSE mounts)
 * gets the exclusive rename. Never a plain rename: that would replace. */
static int commit_new(const char *tmp, const char *path)
{
    if (!link(tmp, path)) {
        unlink(tmp);
        return 0;
    }
    if (errno == EEXIST)
        return -1;
#if defined(__APPLE__) && defined(RENAME_EXCL)
    return renamex_np(tmp, path, RENAME_EXCL) ? -1 : 0;
#elif defined(__linux__) && defined(SYS_renameat2)
    return syscall(SYS_renameat2, AT_FDCWD, tmp, AT_FDCWD, path, 1 /* RENAME_NOREPLACE */) ? -1
                                                                                          : 0;
#else
    return -1;
#endif
}
#endif

/* Write data to a fresh "<path>.<pid>-<k>.tmp" in path's folder, made with
 * O_EXCL and no link following (CREATE_NEW on Windows), so a file or a
 * link already under that name is never written through or truncated;
 * then commit it over path (replace) or only to an absent path. */
static int write_commit(const char *path, const void *data, size_t len, int replace)
{
    char tmp[FB_PATH_MAX];
    int k, e;
#ifdef _WIN32
    wchar_t wt[FB_PATH_MAX], wp[FB_PATH_MAX];
    HANDLE h = INVALID_HANDLE_VALUE;
    const char *p = data;
    size_t left = len;
    int ok = 1;
    if (!to_w(path, wp, FB_PATH_MAX)) {
        errno = ENAMETOOLONG;
        return FB_E_IO;
    }
    for (k = 0; k < 100; k++) {
        DWORD le;
        if (snprintf(tmp, sizeof tmp, "%s.%lu-%d.tmp", path, (unsigned long)GetCurrentProcessId(),
                     k) >= (int)sizeof tmp ||
            !to_w(tmp, wt, FB_PATH_MAX)) {
            errno = ENAMETOOLONG;
            return FB_E_IO;
        }
        h = CreateFileW(wt, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
            break;
        le = GetLastError();
        if (le != ERROR_FILE_EXISTS && le != ERROR_ALREADY_EXISTS)
            break;
    }
    if (h == INVALID_HANDLE_VALUE) {
        errno = err_from_win(GetLastError());
        return FB_E_IO;
    }
    while (ok && left) {
        DWORD chunk = left > 0x40000000u ? 0x40000000u : (DWORD)left, w = 0;
        ok = WriteFile(h, p, chunk, &w, NULL) && w == chunk;
        p += w;
        left -= w;
    }
    ok = ok && FlushFileBuffers(h);
    e = ok ? 0 : err_from_win(GetLastError());
    ok = CloseHandle(h) && ok;
    if (ok && MoveFileExW(wt, wp, MOVEFILE_WRITE_THROUGH | (replace ? MOVEFILE_REPLACE_EXISTING : 0)))
        return FB_OK;
    if (ok) {
        DWORD le = GetLastError();
        e = le == ERROR_ALREADY_EXISTS || le == ERROR_FILE_EXISTS ? EEXIST : err_from_win(le);
    }
    DeleteFileW(wt);
    errno = e ? e : EIO;
    return errno == EEXIST ? FB_E_EXISTS : FB_E_IO;
#else
    const char *p = data;
    size_t left = len;
    char dir[FB_PATH_MAX];
    char *slash;
    int fd = -1;
    for (k = 0; k < 100; k++) {
        if (snprintf(tmp, sizeof tmp, "%s.%ld-%d.tmp", path, (long)getpid(), k) >=
            (int)sizeof tmp) {
            errno = ENAMETOOLONG;
            return FB_E_IO;
        }
        fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0644);
        if (fd >= 0 || errno != EEXIST)
            break;
    }
    if (fd < 0)
        return FB_E_IO;
    while (left) {
        ssize_t w = write(fd, p, left);
        if (w < 0 && errno == EINTR)
            continue;
        if (w <= 0) {
            e = w < 0 ? errno : EIO;
            goto fail;
        }
        p += w;
        left -= (size_t)w;
    }
    if (fsync(fd)) {
        e = errno;
        goto fail;
    }
    if (close(fd)) {
        e = errno;
        fd = -1;
        goto fail;
    }
    fd = -1;
    if (replace ? rename(tmp, path) : commit_new(tmp, path)) {
        e = errno;
        goto fail;
    }
    /* the rename itself made durable; a failure here loses nothing */
    strcpy(dir, path);
    slash = strrchr(dir, '/');
    if (slash) {
        *(slash == dir ? slash + 1 : slash) = 0;
        fd = open(dir, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            fsync(fd);
            close(fd);
        }
    }
    return FB_OK;
fail:
    if (fd >= 0)
        close(fd);
    unlink(tmp); /* ours: made with O_EXCL above */
    errno = e;
    return e == EEXIST ? FB_E_EXISTS : FB_E_IO;
#endif
}

int fb_write_atomic(const char *path, const void *data, size_t len)
{
    return write_commit(path, data, len, 1);
}

int fb_write_new(const char *path, const void *data, size_t len)
{
    return write_commit(path, data, len, 0);
}

int fb_import_unique(const char *dir, const char *stem, const char *ext, const void *data,
                     size_t len, char *out, size_t n)
{
    char base[FB_NAME_MAX + FB_EXT_MAX + 1], name[FB_ENTRY_NAME_MAX], p[FB_PATH_MAX];
    int i, rc;
    if (!ext)
        ext = "";
    if (checked_stem(stem, ext, base, sizeof base))
        return FB_E_NAME;
    for (i = 1; i < 1000; i++) {
        if (nth_name(base, ext, i, name, sizeof name) || fb_path_join(dir, name, p, sizeof p))
            return FB_E_RANGE;
        rc = fb_write_new(p, data, len);
        if (rc == FB_E_EXISTS)
            continue;
        if (rc == FB_OK && copy(out, n, name))
            return FB_E_RANGE;
        return rc;
    }
    return FB_E_RANGE;
}

static int is_folder(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    DWORD a;
    if (!to_w(path, w, FB_PATH_MAX))
        return 0;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* ---- export ---- */

int fb_export_begin(struct fb_export *ex, const char *dir, const char *name, const char *ext)
{
    memset(ex, 0, sizeof *ex);
    if (fb_name_check(name, ext, ex->name, sizeof ex->name, &ex->reason))
        return ex->state = FB_EXP_REFUSED;
    if (fb_path_join(dir, ex->name, ex->path, sizeof ex->path)) {
        ex->reason = "The folder's path is too long";
        return ex->state = FB_EXP_REFUSED;
    }
    if (is_folder(ex->path)) {
        ex->reason = "A folder has that name";
        return ex->state = FB_EXP_REFUSED;
    }
    if (fb_exists(ex->path)) {
        snprintf(ex->prompt, sizeof ex->prompt, "Replace %s?", ex->name);
        ex->focus_yes = 0;
        return ex->state = FB_EXP_CONFIRM;
    }
    return ex->state = FB_EXP_READY;
}

void fb_export_answer(struct fb_export *ex, int yes)
{
    if (ex->state == FB_EXP_CONFIRM)
        ex->state = yes ? FB_EXP_READY : FB_EXP_CANCELLED;
}

int fb_export_write(struct fb_export *ex, const void *data, size_t len)
{
    if (ex->state != FB_EXP_READY)
        return ex->state;
    if (fb_write_atomic(ex->path, data, len)) {
        ex->reason = "Cannot write the file";
        return ex->state = FB_EXP_FAILED;
    }
    return ex->state = FB_EXP_DONE;
}

/* ---- host homes ---- */

int fb_home_pick(int under_wine, const char *unix_home, const char *userprofile,
                 fb_dos_path_fn to_dos, char *out, size_t n)
{
    if (under_wine && unix_home && *unix_home == '/') {
        size_t i, l = strlen(unix_home);
        if (to_dos && to_dos(unix_home, out, n))
            return 1;
        /* Wine without the export: Z: is the Unix root /, by Wine's default
         * dosdevices link. */
        if (l + 3 > n)
            return 0;
        out[0] = 'Z';
        out[1] = ':';
        for (i = 0; i <= l; i++)
            out[i + 2] = unix_home[i] == '/' ? '\\' : unix_home[i];
        return 1;
    }
    if (userprofile && *userprofile)
        return !copy(out, n, userprofile);
    if (unix_home && *unix_home)
        return !copy(out, n, unix_home);
    return 0;
}

#ifdef _WIN32
typedef WCHAR *(CDECL *wine_dos_fn)(const char *);

/* kernel32's wine_get_dos_file_name, found at run time so a plain Windows
 * build never needs it; its result is on the process heap. */
static int wine_to_dos(const char *unix_path, char *out, size_t n)
{
    HMODULE k = GetModuleHandleW(L"kernel32.dll");
    wine_dos_fn f = k ? (wine_dos_fn)(void (*)(void))GetProcAddress(k, "wine_get_dos_file_name")
                      : NULL;
    WCHAR *w;
    int ok;
    if (!f || !(w = f(unix_path)))
        return 0;
    ok = from_w(w, out, (int)n);
    HeapFree(GetProcessHeap(), 0, w);
    return ok;
}

static int env_u8(const wchar_t *name, char *out, int n)
{
    wchar_t w[FB_PATH_MAX];
    DWORD l = GetEnvironmentVariableW(name, w, FB_PATH_MAX);
    return l && l < FB_PATH_MAX && from_w(w, out, n);
}
#endif

int fb_under_wine(void)
{
#ifdef _WIN32
    HMODULE nt = GetModuleHandleW(L"ntdll.dll");
    return nt && GetProcAddress(nt, "wine_get_version") != NULL;
#else
    return 0;
#endif
}

int fb_home_dir(char *out, size_t n)
{
#ifdef _WIN32
    char home[FB_PATH_MAX], prof[FB_PATH_MAX];
    int wine = fb_under_wine();
    int h = wine && env_u8(L"HOME", home, sizeof home);
    int p = env_u8(L"USERPROFILE", prof, sizeof prof);
    return fb_home_pick(wine, h ? home : NULL, p ? prof : NULL, wine_to_dos, out, n);
#else
    const char *h = getenv("HOME");
    if (!h || !*h) {
        struct passwd *pw = getpwuid(getuid());
        h = pw ? pw->pw_dir : NULL;
    }
    return fb_home_pick(0, h, NULL, NULL, out, n);
#endif
}
