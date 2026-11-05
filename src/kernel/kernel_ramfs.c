/*
 * kernel_ramfs.c - the title's cache partition held in host memory
 *
 * See kernel_ramfs.h for why. The store is a flat list of nodes keyed by the
 * path below the partition root ("" is the root, '\' separators, matched
 * without case and kept as created), each file a growable buffer. A handle is
 * a real host event (manual reset, signalled), so the bridge's token table,
 * a wait on the handle and CloseHandle all behave as they do for a disk file;
 * a table here maps it to the open file object.
 *
 * It keeps the disk backend's semantics where a title can tell them apart:
 * the deferred FATX overwrite (kernel_file.c, s_overwrite_pending), POSIX
 * delete (the name goes at once, the bytes when the last handle closes), the
 * read and write status codes, the directory entry layout. Where the disk
 * backend only approximates the console, as with a full partition, it does
 * what FATX does: STATUS_DISK_FULL, and nothing written.
 *
 * A format does not clear the store. The bare device stays the partition
 * image (xbox_ramfs_create leaves it to the disk), so a title that formats
 * its cache partition writes the image and the files stay, as the disk
 * backend's files under <hdd>/Cache do. A title that formats a cache it
 * cannot mount rewrites what it needs afterwards either way.
 */
#include "kernel.h"
#include "kernel_ramfs.h"
#include "guest_det_device.h"
#include "mem_stats.h"
#include "recomp_env.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if !defined(_WIN32)
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#ifndef STATUS_DISK_FULL
#define STATUS_DISK_FULL                ((NTSTATUS)0xC000007FL)
#endif
#ifndef STATUS_NOT_SAME_DEVICE
#define STATUS_NOT_SAME_DEVICE          ((NTSTATUS)0xC00000D4L)
#endif
#ifndef STATUS_DIRECTORY_NOT_EMPTY
#define STATUS_DIRECTORY_NOT_EMPTY      ((NTSTATUS)0xC0000101L)
#endif
#ifndef STATUS_NOT_A_DIRECTORY
#define STATUS_NOT_A_DIRECTORY          ((NTSTATUS)0xC0000103L)
#endif
#ifndef STATUS_BUFFER_TOO_SMALL
#define STATUS_BUFFER_TOO_SMALL         ((NTSTATUS)0xC0000023L)
#endif
#ifndef STATUS_FILE_IS_A_DIRECTORY
#define STATUS_FILE_IS_A_DIRECTORY      ((NTSTATUS)0xC00000BAL)
#endif

#define RAMFS_DELETE_ON_CLOSE   0x00001000u
#define RAMFS_CLUSTER           16384ull     /* FATX: 512-byte sectors x 32 */
#define RAMFS_PATH              512

/* NtCreateFile's IoStatusBlock->Information. */
#define RAMFS_SUPERSEDED  0
#define RAMFS_OPENED      1
#define RAMFS_CREATED     2
#define RAMFS_OVERWRITTEN 3

extern uint32_t g_xbox_last_file_error;      /* kernel_file.c */

typedef struct ram_node {
    uint32_t      id;           /* creation order: the enumeration cursor */
    char         *path;
    int           is_dir;
    int           linked;       /* still has its name (not deleted)        */
    int           open_count;   /* file objects on it                      */
    uint64_t      size;
    uint8_t      *buf;          /* [size, buf_cap) is always zero          */
    uint64_t      buf_cap;
    LARGE_INTEGER ctime, mtime;
    xbox_host_char *seed;       /* host file still to be read in, or NULL  */
    int           seen;         /* named to the title by a listing or query */
} ram_node;

typedef struct ram_file {
    ram_node *node;
    int       refs;             /* handles: NtDuplicateObject shares one   */
    uint64_t  pos;
    int       write;
    int       append;
    int       pending;          /* deferred FATX overwrite (see below)     */
    int       delete_on_close;
    int       enum_started;
    int       enum_done;
    uint32_t  enum_last;
    char      pattern[64];
} ram_file;

typedef struct {
    HANDLE    h;
    ram_file *f;
} ram_handle;

static SRWLOCK      s_lock = SRWLOCK_INIT;
static volatile int s_enabled = -1;         /* -1: not yet resolved */
static int          s_started;              /* the store was used: on/off is fixed */
static uint64_t     s_cap;                  /* 0: stock */
static uint64_t     s_used;
static uint32_t     s_next_id;
static ram_node   **s_nodes;                /* in id order; [0] is the root */
static size_t       s_node_count, s_node_cap;
static ram_handle  *s_handles;
static size_t       s_handle_count, s_handle_cap;
static int          s_full_warned;
static int          s_seed_overflow;        /* the seed left something out */

/* Every change of the charge goes through here, so the [MEM] exit line
 * (mem_stats.h) can say what the store held at the end and at its peak. */
static void set_used(uint64_t v)
{
    s_used = v;
    mem_stats_ramcache_note(v);
}

/* ------------------------------------------------------------------------ */

void xbox_RamCacheEnable(int on)
{
    AcquireSRWLockExclusive(&s_lock);
    if (!s_started)
        s_enabled = on ? 1 : 0;
    ReleaseSRWLockExclusive(&s_lock);
}

void xbox_RamCacheSetCap(uint64_t bytes)
{
    AcquireSRWLockExclusive(&s_lock);
    if (!s_started)
        s_cap = bytes;
    ReleaseSRWLockExclusive(&s_lock);
}

uint64_t xbox_RamCacheUsed(void)
{
    uint64_t used;
    AcquireSRWLockExclusive(&s_lock);
    used = s_used;
    ReleaseSRWLockExclusive(&s_lock);
    return used;
}

static int word_is(const char *v, const char *w)
{
    while (*v && *w && tolower((unsigned char)*v) == *w) {
        v++;
        w++;
    }
    return !*v && !*w;
}

/* RECOMP_STORAGE_RAM_CACHE without the enhance layer: on unless the
 * variable says off, in the words enhance_cfg_bool takes, so a build with
 * and one without the layer read the same value the same way. A value that
 * is neither gets the layer's warning too; the layer then falls back to
 * config.toml, and here there is no file, so the default (on) holds. */
static int env_says_on(void)
{
    static const char *const off[] = { "0", "false", "no", "off" };
    static const char *const on[] = { "1", "true", "yes", "on" };
    static int warned;
    const char *v = recomp_env(RENV_STORAGE_RAM_CACHE);
    size_t i;
    if (!v || !*v)
        return 1;
    for (i = 0; i < sizeof off / sizeof off[0]; i++) {
        if (word_is(v, off[i]))
            return 0;
        if (word_is(v, on[i]))
            return 1;
    }
    if (!warned) {
        warned = 1;
        fprintf(stderr, "[FILE] %s=%s is not a bool (1/0, true/false, yes/no, on/off); "
                "ignored, the RAM cache stays on\n",
                recomp_env_name(RENV_STORAGE_RAM_CACHE), v);
    }
    return 1;
}

int xbox_ramfs_active(void)
{
    int on = s_enabled;
    if (on < 0) {
        on = env_says_on();
        AcquireSRWLockExclusive(&s_lock);
        if (s_enabled < 0)
            s_enabled = on;
        on = s_enabled;
        ReleaseSRWLockExclusive(&s_lock);
    }
    return on;
}

