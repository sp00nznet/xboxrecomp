/*
 * saves_store.c - list, export and import the title's save trees
 * (saves_store.h; openspec change overlay-saves, D5 and D6).
 */
#ifdef _WIN32
#include <windows.h>
#include <process.h>
#else
#include <sys/time.h>
#include <unistd.h>
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

#include "saves_store.h"

static const char *const k_tree[2] = { "UDATA", "TDATA" };

/* ---- host file calls the browser model has no use for ---- */

#ifdef _WIN32
static int to_w(const char *s, wchar_t *w, int cap)
{
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s, -1, w, cap) > 0;
}

/* 100 ns ticks since 1601 <-> Unix seconds */
static int64_t from_ft(FILETIME ft)
{
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    return (int64_t)(t / 10000000u) - 11644473600LL;
}
#endif

/* The kind of path without following a link: 1 a folder, 2 a regular
 * file, 3 anything else (a link, a device), 0 nothing there. */
static int path_kind(const char *path, int64_t *mtime)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!to_w(path, w, FB_PATH_MAX) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d))
        return 0;
    if (mtime) *mtime = from_ft(d.ftLastWriteTime);
    if (d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) return 3;
    return (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 1 : 2;
#else
    struct stat st;
    if (lstat(path, &st))
        return 0;
    if (mtime) *mtime = (int64_t)st.st_mtime;
    return S_ISDIR(st.st_mode) ? 1 : S_ISREG(st.st_mode) ? 2 : 3;
#endif
}

static void set_mtime(const char *path, int64_t t)
{
    if (t <= 0)
        return;
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    HANDLE h;
    uint64_t v = (uint64_t)(t + 11644473600LL) * 10000000u;
    FILETIME ft = { (DWORD)v, (DWORD)(v >> 32) };
    if (!to_w(path, w, FB_PATH_MAX))
        return;
    h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                    OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, NULL, &ft, &ft);
        CloseHandle(h);
    }
#else
    struct timeval tv[2] = { { (time_t)t, 0 }, { (time_t)t, 0 } };
    (void)utimes(path, tv);
#endif
}

static int (*s_rename_fail)(const char *from, const char *to);

void saves_store_rename_hook(int (*fail)(const char *from, const char *to))
{
    s_rename_fail = fail;
}

/* Rename a folder, never over something that is there. 0 or -1. */
static int rename_dir(const char *from, const char *to)
{
    if (s_rename_fail && s_rename_fail(from, to))
        return -1;
#ifdef _WIN32
    wchar_t a[FB_PATH_MAX], b[FB_PATH_MAX];
    return to_w(from, a, FB_PATH_MAX) && to_w(to, b, FB_PATH_MAX) && MoveFileExW(a, b, 0)
        ? 0 : -1;
#else
    if (path_kind(to, NULL))
        return -1;
    return rename(from, to) ? -1 : 0;
#endif
}

static int remove_one(const char *path, int dir)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    if (!to_w(path, w, FB_PATH_MAX))
        return -1;
    return (dir ? RemoveDirectoryW(w) : DeleteFileW(w)) ? 0 : -1;
#else
    return dir ? rmdir(path) : unlink(path);
#endif
}

/* Make one folder that must not be there yet. 0, 1 when it is, -1. */
static int make_dir_new(const char *path)
{
#ifdef _WIN32
    wchar_t w[FB_PATH_MAX];
    if (!to_w(path, w, FB_PATH_MAX))
        return -1;
    if (CreateDirectoryW(w, NULL))
        return 0;
    return GetLastError() == ERROR_ALREADY_EXISTS ? 1 : -1;
#else
    if (!mkdir(path, 0755))
        return 0;
    return errno == EEXIST ? 1 : -1;
#endif
}

struct names {
    char (*v)[FB_ENTRY_NAME_MAX];
    int n, cap;
};

static int collect(void *ctx, const struct fb_entry *e)
{
    struct names *s = ctx;
    if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
        return 0;
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 32;
        void *v = realloc(s->v, (size_t)c * sizeof *s->v);
        if (!v) return 1;
        s->v = v;
        s->cap = c;
    }
    memcpy(s->v[s->n], e->name, sizeof s->v[0]);
    s->n++;
    return 0;
}