static uint64_t cap_bytes(void)
{
    return s_cap ? s_cap : XBOX_RAMFS_STOCK_CAP;
}

static uint64_t clusters(uint64_t bytes)
{
    return (bytes + RAMFS_CLUSTER - 1) / RAMFS_CLUSTER * RAMFS_CLUSTER;
}

static uint64_t node_charge(const ram_node *n)
{
    if (!n->path[0])
        return 0;               /* the root's cluster is part of the volume */
    return n->is_dir ? RAMFS_CLUSTER : clusters(n->size);
}

static void now_filetime(LARGE_INTEGER *out)
{
    out->QuadPart = (LONGLONG)(116444736000000000ULL
                               + (ULONGLONG)time(NULL) * 10000000ULL);
}

static void full_warning(const char *path)
{
    if (s_full_warned++)
        return;
    fprintf(stderr, "  [FILE] ram cache: full (%llu MB cap), %s not grown: "
            "STATUS_DISK_FULL, as a full partition answers\n",
            (unsigned long long)(cap_bytes() >> 20), path);
    fflush(stderr);
}

/* ---- paths ------------------------------------------------------------- */

static int path_eq(const char *a, const char *b)
{
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b))
            return 0;
        a++, b++;
    }
    return *a == *b;
}

/* '/' to '\', no leading, trailing or doubled separators. */
static int normalize(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in; in++) {
        char c = *in == '/' ? '\\' : *in;
        if (c == '\\' && (o == 0 || out[o - 1] == '\\'))
            continue;
        if (o + 1 >= n)
            return 0;
        out[o++] = c;
    }
    while (o && out[o - 1] == '\\')
        o--;
    out[o] = '\0';
    return 1;
}

static int join(const char *dir, const char *name, char *out, size_t n)
{
    char tmp[RAMFS_PATH * 2];
    snprintf(tmp, sizeof tmp, "%s\\%s", dir, name);
    return normalize(tmp, out, n);
}

static void parent_of(const char *path, char *out, size_t n)
{
    const char *cut = strrchr(path, '\\');
    size_t len = cut ? (size_t)(cut - path) : 0;
    if (len >= n)
        len = n - 1;
    memcpy(out, path, len);
    out[len] = '\0';
}

static const char *leaf_of(const char *path)
{
    const char *cut = strrchr(path, '\\');
    return cut ? cut + 1 : path;
}

static int is_relative(const char *name)
{
    return name[0] != '\\' && name[0] != '/' && !(name[0] && name[1] == ':');
}

/* Case-insensitive '*' and '?' match, as FindFirstFile and fnmatch with
 * FNM_CASEFOLD do for the names a title asks for. */
static int wild(const char *p, const char *s)
{
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*')
                p++;
            if (!p[1])
                return 1;
            for (; *s; s++)
                if (wild(p + 1, s))
                    return 1;
            return 0;
        }
        if (!*s)
            return 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s))
            return 0;
    }
    return *s == '\0';
}

/* ---- nodes ------------------------------------------------------------- */

static ram_node *find_node(const char *path)
{
    size_t i;
    for (i = 0; i < s_node_count; i++)
        if (path_eq(s_nodes[i]->path, path))
            return s_nodes[i];
    return NULL;
}

static ram_node *new_node(const char *path, int is_dir)
{
    ram_node *n;
    if (s_node_count == s_node_cap) {
        size_t cap = s_node_cap ? s_node_cap * 2 : 64;
        ram_node **grown = (ram_node **)realloc(s_nodes, cap * sizeof *grown);
        if (!grown)
            return NULL;
        s_nodes = grown;
        s_node_cap = cap;
    }
    n = (ram_node *)calloc(1, sizeof *n);
    if (!n || !(n->path = strdup(path))) {
        free(n);
        return NULL;
    }
    n->id = s_next_id++;
    n->is_dir = is_dir;
    n->linked = 1;
    now_filetime(&n->ctime);
    n->mtime = n->ctime;
    s_nodes[s_node_count++] = n;
    set_used(s_used + node_charge(n));
    return n;
}

static void free_node(ram_node *n)
{
    set_used(s_used - node_charge(n));
    free(n->buf);
    free(n->seed);
    free(n->path);
    free(n);
}

/* Remove the name. The bytes stay for the handles still open on it, as an
 * unlinked file's do on the host, and still count against the cap. */
static void unlink_node(ram_node *n)
{
    size_t i;
    for (i = 0; i < s_node_count; i++) {
        if (s_nodes[i] == n) {
            memmove(&s_nodes[i], &s_nodes[i + 1],
                    (s_node_count - i - 1) * sizeof *s_nodes);
            s_node_count--;
            break;
        }
    }
    n->linked = 0;
    if (!n->open_count)
        free_node(n);
}

static int dir_has_children(const ram_node *d)
{
    char parent[RAMFS_PATH];
    size_t i;
    for (i = 0; i < s_node_count; i++) {
        if (s_nodes[i] == d || !s_nodes[i]->path[0])
            continue;
        parent_of(s_nodes[i]->path, parent, sizeof parent);
        if (path_eq(parent, d->path))
            return 1;
    }
    return 0;
}

/* Tests: whether `path` is still a seed never opened, and its node order,
 * without the query marking it seen. */