static void names_free(struct names *s)
{
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* The names in a folder, sorted so an export is the same bytes for the
 * same saves; each one's kind is read again with path_kind, which does not
 * follow a link. 0, or an errno. */
static int cmp_name(const void *a, const void *b) { return strcmp(a, b); }
static int list_dir(const char *dir, struct names *s)
{
    int r;
    memset(s, 0, sizeof *s);
    r = fb_list_host(dir, collect, s, NULL);
    if (r) {
        names_free(s);
        return r;
    }
    if (s->n > 1) {
        char (*v)[FB_ENTRY_NAME_MAX] = s->v;
        qsort(v, (size_t)s->n, sizeof *v, cmp_name);
    }
    return 0;
}

/* Remove a tree; a link inside is removed, never followed. */
static int remove_tree(const char *path, int depth)
{
    struct names s;
    int kind = path_kind(path, NULL), rc = 0;
    if (!kind)
        return 0;
    if (kind != 1 || depth > 32) {
#ifdef _WIN32
        /* a folder link (a junction) is removed as a folder */
        if (kind == 3 && !remove_one(path, 1))
            return 0;
#endif
        return remove_one(path, 0);
    }
    if (list_dir(path, &s))
        return -1;
    for (int i = 0; i < s.n; i++) {
        char c[FB_PATH_MAX];
        if (fb_path_join(path, s.v[i], c, sizeof c) || remove_tree(c, depth + 1))
            rc = -1;
    }
    names_free(&s);
    return remove_one(path, 1) ? -1 : rc;
}

static int tid_dir(const struct saves_paths *p, int t, char *out, size_t n)
{
    char tid[16];
    snprintf(tid, sizeof tid, "%08x", (unsigned)p->tid);
    return p->root[t][0] && !fb_path_join(p->root[t], tid, out, n) ? 0 : -1;
}

/* ---- the listing ---- */

static struct saves_item *item_for(struct saves_info *o, const char *id)
{
    for (int i = 0; i < o->n; i++)
        if (!strcmp(o->item[i].id, id))
            return &o->item[i];
    o->saves++;
    if (o->n == SAVES_LIST_MAX)
        return NULL;
    memset(&o->item[o->n], 0, sizeof o->item[0]);
    snprintf(o->item[o->n].id, sizeof o->item[0].id, "%s", id);
    snprintf(o->item[o->n].name, sizeof o->item[0].name, "%s", id);
    return &o->item[o->n++];
}

/* Sizes and the newest time under dir, depth bounded; one save's when
 * it is non-NULL. Links are not followed. */
static int scan_tree(const char *dir, int depth, struct saves_info *o, struct saves_item *it)
{
    struct names s;
    int rc = 0;
    if (depth > SZ_MAX_DEPTH)
        return 0;
    if (list_dir(dir, &s))
        return -1;
    for (int i = 0; i < s.n; i++) {
        char c[FB_PATH_MAX];
        int64_t mt = 0;
        int k;
        if (fb_path_join(dir, s.v[i], c, sizeof c))
            continue;
        k = path_kind(c, &mt);
        if (k == 1) {
            if (scan_tree(c, depth + 1, o, it)) rc = -1;
        } else if (k == 2) {
            int64_t size = 0;
            struct stat st;
#ifdef _WIN32
            wchar_t w[FB_PATH_MAX];
            struct _stat64 ws;
            if (to_w(c, w, FB_PATH_MAX) && !_wstat64(w, &ws)) size = ws.st_size;
            (void)st;
#else
            if (!lstat(c, &st)) size = st.st_size;
#endif
            o->files++;
            o->bytes += (uint64_t)size;
            if (mt > o->mtime) o->mtime = mt;
            if (it) {
                it->bytes += (uint64_t)size;
                if (mt > it->mtime) it->mtime = mt;
                if (depth == 0 && !strcmp(s.v[i], "SaveMeta.xbx")) {
                    char *buf;
                    size_t len;
                    if (fb_read_file(c, 64 * 1024, &buf, &len) == FB_OK) {
                        saves_meta_name((const unsigned char *)buf, len, it->name,
                                        sizeof it->name);
                        if (!it->name[0])
                            snprintf(it->name, sizeof it->name, "%s", it->id);
                        free(buf);
                    }
                }
            }
        }
    }
    names_free(&s);
    return rc;
}

int saves_scan(const struct saves_paths *p, struct saves_info *o)
{
    char dir[FB_PATH_MAX];
    int rc = 0;
    memset(o, 0, sizeof *o);
    for (int t = 0; t < 2; t++) {
        struct names s;
        if (tid_dir(p, t, dir, sizeof dir) || path_kind(dir, NULL) != 1)
            continue;
        if (t == 1) {
            if (scan_tree(dir, 1, o, NULL)) rc = -1;
            continue;
        }
        /* UDATA/<tid>: each folder is one save; TitleMeta.xbx and
         * TitleImage.xbx beside them count with the files */
        if (list_dir(dir, &s)) {
            rc = -1;
            continue;
        }
        for (int i = 0; i < s.n; i++) {
            char c[FB_PATH_MAX];
            int64_t mt = 0;
            int k;
            if (fb_path_join(dir, s.v[i], c, sizeof c))
                continue;
            k = path_kind(c, &mt);
            if (k == 1) {
                struct saves_item *it = item_for(o, s.v[i]);
                struct saves_item spare;
                if (!it) {
                    memset(&spare, 0, sizeof spare);
                    it = &spare;
                }
                if (scan_tree(c, 0, o, it)) rc = -1;
            } else if (k == 2) {
                struct stat st;
                int64_t size = 0;
#ifdef _WIN32
                wchar_t w[FB_PATH_MAX];
                struct _stat64 ws;
                if (to_w(c, w, FB_PATH_MAX) && !_wstat64(w, &ws)) size = ws.st_size;
                (void)st;
#else
                if (!lstat(c, &st)) size = st.st_size;
#endif
                o->files++;
                o->bytes += (uint64_t)size;
                if (mt > o->mtime) o->mtime = mt;
            }
        }
        names_free(&s);
    }
    return rc;
}

struct id_ref {
    const char *s;
    size_t len;
};

static int cmp_id_ref(const void *a, const void *b)
{
    const struct id_ref *x = a, *y = b;
    size_t l = x->len < y->len ? x->len : y->len;
    int c = memcmp(x->s, y->s, l);
    return c ? c : (x->len > y->len) - (x->len < y->len);
}

void saves_scan_archive(const struct sz_archive *a, uint32_t tid, struct saves_info *o)
{
    /* Saves past the list are only counted: their ids are kept here and
     * counted once each at the end, so a zip of many saves stays linear
     * but for the sort. */
    struct id_ref *past = a->n ? malloc((size_t)a->n * sizeof *past) : NULL;
    int npast = 0;
    memset(o, 0, sizeof *o);
    for (int i = 0; i < a->n; i++) {
        const struct sz_entry *e = &a->e[i];
        int tree = 0;
        const char *rest = saves_zip_rest(e->name, tid, &tree);
        const char *slash;
        char id[SZ_NAME_MAX + 1];
        struct saves_item *it;
        if (!rest)
            continue;
        if (!e->is_dir) {
            o->files++;
            o->bytes += e->size;
            if (e->mtime > o->mtime) o->mtime = e->mtime;
        }
        if (tree != SAVES_TREE_UDATA || !(slash = strchr(rest, '/')))
            continue;   /* TDATA, or a file beside the saves */
        if ((size_t)(slash - rest) >= sizeof id)
            continue;
        memcpy(id, rest, (size_t)(slash - rest));
        id[slash - rest] = 0;
        if (!(it = item_for(o, id))) {
            /* item_for counted it; without memory each entry stays counted */
            if (past) {
                o->saves--;
                past[npast].s = rest;
                past[npast++].len = (size_t)(slash - rest);
            }
            continue;
        }
        if (!e->is_dir) {
            it->bytes += e->size;
            if (e->mtime > it->mtime) it->mtime = e->mtime;
            if (!strcmp(slash + 1, "SaveMeta.xbx")
                && saves_meta_name(e->data, e->size, it->name, sizeof it->name))
                snprintf(it->name, sizeof it->name, "%s", id);
        }
    }
    if (npast) {
        qsort(past, (size_t)npast, sizeof *past, cmp_id_ref);
        for (int i = 0; i < npast; i++)
            if (!i || cmp_id_ref(&past[i - 1], &past[i]))
                o->saves++;
    }
    free(past);
}


/* ---- export ---- */

struct item {
    char name[SZ_PATH_MAX];
    char *data;
    size_t len;
    int64_t mtime;
    int dir;
};

struct items {
    struct item *v;
    int n, cap;
    uint64_t bytes;
    int files;
};

static int items_add(struct items *s, const char *name, char *data, size_t len, int64_t mt,
                     int dir)
{
    if (s->n == s->cap) {
        int c = s->cap ? s->cap * 2 : 64;
        struct item *v = realloc(s->v, (size_t)c * sizeof *v);
        if (!v) return -1;
        s->v = v;
        s->cap = c;
    }
    if (strlen(name) >= SZ_PATH_MAX) return -1;
    snprintf(s->v[s->n].name, SZ_PATH_MAX, "%s", name);
    s->v[s->n].data = data;
    s->v[s->n].len = len;
    s->v[s->n].mtime = mt;
    s->v[s->n].dir = dir;
    s->n++;
    return 0;
}

static void items_free(struct items *s)
{
    for (int i = 0; i < s->n; i++)
        free(s->v[i].data);
    free(s->v);
    memset(s, 0, sizeof *s);
}

/* Collect dir as zip prefix (ending '/'), depth counted in the zip's
 * components. */
static int gather(const char *dir, const char *prefix, int depth, struct items *s,
                  char *err, size_t errn)
{
    struct names l;
    int64_t mt = 0;
    path_kind(dir, &mt);
    if (items_add(s, prefix, NULL, 0, mt, 1)) {
        snprintf(err, errn, "Out of memory collecting the saves");
        return -1;
    }
    if (list_dir(dir, &l)) {
        snprintf(err, errn, "Could not read %s", dir);
        return -1;
    }
    for (int i = 0; i < l.n; i++) {
        char c[FB_PATH_MAX], zn[SZ_PATH_MAX];
        const char *why;
        int k;
        if (fb_path_join(dir, l.v[i], c, sizeof c)
            || snprintf(zn, sizeof zn, "%s%s", prefix, l.v[i]) >= (int)sizeof zn - 1) {
            snprintf(err, errn, "A save path is too long (%s)", l.v[i]);
            goto fail;
        }
        k = path_kind(c, &mt);
        if (k != 1 && k != 2)
            continue;       /* a link or a special file is never a save */
        if (sz_name_ok(zn, &why)) {
            snprintf(err, errn, "A save file has %s (%s)", why, l.v[i]);
            goto fail;
        }
        if (k == 1) {
            if (depth + 1 >= SZ_MAX_DEPTH) {
                snprintf(err, errn, "The save folders are nested too deep");
                goto fail;
            }
            strcat(zn, "/");
            if (gather(c, zn, depth + 1, s, err, errn))
                goto fail;
        } else {
            char *buf = NULL;
            size_t len = 0;
            int r = fb_read_file(c, SZ_MAX_FILE, &buf, &len);
            if (r == FB_E_TOO_BIG) {
                snprintf(err, errn, "A save file is over 16 MiB (%s)", l.v[i]);
                goto fail;
            }
            if (r != FB_OK) {
                snprintf(err, errn, "Could not read %s", c);
                goto fail;
            }
            s->files++;
            s->bytes += len;
            if (s->bytes > SZ_MAX_TOTAL || s->n + 2 > SZ_MAX_ENTRIES) {
                free(buf);
                snprintf(err, errn, "The saves are too big for one zip");
                goto fail;
            }
            if (items_add(s, zn, buf, len, mt, 0)) {
                free(buf);
                snprintf(err, errn, "Out of memory collecting the saves");
                goto fail;
            }
        }
    }
    names_free(&l);
    return 0;
fail:
    names_free(&l);
    return -1;
}

int saves_export(const struct saves_paths *p, const struct saves_manifest *base,
                 unsigned char **zip, size_t *len, char *err, size_t errn)
{
    struct items s;
    struct sz_writer w;
    struct saves_manifest m = *base;
    char man[SZ_MANIFEST_MAX];
    int ml, any = 0;
    time_t now = time(NULL);

    memset(&s, 0, sizeof s);
    *zip = NULL;
    *len = 0;
    for (int t = 0; t < 2; t++) {
        char dir[FB_PATH_MAX], prefix[64];
        if (tid_dir(p, t, dir, sizeof dir) || path_kind(dir, NULL) != 1)
            continue;
        if (!any && items_add(&s, "hdd/", NULL, 0, (int64_t)now, 1))
            goto oom;
        any = 1;
        snprintf(prefix, sizeof prefix, "hdd/%s/", k_tree[t]);
        if (items_add(&s, prefix, NULL, 0, (int64_t)now, 1))
            goto oom;
        snprintf(prefix, sizeof prefix, "hdd/%s/%08x/", k_tree[t], (unsigned)p->tid);
        if (gather(dir, prefix, 3, &s, err, errn)) {
            items_free(&s);
            return -1;
        }
    }
    if (!s.files) {
        items_free(&s);
        return 0;
    }
    m.format = SAVES_FORMAT;
    snprintf(m.kind, sizeof m.kind, "saves");
    m.title_id = p->tid;
    m.files = s.files;
    m.bytes = s.bytes;
    if ((ml = saves_manifest_write(&m, man, sizeof man)) < 0) {
        items_free(&s);
        snprintf(err, errn, "The manifest does not fit");
        return -1;
    }
    sz_writer_init(&w);
    sz_add_file(&w, SZ_MANIFEST, man, (size_t)ml, (int64_t)now);
    for (int i = 0; i < s.n; i++) {
        if (s.v[i].dir)
            sz_add_dir(&w, s.v[i].name, s.v[i].mtime);
        else
            sz_add_file(&w, s.v[i].name, s.v[i].data, s.v[i].len, s.v[i].mtime);
    }
    items_free(&s);
    if (sz_finish(&w, zip, len)) {
        snprintf(err, errn, "The saves are too big for one zip");
        return -1;
    }
    return m.files;
oom:
    items_free(&s);
    snprintf(err, errn, "Out of memory collecting the saves");
    return -1;
}

/* ---- import ---- */

/* The folder a path is in, into out. */
static int parent_of(const char *path, char *out, size_t n)
{
    size_t l = strlen(path);
    while (l > 1 && (path[l - 1] == '/' || path[l - 1] == '\\'))
        l--;
    while (l > 0 && path[l - 1] != '/' && path[l - 1] != '\\')
        l--;
    while (l > 1 && (path[l - 1] == '/' || path[l - 1] == '\\'))
        l--;
    if (!l || l >= n)
        return -1;
    memcpy(out, path, l);
    out[l] = 0;
    return 0;
}

/* Join a zip-style rest ("a/b/c", '/'-separated) under dir. */
static int join_rest(const char *dir, const char *rest, char *out, size_t n)
{
    char tmp[FB_PATH_MAX];
    size_t l = strlen(rest);
    if (l >= sizeof tmp) return -1;
    memcpy(tmp, rest, l + 1);
    while (l && tmp[l - 1] == '/')
        tmp[--l] = 0;
    if (!l)
        return snprintf(out, n, "%s", dir) >= (int)n ? -1 : 0;
#ifdef _WIN32
    for (char *c = tmp; *c; c++)
        if (*c == '/') *c = '\\';
#endif
    return fb_path_join(dir, tmp, out, n);
}

int saves_import(const struct saves_paths *p, const struct sz_archive *a, int trees,
                 int (*busy)(void *ctx), void *ctx, char *err, size_t errn)
{
    char stage[2][FB_PATH_MAX], fresh[2][FB_PATH_MAX], old[2][FB_PATH_MAX],
         live[2][FB_PATH_MAX];
    int swapped[2] = { 0, 0 }, had_old[2] = { 0, 0 }, made[2] = { 0, 0 };
    unsigned long stamp = (unsigned long)time(NULL);
#ifdef _WIN32
    unsigned long pid = (unsigned long)_getpid();
#else
    unsigned long pid = (unsigned long)getpid();
#endif

    for (int t = 0; t < 2; t++) {
        char parent[FB_PATH_MAX], name[96];
        if (!(trees & (1 << t)))
            continue;
        if (tid_dir(p, t, live[t], sizeof live[t]) || parent_of(p->root[t], parent, sizeof parent)) {
            snprintf(err, errn, "The save folder is not known");
            goto undo_stage;
        }
        /* A folder of its own each time, made here and nowhere else: a
         * failed undo or a crash between the renames below leaves the old
         * saves in a stage folder, which a later import, even a retry in
         * the same second, must never reuse and delete. */
        if (fb_mkdirs(parent)) {
            snprintf(err, errn, "Could not make a folder beside the saves (%s)", parent);
            goto undo_stage;
        }
        for (int k = 0, rc = 1; rc; k++) {
            if (k == 100) {
                snprintf(err, errn, "Could not make a folder beside the saves (%s)", parent);
                goto undo_stage;
            }
            snprintf(name, sizeof name, ".saves-import-%lu-%lu-%d-%s", stamp, pid, k, k_tree[t]);
            if (fb_path_join(parent, name, stage[t], sizeof stage[t])
                || (rc = make_dir_new(stage[t])) < 0) {
                snprintf(err, errn, "Could not make a folder beside the saves (%s)", parent);
                goto undo_stage;
            }
        }
        made[t] = 1;
        if (fb_path_join(stage[t], "new", fresh[t], sizeof fresh[t])
            || fb_path_join(stage[t], "old", old[t], sizeof old[t])
            || make_dir_new(fresh[t])) {
            snprintf(err, errn, "Could not make a folder beside the saves (%s)", parent);
            goto undo_stage;
        }
    }

    /* stage every entry */
    for (int i = 0; i < a->n; i++) {
        const struct sz_entry *e = &a->e[i];
        char path[FB_PATH_MAX], dir[FB_PATH_MAX];
        int tree = 0, t;
        const char *rest = saves_zip_rest(e->name, p->tid, &tree);
        if (!rest)
            continue;
        t = tree == SAVES_TREE_UDATA ? 0 : 1;
        if (!made[t] || join_rest(fresh[t], rest, path, sizeof path)) {
            snprintf(err, errn, "A save path is too long (%s)", e->name);
            goto undo_stage;
        }
        if (e->is_dir) {
            if (fb_mkdirs(path)) {
                snprintf(err, errn, "Could not write %s", path);
                goto undo_stage;
            }
            continue;
        }
        if (parent_of(path, dir, sizeof dir) || fb_mkdirs(dir)
            || fb_write_new(path, e->data, e->size) != FB_OK) {
            snprintf(err, errn, "Could not write %s", path);
            goto undo_stage;
        }
        set_mtime(path, e->mtime);
    }

    if (busy && busy(ctx)) {
        snprintf(err, errn, "The game is saving; try again in a moment");
        goto undo_stage;
    }

    /* swap */
    for (int t = 0; t < 2; t++) {
        if (!made[t])
            continue;
        if (fb_mkdirs(p->root[t])) {
            snprintf(err, errn, "Could not make %s", p->root[t]);
            goto undo_swap;
        }
        if (path_kind(live[t], NULL)) {
            if (rename_dir(live[t], old[t])) {
#ifdef _WIN32
                /* A folder with an open handle under it does not move. */
                snprintf(err, errn, "Could not move the current saves aside (%s): the game "
                         "may have the folder open; quit to the title menu and try again",
                         live[t]);
#else
                snprintf(err, errn, "Could not move the current saves aside (%s)", live[t]);
#endif
                goto undo_swap;
            }
            had_old[t] = 1;
        }
        if (rename_dir(fresh[t], live[t])) {
            snprintf(err, errn, "Could not move the new saves in (%s)", live[t]);
            if (had_old[t] && !rename_dir(old[t], live[t]))
                had_old[t] = 0;
            goto undo_swap;
        }
        swapped[t] = 1;
    }
    for (int t = 0; t < 2; t++)
        if (made[t])
            remove_tree(stage[t], 0);
    return 0;

undo_swap:
    /* had_old[t] stays 1 while a tree's old saves are out of place; the
     * failing tree cleared it above when they went back. */
    for (int t = 1; t >= 0; t--) {
        if (!swapped[t] || rename_dir(live[t], fresh[t]))
            continue;   /* not swapped, or the zip's saves stay live */
        swapped[t] = 0;
        if (had_old[t] && !rename_dir(old[t], live[t]))
            had_old[t] = 0;
    }
    /* What the undo could not put back is said, never "nothing changed":
     * the live saves are then mixed, and the stage folder that keeps a
     * tree's old saves stays for the player. */
    for (int t = 0; t < 2; t++) {
        size_t l = strlen(err);
        if (!made[t] || l + 1 >= errn)
            continue;
        /* the path last, so it reads whole when the status line is cut */
        if (had_old[t] && swapped[t])
            snprintf(err + l, errn - l, "; undoing it failed too: %s holds the zip's saves, "
                     "and the old %s saves are kept in %s", k_tree[t], k_tree[t], old[t]);
        else if (had_old[t])
            snprintf(err + l, errn - l, "; undoing it failed too: the old %s saves are kept "
                     "in %s", k_tree[t], old[t]);
        else if (swapped[t])
            snprintf(err + l, errn - l, "; undoing it failed too: %s holds the zip's saves",
                     k_tree[t]);
    }
    for (int t = 0; t < 2; t++)
        if (made[t] && !had_old[t])
            remove_tree(stage[t], 0);
    return -1;
undo_stage:
    for (int t = 0; t < 2; t++)
        if (made[t])
            remove_tree(stage[t], 0);
    return -1;
}