int xbox_RamCacheSeedState(const char *path, uint32_t *order)
{
    char rel[RAMFS_PATH];
    ram_node *n;
    int state = -1;
    if (!normalize(path, rel, sizeof rel))
        return -1;
    AcquireSRWLockExclusive(&s_lock);
    n = find_node(rel);
    if (n) {
        state = n->seed ? 1 : 0;
        if (order)
            *order = n->id;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return state;
}

/* Room for `extra` more bytes of charge, dropping seeds if it takes
 * that. The partition is scratch, and <hdd>/Cache is shared: a dev
 * Cache dir held 360 stale BLiNX 2 pages and 300 BLiNX 1 ones, the seed
 * filled 747 MB of the 750 MB, and BLiNX 2 then could not grow its own time
 * record, so REW looped its 10-frame ring.
 *
 * Only when the seed itself overflowed (s_seed_overflow: a file or directory
 * was left out at the cap). Then the Cache dir is not a state a console could
 * hold, and its contents are the host's leftovers, not the title's. A seed
 * that fit is the title's own data from an earlier run: dropping one of its
 * files would lose it silently mid-run, where the cap answering
 * STATUS_DISK_FULL lets the title clean up itself, as on the console.
 *
 * What goes is a seed the title has not met this run: never opened (its bytes
 * still on the host, no handle on it) and never named to it by a listing or a
 * query (`seen`), so it cannot be counting on it. The oldest write time goes
 * first, ties to the first seeded; the host copy stays. A seed the title
 * opened or saw, and every file it made, is never dropped: past those the cap
 * answers STATUS_DISK_FULL, and a request that would fail anyway drops
 * nothing. Under the cap nothing is dropped.
 *
 * The charge never runs past the cap from seeding (directories are capped
 * too, seed_dir), so a seed's first open (seed_fill) re-charges what it
 * already held and never needs room. */
static int droppable(const ram_node *c, const ram_node *keep)
{
    return c != keep && c->seed && !c->seen && !c->is_dir && !c->open_count
        && c->size;
}

static uint64_t droppable_bytes(const ram_node *keep)
{
    uint64_t spare = 0;
    size_t i;
    if (!s_seed_overflow)
        return 0;
    for (i = 0; i < s_node_count; i++)
        if (droppable(s_nodes[i], keep))
            spare += node_charge(s_nodes[i]);
    return spare;
}

static int make_room(uint64_t extra, const ram_node *keep)
{
    size_t i;

    if (s_used + extra <= cap_bytes())
        return 1;
    if (s_used - droppable_bytes(keep) + extra > cap_bytes())
        return 0;
    while (s_used + extra > cap_bytes()) {
        ram_node *victim = NULL;
        for (i = 0; i < s_node_count; i++) {
            ram_node *c = s_nodes[i];
            if (droppable(c, keep)
                    && (!victim || c->mtime.QuadPart < victim->mtime.QuadPart))
                victim = c;
        }
        if (!victim)
            return 0;
        fprintf(stderr, "  [FILE] ram cache: %s (%llu bytes), a seed the title "
                "has not opened or listed, dropped to make room\n", victim->path,
                (unsigned long long)victim->size);
        fflush(stderr);
        unlink_node(victim);
    }
    return 1;
}

/* Grow or shrink a file to `size`. Growth past the cap writes nothing. */
static NTSTATUS set_size(ram_node *n, uint64_t size)
{
    uint64_t old_charge = node_charge(n), new_charge;

    if (size == n->size)
        return STATUS_SUCCESS;
    if (size < n->size) {
        memset(n->buf + size, 0, (size_t)(n->size - size));
        n->size = size;
        set_used(s_used - old_charge + node_charge(n));
        return STATUS_SUCCESS;
    }
    new_charge = clusters(size);
    if (s_used - old_charge + new_charge > cap_bytes()
            && !make_room(new_charge - old_charge, n)) {
        full_warning(n->path);
        return STATUS_DISK_FULL;
    }
    if (size > n->buf_cap) {
        /* Half again each time: a 2 MB cache file written 64 KB at a time
         * reallocates a dozen times, not thirty. Trimmed at the last close.
         * The cap counts clusters of the file size, as FATX does, not this
         * slack: host memory can run past the cap by up to half of each open
         * file, which the title never sees. */
        uint64_t cap = n->buf_cap + n->buf_cap / 2;
        uint8_t *grown;
        if (cap < size)
            cap = size;
        cap = (cap + 65535) & ~65535ull;
        if ((uint64_t)(size_t)cap != cap)
            return STATUS_DISK_FULL;
        grown = (uint8_t *)realloc(n->buf, (size_t)cap);
        if (!grown) {
            fprintf(stderr, "  [FILE] ram cache: out of host memory growing %s "
                    "to %llu bytes\n", n->path, (unsigned long long)size);
            fflush(stderr);
            return STATUS_DISK_FULL;
        }
        memset(grown + n->buf_cap, 0, (size_t)(cap - n->buf_cap));
        n->buf = grown;
        n->buf_cap = cap;
    }
    n->size = size;
    set_used(s_used - old_charge + new_charge);
    return STATUS_SUCCESS;
}

static void trim(ram_node *n)
{
    uint64_t want = (n->size + 65535) & ~65535ull;
    if (n->is_dir || n->buf_cap <= want)
        return;
    if (!want) {
        free(n->buf);
        n->buf = NULL;
        n->buf_cap = 0;
        return;
    }
    {
        uint8_t *shrunk = (uint8_t *)realloc(n->buf, (size_t)want);
        if (shrunk) {
            n->buf = shrunk;
            n->buf_cap = want;
        }
    }
}

/* ---- seeding from <hdd>/Cache ------------------------------------------ */

static unsigned s_seed_files;
static uint64_t s_seed_bytes;

static void seed_file(const char *rel, uint64_t size, LONGLONG ctime,
                      LONGLONG mtime, const xbox_host_char *host)
{
    ram_node *n;
    size_t len;

    if (find_node(rel))
        return;
    if (s_used + clusters(size) > cap_bytes()) {
        fprintf(stderr, "  [FILE] ram cache: %s (%llu bytes) left out, past "
                "the cap\n", rel, (unsigned long long)size);
        fflush(stderr);
        s_seed_overflow = 1;
        return;
    }
    n = new_node(rel, 0);
    if (!n)
        return;
#if defined(_WIN32)
    len = (wcslen(host) + 1) * sizeof(WCHAR);
#else
    len = strlen(host) + 1;
#endif
    n->seed = (xbox_host_char *)malloc(len);
    if (n->seed)
        memcpy(n->seed, host, len);
    set_used(s_used - node_charge(n));
    n->size = size;
    set_used(s_used + node_charge(n));
    n->ctime.QuadPart = ctime;
    n->mtime.QuadPart = mtime;
    s_seed_files++;
    s_seed_bytes += size;
}

/* A seeded directory costs a cluster like any other: past the cap it is left
 * out with what is in it, so the seed never charges more than the cap. */
static int seed_dir(const char *rel)
{
    if (find_node(rel))
        return 1;
    if (s_used + RAMFS_CLUSTER > cap_bytes()) {
        fprintf(stderr, "  [FILE] ram cache: %s\\ left out with its files, past "
                "the cap\n", rel);
        fflush(stderr);
        s_seed_overflow = 1;
        return 0;
    }
    return new_node(rel, 1) != NULL;
}

#if defined(_WIN32)
static void seed_scan(const WCHAR *dir, const char *rel, int depth)
{
    WCHAR pattern[MAX_PATH], child[MAX_PATH];
    WIN32_FIND_DATAW fd;
    HANDLE find;

    if (depth > 16 || _snwprintf(pattern, MAX_PATH, L"%ls\\*", dir) < 0)
        return;
    find = FindFirstFileW(pattern, &fd);
    if (find == INVALID_HANDLE_VALUE)
        return;
    do {
        char name[256], sub[RAMFS_PATH];
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L".."))
            continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, sizeof name,
                                 NULL, NULL)
                || !join(rel, name, sub, sizeof sub)
                || _snwprintf(child, MAX_PATH, L"%ls\\%ls", dir, fd.cFileName) < 0)
            continue;
        child[MAX_PATH - 1] = 0;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!seed_dir(sub))
                continue;
            seed_scan(child, sub, depth + 1);
        } else {
            uint64_t size = ((uint64_t)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
            LARGE_INTEGER c, m;
            c.LowPart = fd.ftCreationTime.dwLowDateTime;
            c.HighPart = (LONG)fd.ftCreationTime.dwHighDateTime;
            m.LowPart = fd.ftLastWriteTime.dwLowDateTime;
            m.HighPart = (LONG)fd.ftLastWriteTime.dwHighDateTime;
            seed_file(sub, size, c.QuadPart, m.QuadPart, child);
        }
    } while (FindNextFileW(find, &fd));
    FindClose(find);
}

static int seed_root(xbox_host_char *out, size_t n, char *shown, size_t shown_n)
{
    char root[MAX_PATH * 3];
    WCHAR wroot[MAX_PATH];
    if (!xbox_path_hdd_root(root, sizeof root)
            || !MultiByteToWideChar(CP_UTF8, 0, root, -1, wroot, MAX_PATH))
        return 0;
    if (_snwprintf(out, n, L"%ls\\Cache", wroot) < 0)
        return 0;
    out[n - 1] = 0;
    snprintf(shown, shown_n, "%s\\Cache", root);
    return 1;
}

static int seed_load(ram_node *n)
{
    HANDLE h = CreateFileW(n->seed, GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    uint64_t got = 0;
    if (h == INVALID_HANDLE_VALUE)
        return -1;
    while (got < n->size) {
        DWORD want = (DWORD)(n->size - got > 0x100000 ? 0x100000 : n->size - got);
        DWORD r = 0;
        if (!ReadFile(h, n->buf + got, want, &r, NULL) || r == 0)
            break;
        got += r;
    }
    CloseHandle(h);
    return got == n->size ? 0 : -1;
}
#else
static void seed_scan(const char *dir, const char *rel, int depth)
{
    DIR *d;
    struct dirent *de;

    if (depth > 16 || !(d = opendir(dir)))
        return;
    while ((de = readdir(d)) != NULL) {
        char child[MAX_PATH * 2], sub[RAMFS_PATH];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if ((size_t)snprintf(child, sizeof child, "%s/%s", dir, de->d_name)
                    >= sizeof child
                || !join(rel, de->d_name, sub, sizeof sub)
                || stat(child, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (!seed_dir(sub))
                continue;
            seed_scan(child, sub, depth + 1);
        } else if (S_ISREG(st.st_mode)) {
            LONGLONG c = (LONGLONG)(116444736000000000ULL
                                    + (ULONGLONG)st.st_ctime * 10000000ULL);
            LONGLONG m = (LONGLONG)(116444736000000000ULL
                                    + (ULONGLONG)st.st_mtime * 10000000ULL);
            seed_file(sub, (uint64_t)st.st_size, c, m, child);
        }
    }
    closedir(d);
}

static int seed_root(xbox_host_char *out, size_t n, char *shown, size_t shown_n)
{
    char root[MAX_PATH * 2];
    if (!xbox_path_hdd_root(root, sizeof root)
            || (size_t)snprintf(out, n, "%s/Cache", root) >= n)
        return 0;
    snprintf(shown, shown_n, "%s", out);
    return 1;
}

static int seed_load(ram_node *n)
{
    int fd = open(n->seed, O_RDONLY);
    uint64_t got = 0;
    if (fd < 0)
        return -1;
    while (got < n->size) {
        ssize_t r = read(fd, n->buf + got, (size_t)(n->size - got));
        if (r <= 0)
            break;
        got += (uint64_t)r;
    }
    close(fd);
    return got == n->size ? 0 : -1;
}
#endif

/* A seeded file's bytes come in on its first open, so a title that leaves
 * most of its old cache alone costs no reads and no memory for it. */
static void seed_fill(ram_node *n)
{
    uint64_t size = n->size;
    if (!n->seed)
        return;
    if (size) {
        uint64_t charge = node_charge(n);
        /* set_size from 0: the charge is already counted. */
        set_used(s_used - charge);
        n->size = 0;
        if (set_size(n, size) != STATUS_SUCCESS || seed_load(n) != 0) {
            fprintf(stderr, "  [FILE] ram cache: could not read the seed for "
                    "%s; it starts empty\n", n->path);
            fflush(stderr);
            set_size(n, 0);
        }
    }
    free(n->seed);
    n->seed = NULL;
}

/* First use, under the lock: the root, then the seed. */
static void start(void)
{
    xbox_host_char dir[MAX_PATH * 2];
    char shown[MAX_PATH * 3];

    if (s_started)
        return;
    s_started = 1;
    new_node("", 1);
    if (seed_root(dir, sizeof dir / sizeof dir[0], shown, sizeof shown)) {
        seed_scan(dir, "", 0);
        if (s_used > cap_bytes())
            s_seed_overflow = 1;
        fprintf(stderr, "  [FILE] ram cache: on, %u files %llu MB seeded from %s, "
                "cap %llu MB, nothing written back\n", s_seed_files,
                (unsigned long long)(s_seed_bytes >> 20), shown,
                (unsigned long long)(cap_bytes() >> 20));
    } else {
        fprintf(stderr, "  [FILE] ram cache: on, no HDD root to seed from, cap "
                "%llu MB\n", (unsigned long long)(cap_bytes() >> 20));
    }
    fflush(stderr);
}

/* ---- handles ----------------------------------------------------------- */

static ram_file *find_file(HANDLE h, size_t *slot)
{
    size_t i;
    if (!h || h == INVALID_HANDLE_VALUE)
        return NULL;
    for (i = 0; i < s_handle_count; i++) {
        if (s_handles[i].h == h) {
            if (slot)
                *slot = i;
            return s_handles[i].f;
        }
    }
    return NULL;
}

static HANDLE add_handle(ram_file *f)
{
    HANDLE h;
    if (s_handle_count == s_handle_cap) {
        size_t cap = s_handle_cap ? s_handle_cap * 2 : 32;
        ram_handle *grown = (ram_handle *)realloc(s_handles, cap * sizeof *grown);
        if (!grown)
            return NULL;
        s_handles = grown;
        s_handle_cap = cap;
    }
    /* Signalled and manual reset: a wait on a file handle is a wait for its
     * I/O, and every request here completes before it returns. */
    h = CreateEventW(NULL, TRUE, TRUE, NULL);
    if (!h)
        return NULL;
    s_handles[s_handle_count].h = h;
    s_handles[s_handle_count].f = f;
    s_handle_count++;
    f->refs++;
    return h;
}

/* A pending overwrite ends in a truncation to 0 (kernel_file.c). */
static void settle(ram_file *f)
{
    if (f->pending) {
        f->pending = 0;
        set_size(f->node, 0);
    }
}

static void release_file(ram_file *f)
{
    ram_node *n = f->node;
    if (--f->refs > 0)
        return;
    settle(f);
    if (f->delete_on_close && n->linked && n->path[0]
            && !(n->is_dir && dir_has_children(n)))
        unlink_node(n);
    if (--n->open_count == 0) {
        if (!n->linked)
            free_node(n);
        else
            trim(n);
    }
    free(f);
}

/* ---- resolving a guest name -------------------------------------------- */

/* 1: the store's, with the path in `rel`; 0: the disk's. Called without the
 * lock for the path table (it takes the symbolic-link table's), then with it
 * for a RootDirectory. `device` is set for a bare cache device. */
static int resolve_name(PXBOX_OBJECT_ATTRIBUTES oa, char *rel, size_t n, int *device)
{
    const char *name;
    char rest[RAMFS_PATH];
    int r;

    *device = 0;
    if (!oa || !oa->ObjectName || !oa->ObjectName->Buffer)
        return 0;
    name = oa->ObjectName->Buffer;
    if (oa->RootDirectory && is_relative(name)) {
        ram_file *dir;
        AcquireSRWLockExclusive(&s_lock);
        dir = find_file(oa->RootDirectory, NULL);
        r = dir && join(dir->node->path, name, rel, n);
        ReleaseSRWLockExclusive(&s_lock);
        return r;
    }
    r = xbox_path_cache_rest(name, rest, sizeof rest);
    if (r == 2) {
        *device = 1;
        rel[0] = '\0';
        return 1;
    }
    return r == 1 && normalize(rest, rel, n);
}

static void fill_net(const ram_node *nd, PXBOX_FILE_NETWORK_OPEN_INFORMATION info)
{
    info->CreationTime = nd->ctime;
    info->LastAccessTime = nd->mtime;
    info->LastWriteTime = nd->mtime;
    info->ChangeTime = nd->mtime;
    if (xbox_GuestDetOn())      /* the deterministic mode's fixed file times */
        info->CreationTime.QuadPart = info->LastAccessTime.QuadPart =
            info->LastWriteTime.QuadPart = info->ChangeTime.QuadPart =
            XBOX_DET_FILETIME;
    info->EndOfFile.QuadPart = nd->is_dir ? 0 : (LONGLONG)nd->size;
    info->AllocationSize.QuadPart = nd->is_dir ? 0 : (LONGLONG)clusters(nd->size);
    info->FileAttributes = nd->is_dir ? XBOX_FILE_ATTRIBUTE_DIRECTORY
                                      : XBOX_FILE_ATTRIBUTE_NORMAL;
}

static NTSTATUS fail_open(PXBOX_IO_STATUS_BLOCK ios, NTSTATUS st, uint32_t err)
{
    g_xbox_last_file_error = err;
    if (ios) {
        ios->Status = st;
        ios->Information = 0;
    }
    return st;
}

/* ---- the Nt*File entry points ------------------------------------------ */

int xbox_ramfs_create(PHANDLE out, ACCESS_MASK access, PXBOX_OBJECT_ATTRIBUTES oa,
                      PXBOX_IO_STATUS_BLOCK ios, ULONG disposition, ULONG options,
                      NTSTATUS *st)
{
    char rel[RAMFS_PATH], parent[RAMFS_PATH];
    int device, want_dir = (options & XBOX_FILE_DIRECTORY_FILE) != 0;
    int write = (access & (XBOX_GENERIC_WRITE | XBOX_GENERIC_ALL |
                           XBOX_FILE_WRITE_DATA | XBOX_FILE_APPEND_DATA)) != 0;
    ULONG info;
    ram_node *n;
    ram_file *f;

    if (!resolve_name(oa, rel, sizeof rel, &device))
        return 0;
    /* The bare device stays the partition image (a raw read, an IOCTL);
     * opened as a directory it is the volume, which is the store's root. */
    if (device && !want_dir)
        return 0;
    if (!out) {
        *st = STATUS_INVALID_PARAMETER;
        return 1;
    }
    if (g_xbox_path_hook && !device && !oa->RootDirectory)
        g_xbox_path_hook(oa->ObjectName->Buffer);

    AcquireSRWLockExclusive(&s_lock);
    start();
    n = find_node(rel);
    if (n) {
        if (want_dir && !n->is_dir) {
            *st = fail_open(ios, STATUS_NOT_A_DIRECTORY, 267);
            goto out;
        }
        if (n->is_dir && (options & XBOX_FILE_NON_DIRECTORY_FILE)) {
            *st = fail_open(ios, STATUS_FILE_IS_A_DIRECTORY, 5);
            goto out;
        }
        if (disposition == XBOX_FILE_CREATE) {
            *st = fail_open(ios, STATUS_OBJECT_NAME_COLLISION, 80);
            goto out;
        }
        if (n->is_dir && !want_dir
                && (write || (disposition != XBOX_FILE_OPEN
                              && disposition != XBOX_FILE_OPEN_IF))) {
            *st = fail_open(ios, STATUS_FILE_IS_A_DIRECTORY, 5);
            goto out;
        }
        info = disposition == XBOX_FILE_SUPERSEDE ? RAMFS_SUPERSEDED
             : (disposition == XBOX_FILE_OVERWRITE
                || disposition == XBOX_FILE_OVERWRITE_IF) ? RAMFS_OVERWRITTEN
             : RAMFS_OPENED;
        if (n->is_dir)
            info = RAMFS_OPENED;
    } else {
        ram_node *up;
        parent_of(rel, parent, sizeof parent);
        up = find_node(parent);
        if (!up || !up->is_dir) {
            *st = fail_open(ios, STATUS_OBJECT_PATH_NOT_FOUND, 3);
            goto out;
        }
        if (disposition == XBOX_FILE_OPEN || disposition == XBOX_FILE_OVERWRITE) {
            *st = fail_open(ios, STATUS_OBJECT_NAME_NOT_FOUND, 2);
            goto out;
        }
        if (want_dir && !make_room(RAMFS_CLUSTER, NULL)) {
            full_warning(rel);
            *st = fail_open(ios, STATUS_DISK_FULL, 112);
            goto out;
        }
        n = new_node(rel, want_dir);
        if (!n) {
            *st = fail_open(ios, STATUS_NO_MEMORY, 8);
            goto out;
        }
        info = RAMFS_CREATED;
    }

    f = (ram_file *)calloc(1, sizeof *f);
    if (!f) {
        /* A failed create leaves no name behind, as on disk. */
        if (info == RAMFS_CREATED)
            unlink_node(n);
        *st = fail_open(ios, STATUS_NO_MEMORY, 8);
        goto out;
    }
    /* Read in on an overwrite too: the deferred FATX overwrite can hand the
     * old bytes back through SetEndOfFile, so they must be here. */
    if (!n->is_dir)
        seed_fill(n);
    f->node = n;
    f->write = write || disposition == XBOX_FILE_SUPERSEDE
            || disposition == XBOX_FILE_CREATE || disposition == XBOX_FILE_OPEN_IF
            || disposition == XBOX_FILE_OVERWRITE
            || disposition == XBOX_FILE_OVERWRITE_IF;
    /* Append-only access appends. GENERIC_WRITE maps to FILE_GENERIC_WRITE,
     * which holds APPEND_DATA as well as WRITE_DATA, and such a handle writes
     * at the offset it is given on NT and FATX. */
    f->append = (access & XBOX_FILE_APPEND_DATA)
             && !(access & (XBOX_FILE_WRITE_DATA | XBOX_GENERIC_WRITE
                            | XBOX_GENERIC_ALL));
    f->delete_on_close = (options & RAMFS_DELETE_ON_CLOSE) != 0;
    /* The disk backend's deferred FATX overwrite, so a title sees the same
     * bytes either way: see s_overwrite_pending in kernel_file.c. */
    f->pending = !n->is_dir && info != RAMFS_OPENED && info != RAMFS_CREATED
              && n->size > 0;
    n->open_count++;
    *out = add_handle(f);
    if (!*out) {
        f->refs = 1;
        if (info == RAMFS_CREATED)
            f->delete_on_close = 1;     /* no name left behind */
        release_file(f);
        *st = fail_open(ios, STATUS_INSUFFICIENT_RESOURCES, 4);
        goto out;
    }
    if (ios) {
        ios->Status = STATUS_SUCCESS;
        ios->Information = info;
    }
    *st = STATUS_SUCCESS;
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_read(HANDLE h, HANDLE event, PXBOX_IO_STATUS_BLOCK ios,
                    PVOID buf, ULONG len, PLARGE_INTEGER off, NTSTATUS *st)
{
    ram_file *f;
    ram_node *n;
    uint64_t pos, got = 0;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    n = f->node;
    if (!ios) {
        *st = STATUS_INVALID_PARAMETER;
    } else if (n->is_dir) {
        ios->Status = *st = STATUS_INVALID_DEVICE_REQUEST;
        ios->Information = 0;
    } else {
        settle(f);
        pos = off && off->QuadPart >= 0 ? (uint64_t)off->QuadPart : f->pos;
        if (pos < n->size) {
            got = n->size - pos;
            if (got > len)
                got = len;
            memcpy(buf, n->buf + pos, (size_t)got);
        }
        f->pos = pos + got;
        ios->Information = (ULONG_PTR)got;
        if (got == 0 && len > 0) {
            ios->Status = *st = STATUS_END_OF_FILE;
        } else {
            ios->Status = *st = STATUS_SUCCESS;
            if (event)
                SetEvent(event);
        }
    }
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_write(HANDLE h, HANDLE event, PXBOX_IO_STATUS_BLOCK ios,
                     PVOID buf, ULONG len, PLARGE_INTEGER off, NTSTATUS *st)
{
    ram_file *f;
    ram_node *n;
    uint64_t pos;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    n = f->node;
    if (!ios) {
        *st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    ios->Information = 0;
    if (n->is_dir) {
        ios->Status = *st = STATUS_INVALID_DEVICE_REQUEST;
        goto out;
    }
    if (!f->write) {
        ios->Status = *st = STATUS_ACCESS_DENIED;
        goto out;
    }
    settle(f);
    pos = f->append ? n->size
        : off && off->QuadPart >= 0 ? (uint64_t)off->QuadPart : f->pos;
    if (pos + len > n->size) {
        NTSTATUS grown = set_size(n, pos + len);
        if (grown != STATUS_SUCCESS) {
            ios->Status = *st = grown;
            goto out;
        }
    }
    memcpy(n->buf + pos, buf, len);
    f->pos = pos + len;
    now_filetime(&n->mtime);
    ios->Information = len;
    ios->Status = *st = STATUS_SUCCESS;
    if (event)
        SetEvent(event);
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_close(HANDLE h)
{
    ram_file *f;
    size_t slot;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, &slot);
    if (f) {
        /* Out of the table before CloseHandle: the host can hand the same
         * value straight back for the next handle, which may be a disk one. */
        s_handles[slot] = s_handles[--s_handle_count];
        release_file(f);
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (f)
        CloseHandle(h);
    return f != NULL;
}

int xbox_ramfs_delete(PXBOX_OBJECT_ATTRIBUTES oa, NTSTATUS *st)
{
    char rel[RAMFS_PATH];
    int device;
    ram_node *n;

    if (!resolve_name(oa, rel, sizeof rel, &device) || device)
        return 0;
    AcquireSRWLockExclusive(&s_lock);
    start();
    n = find_node(rel);
    if (!n)
        *st = STATUS_OBJECT_NAME_NOT_FOUND;
    else if (!rel[0])
        *st = STATUS_ACCESS_DENIED;
    else if (n->is_dir && dir_has_children(n))
        *st = STATUS_DIRECTORY_NOT_EMPTY;
    else {
        unlink_node(n);
        *st = STATUS_SUCCESS;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_query_info(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                          ULONG len, XBOX_FILE_INFORMATION_CLASS cls, NTSTATUS *st)
{
    ram_file *f;
    ram_node *n;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    n = f->node;
    if (!ios || !info) {
        *st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    /* Not for a position query: XAPI SetEndOfFile makes one first. */
    if (cls != XboxFilePositionInformation)
        settle(f);
    switch (cls) {
        case XboxFileBasicInformation: {
            PXBOX_FILE_BASIC_INFORMATION b = (PXBOX_FILE_BASIC_INFORMATION)info;
            XBOX_FILE_NETWORK_OPEN_INFORMATION net;
            fill_net(n, &net);
            b->CreationTime = net.CreationTime;
            b->LastAccessTime = net.LastAccessTime;
            b->LastWriteTime = net.LastWriteTime;
            b->ChangeTime = net.ChangeTime;
            b->FileAttributes = net.FileAttributes;
            ios->Information = sizeof *b;
            break;
        }
        case XboxFileStandardInformation: {
            PXBOX_FILE_STANDARD_INFORMATION s = (PXBOX_FILE_STANDARD_INFORMATION)info;
            s->EndOfFile.QuadPart = n->is_dir ? 0 : (LONGLONG)n->size;
            s->AllocationSize.QuadPart = n->is_dir ? 0 : (LONGLONG)clusters(n->size);
            s->NumberOfLinks = n->linked ? 1 : 0;
            s->DeletePending = n->linked ? FALSE : TRUE;
            s->Directory = n->is_dir ? TRUE : FALSE;
            ios->Information = sizeof *s;
            break;
        }
        case XboxFilePositionInformation:
            ((PXBOX_FILE_POSITION_INFORMATION)info)->CurrentByteOffset.QuadPart =
                (LONGLONG)f->pos;
            ios->Information = sizeof(XBOX_FILE_POSITION_INFORMATION);
            break;
        case XboxFileInternalInformation:
            /* As the disk backends, for XAPI GetFileInformationByHandle: the
             * node id names the file for as long as the store lives. */
            if (len < sizeof(XBOX_FILE_INTERNAL_INFORMATION)) {
                *st = STATUS_INFO_LENGTH_MISMATCH;
                ios->Status = *st;
                ios->Information = 0;
                goto out;
            }
            ((PXBOX_FILE_INTERNAL_INFORMATION)info)->IndexNumber.QuadPart =
                (LONGLONG)n->id;
            ios->Information = sizeof(XBOX_FILE_INTERNAL_INFORMATION);
            break;
        case XboxFileNetworkOpenInformation:
            fill_net(n, (PXBOX_FILE_NETWORK_OPEN_INFORMATION)info);
            ios->Information = sizeof(XBOX_FILE_NETWORK_OPEN_INFORMATION);
            break;
        default:
            xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                     "ram cache NtQueryInformationFile: unhandled class %d", cls);
            *st = STATUS_NOT_IMPLEMENTED;
            goto out;
    }
    ios->Status = *st = STATUS_SUCCESS;
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_set_info(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                        ULONG len, XBOX_FILE_INFORMATION_CLASS cls, NTSTATUS *st)
{
    ram_file *f;
    ram_node *n;
    NTSTATUS r = STATUS_SUCCESS;

    (void)len;
    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    n = f->node;
    if (!ios || !info) {
        *st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    switch (cls) {
        case XboxFilePositionInformation: {
            LONGLONG pos = ((PXBOX_FILE_POSITION_INFORMATION)info)->CurrentByteOffset.QuadPart;
            if (pos < 0)
                r = STATUS_INVALID_PARAMETER;
            else
                f->pos = (uint64_t)pos;
            break;
        }
        case XboxFileEndOfFileInformation: {
            LONGLONG eof = ((PXBOX_FILE_END_OF_FILE_INFORMATION)info)->EndOfFile.QuadPart;
            /* A pending overwrite ends here without the truncation: the bytes
             * below the new end stay, as on FATX. */
            f->pending = 0;
            if (n->is_dir)
                r = STATUS_INVALID_PARAMETER;
            else if (eof < 0)
                r = STATUS_INVALID_PARAMETER;
            else if ((r = set_size(n, (uint64_t)eof)) == STATUS_SUCCESS)
                now_filetime(&n->mtime);
            break;
        }
        case XboxFileAllocationInformation: {
            /* Below the end of file it truncates; above, it only reserves,
             * which a buffer that grows on write does not need. */
            LONGLONG a = ((PXBOX_FILE_END_OF_FILE_INFORMATION)info)->EndOfFile.QuadPart;
            settle(f);
            if (!n->is_dir && a >= 0 && (uint64_t)a < n->size)
                r = set_size(n, (uint64_t)a);
            break;
        }
        case XboxFileDispositionInformation:
            /* As the POSIX backend's unlink: the name goes now. */
            if (((PXBOX_FILE_DISPOSITION_INFORMATION)info)->DeleteFile && n->linked) {
                if (!n->path[0])
                    r = STATUS_ACCESS_DENIED;
                else if (n->is_dir && dir_has_children(n))
                    r = STATUS_DIRECTORY_NOT_EMPTY;
                else
                    unlink_node(n);
            }
            break;
        case XboxFileBasicInformation:
            break;
        default:
            fprintf(stderr, "  [FILE] ram cache NtSetInformationFile: unhandled "
                    "class %d\n", (int)cls);
            fflush(stderr);
            *st = STATUS_NOT_IMPLEMENTED;
            goto out;
    }
    ios->Status = *st = r;
    ios->Information = 0;
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_query_volume(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info,
                            ULONG len, XBOX_FS_INFORMATION_CLASS cls, NTSTATUS *st)
{
    AcquireSRWLockExclusive(&s_lock);
    if (!find_file(h, NULL)) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    if (!ios || !info) {
        *st = STATUS_INVALID_PARAMETER;
    } else if (cls == XboxFileFsSizeInformation) {
        PXBOX_FILE_FS_SIZE_INFORMATION s = (PXBOX_FILE_FS_SIZE_INFORMATION)info;
        uint64_t cap = cap_bytes();
        s->BytesPerSector = 512;
        s->SectorsPerAllocationUnit = 32;
        s->TotalAllocationUnits.QuadPart = (LONGLONG)(cap / RAMFS_CLUSTER);
        /* What a write could get: the seeds make_room would drop count as
         * free, so a title that checks before it writes sees the room too. */
        uint64_t used = s_used - droppable_bytes(NULL);
        s->AvailableAllocationUnits.QuadPart =
            (LONGLONG)(used < cap ? (cap - used) / RAMFS_CLUSTER : 0);
        ios->Status = *st = STATUS_SUCCESS;
        ios->Information = sizeof *s;
    } else if (cls == XboxFileFsVolumeInformation) {
        /* Any fixed serial: the store is one volume for the whole run. */
        *st = xbox_FsVolumeInformation(ios, info, len, 0x52414D43u /* "RAMC" */, "");
    } else {
        xbox_log(XBOX_LOG_WARN, XBOX_LOG_FILE,
                 "ram cache NtQueryVolumeInformationFile: unhandled class %d", cls);
        *st = STATUS_NOT_IMPLEMENTED;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_flush(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, NTSTATUS *st)
{
    int mine;
    AcquireSRWLockExclusive(&s_lock);
    mine = find_file(h, NULL) != NULL;
    ReleaseSRWLockExclusive(&s_lock);
    if (!mine)
        return 0;
    if (ios) {
        ios->Status = STATUS_SUCCESS;
        ios->Information = 0;
    }
    *st = STATUS_SUCCESS;
    return 1;
}

int xbox_ramfs_query_full(PXBOX_OBJECT_ATTRIBUTES oa,
                          PXBOX_FILE_NETWORK_OPEN_INFORMATION info, NTSTATUS *st)
{
    char rel[RAMFS_PATH];
    int device;
    ram_node *n;

    if (!resolve_name(oa, rel, sizeof rel, &device) || device)
        return 0;
    if (!info) {
        *st = STATUS_INVALID_PARAMETER;
        return 1;
    }
    AcquireSRWLockExclusive(&s_lock);
    start();
    n = find_node(rel);
    if (n) {
        n->seen = 1;
        fill_net(n, info);
        *st = STATUS_SUCCESS;
    } else {
        *st = STATUS_OBJECT_NAME_NOT_FOUND;
    }
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_query_dir(HANDLE h, PXBOX_IO_STATUS_BLOCK ios, PVOID info, ULONG len,
                         XBOX_FILE_INFORMATION_CLASS cls, PXBOX_ANSI_STRING name,
                         BOOLEAN restart, NTSTATUS *st)
{
    ram_file *f;
    ram_node *hit = NULL;
    char parent[RAMFS_PATH];
    size_t i;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    if (!ios || !info) {
        *st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    ios->Information = 0;
    if (cls != XboxFileDirectoryInformation) {
        ios->Status = *st = STATUS_INVALID_INFO_CLASS;
        goto out;
    }
    if (!f->node->is_dir) {
        ios->Status = *st = STATUS_INVALID_PARAMETER;
        goto out;
    }
    if (restart || !f->enum_started) {
        f->enum_started = 1;
        f->enum_done = 0;
        f->enum_last = 0;       /* the root's id: every entry is after it */
        if (name && name->Buffer && name->Length > 0) {
            USHORT n = name->Length;
            if (n >= sizeof f->pattern)
                n = sizeof f->pattern - 1;
            memcpy(f->pattern, name->Buffer, n);
            f->pattern[n] = '\0';
        } else {
            strcpy(f->pattern, "*");
        }
        /* The Win32 backend's FindFirstFile reads *.* as every name. */
        if (!strcmp(f->pattern, "*.*"))
            strcpy(f->pattern, "*");
    }
    /* By id, after the last one returned: creating or deleting names during
     * the walk neither repeats nor skips the others. */
    if (!f->enum_done) {
        for (i = 0; i < s_node_count; i++) {
            ram_node *c = s_nodes[i];
            if (c->id <= f->enum_last || !c->path[0])
                continue;
            parent_of(c->path, parent, sizeof parent);
            if (path_eq(parent, f->node->path) && wild(f->pattern, leaf_of(c->path))) {
                hit = c;
                break;
            }
        }
    }
    if (!hit) {
        f->enum_done = 1;
        ios->Status = *st = STATUS_NO_MORE_FILES;
        goto out;
    }
    {
        PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
        XBOX_FILE_NETWORK_OPEN_INFORMATION net;
        const char *leaf = leaf_of(hit->path);
        ULONG header = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
        ULONG name_len = (ULONG)strlen(leaf);

        /* No room for the fixed part: nothing written and the entry stays
         * next, as the I/O manager answers. A name cut short still uses the
         * entry up, with STATUS_BUFFER_OVERFLOW and only the bytes written. */
        if (len < header) {
            ios->Status = *st = STATUS_BUFFER_TOO_SMALL;
            goto out;
        }
        f->enum_last = hit->id;
        hit->seen = 1;
        memset(e, 0, len);
        fill_net(hit, &net);
        e->CreationTime = net.CreationTime;
        e->LastAccessTime = net.LastAccessTime;
        e->LastWriteTime = net.LastWriteTime;
        e->ChangeTime = net.ChangeTime;
        e->EndOfFile = net.EndOfFile;
        e->AllocationSize = net.AllocationSize;
        e->FileAttributes = net.FileAttributes;
        e->FileNameLength = name_len;
        if (header + name_len <= len) {
            memcpy(e->FileName, leaf, name_len);
            ios->Status = *st = STATUS_SUCCESS;
            ios->Information = header + name_len;
        } else {
            memcpy(e->FileName, leaf, len - header);
            ios->Status = *st = STATUS_BUFFER_OVERFLOW;
            ios->Information = len;
        }
    }
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

/* The new name the way the disk backend resolves it: RootDirectory-relative,
 * a bare name within the file's own directory, or a full guest path. Returns
 * 1 with a store path, 0 for a name on disk, -1 for one that cannot be
 * resolved. Called with the lock held; the path table's symbolic-link lock
 * nests inside it and never the other way. */
static int rename_target(PXBOX_OBJECT_ATTRIBUTES nn, const char *own_dir,
                         char *rel, size_t n)
{
    const char *name = nn->ObjectName->Buffer;
    char rest[RAMFS_PATH];

    if (nn->RootDirectory) {
        ram_file *dir = find_file(nn->RootDirectory, NULL);
        if (!dir)
            return 0;
        return join(dir->node->path, name[0] == '\\' ? name + 1 : name, rel, n)
             ? 1 : -1;
    }
    if (!strchr(name, '\\') && !strchr(name, ':')) {
        if (!own_dir)
            return 0;
        return join(own_dir, name, rel, n) ? 1 : -1;
    }
    if (xbox_path_cache_rest(name, rest, sizeof rest) != 1)
        return 0;
    return normalize(rest, rel, n) ? 1 : -1;
}

int xbox_ramfs_rename(HANDLE h, PXBOX_OBJECT_ATTRIBUTES nn, BOOLEAN replace,
                      NTSTATUS *st)
{
    char dst[RAMFS_PATH], own[RAMFS_PATH], src[RAMFS_PATH];
    ram_file *f;
    ram_node *n, *old;
    char **names;
    int where;
    size_t i, src_len;

    if (!nn || !nn->ObjectName || !nn->ObjectName->Buffer
            || !nn->ObjectName->Buffer[0])
        return 0;

    AcquireSRWLockExclusive(&s_lock);
    f = find_file(h, NULL);
    if (!f) {
        /* A disk file renamed into the store: a different volume. */
        where = rename_target(nn, NULL, dst, sizeof dst);
        ReleaseSRWLockExclusive(&s_lock);
        if (where == 1) {
            *st = STATUS_NOT_SAME_DEVICE;
            return 1;
        }
        return 0;
    }
    n = f->node;
    parent_of(n->path, own, sizeof own);
    where = rename_target(nn, own, dst, sizeof dst);
    if (where == 0) {
        *st = STATUS_NOT_SAME_DEVICE;
        goto out;
    }
    if (where < 0 || !dst[0]) {
        *st = STATUS_OBJECT_NAME_INVALID;
        goto out;
    }
    if (!n->linked || !n->path[0]) {
        *st = STATUS_ACCESS_DENIED;
        goto out;
    }
    if (path_eq(n->path, dst)) {
        /* Only the case changes: keep the new spelling. */
        char *p = strdup(dst);
        if (p) {
            free(n->path);
            n->path = p;
        }
        *st = STATUS_SUCCESS;
        goto out;
    }
    parent_of(dst, own, sizeof own);
    {
        ram_node *up = find_node(own);
        if (!up || !up->is_dir) {
            *st = STATUS_OBJECT_PATH_NOT_FOUND;
            goto out;
        }
    }
    old = find_node(dst);
    if (old) {
        if (!replace) {
            *st = STATUS_OBJECT_NAME_COLLISION;
            goto out;
        }
        if (old->is_dir || n->is_dir) {
            *st = STATUS_ACCESS_DENIED;
            goto out;
        }
    }
    snprintf(src, sizeof src, "%s", n->path);
    src_len = strlen(src);
    /* Every new name is made before anything changes, so running out of
     * memory or path length leaves the tree as it was, never half moved.
     * names[i] is s_nodes[i]'s new path, NULL where it keeps its own. */
    names = (char **)calloc(s_node_count, sizeof *names);
    if (!names) {
        *st = STATUS_NO_MEMORY;
        goto out;
    }
    for (i = 0; i < s_node_count; i++) {
        ram_node *c = s_nodes[i];
        char moved[RAMFS_PATH], head[RAMFS_PATH];
        if (c == n) {
            names[i] = strdup(dst);
        } else if (n->is_dir && strlen(c->path) > src_len && c->path[src_len] == '\\') {
            /* A directory takes its subtree with it. */
            memcpy(head, c->path, src_len);
            head[src_len] = '\0';
            if (!path_eq(head, src))
                continue;
            if ((size_t)snprintf(moved, sizeof moved, "%s%s", dst, c->path + src_len)
                    >= sizeof moved) {
                *st = STATUS_OBJECT_NAME_INVALID;
                goto undo;
            }
            names[i] = strdup(moved);
        } else {
            continue;
        }
        if (!names[i]) {
            *st = STATUS_NO_MEMORY;
            goto undo;
        }
    }
    for (i = 0; i < s_node_count; i++) {
        if (names[i]) {
            free(s_nodes[i]->path);
            s_nodes[i]->path = names[i];
        }
    }
    free(names);
    /* Last: the file it replaces goes only once the move cannot fail. */
    if (old)
        unlink_node(old);
    XBOX_TRACE(XBOX_LOG_FILE, "ram cache rename %s -> %s", src, dst);
    *st = STATUS_SUCCESS;
    goto out;
undo:
    for (i = 0; i < s_node_count; i++)
        free(names[i]);
    free(names);
out:
    ReleaseSRWLockExclusive(&s_lock);
    return 1;
}

int xbox_ramfs_duplicate(HANDLE src, HANDLE *dup, int close_source)
{
    ram_file *f;
    size_t slot;

    *dup = NULL;
    AcquireSRWLockExclusive(&s_lock);
    f = find_file(src, NULL);
    if (!f) {
        ReleaseSRWLockExclusive(&s_lock);
        return 0;
    }
    *dup = add_handle(f);
    /* DUPLICATE_CLOSE_SOURCE closes the source even when the duplicate
     * fails, as DuplicateHandle does; the bridge then fails the call. */
    if (close_source && find_file(src, &slot)) {
        s_handles[slot] = s_handles[--s_handle_count];
        release_file(f);
    } else {
        close_source = 0;
    }
    ReleaseSRWLockExclusive(&s_lock);
    if (close_source)
        CloseHandle(src);
    return 1;
}
