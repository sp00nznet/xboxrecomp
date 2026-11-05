/**
 * shader_cache.c - the on-disk shader bytecode cache (see shader_cache.h).
 *
 * Entry layout, little-endian:
 *   0  "XRSC"
 *   4  u32 format (SHADER_CACHE_FORMAT)
 *   8  u32 key length
 *  12  u32 code length
 *  16  u64 FNV-1a of the key
 *  24  u64 FNV-1a of the code
 *  32  the key, then the code
 * The key is serialised as u32-length-prefixed compiler, target and entry,
 * the two flag words, then the u32-length-prefixed source.
 */
#include "shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#include <direct.h>
#include <sys/utime.h>
#include <wchar.h>
#else
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#endif

#define HDR_SIZE 32u
#define FNV_BASIS 0xCBF29CE484222325ull
#define FNV_PRIME 0x100000001B3ull

static uint64_t fnv(uint64_t h, const void *p, size_t n)
{
    const uint8_t *b = (const uint8_t *)p;
    while (n--)
        h = (h ^ *b++) * FNV_PRIME;
    return h;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
         | (uint32_t)p[3] << 24;
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32;
}

/* The key's serialisation, written into buf (when non-NULL) and hashed in
 * one pass, so the hash never needs the buffer. Returns its length, or 0
 * when a part is too large. */
typedef struct { uint8_t *buf; size_t at; uint64_t h; } sink;

static void sink_put(sink *s, const void *p, size_t n)
{
    if (s->buf)
        memcpy(s->buf + s->at, p, n);
    s->h = fnv(s->h, p, n);
    s->at += n;
}

static void sink_u32(sink *s, uint32_t v)
{
    uint8_t b[4];
    put32(b, v);
    sink_put(s, b, 4);
}

static void sink_str(sink *s, const char *p, size_t n)
{
    sink_u32(s, (uint32_t)n);
    if (n)
        sink_put(s, p, n);
}

static size_t key_len(const shader_cache_key *k)
{
    size_t c = k->compiler ? strlen(k->compiler) : 0;
    size_t t = k->target ? strlen(k->target) : 0;
    size_t e = k->entry ? strlen(k->entry) : 0;
    if (c > SHADER_CACHE_MAX_PART || t > SHADER_CACHE_MAX_PART
     || e > SHADER_CACHE_MAX_PART || k->src_len > SHADER_CACHE_MAX_PART)
        return 0;
    if (16 + c + t + e + 8 + k->src_len > SHADER_CACHE_MAX_PART)
        return 0;
    return 16 + c + t + e + 8 + k->src_len;
}

static uint64_t key_write(const shader_cache_key *k, uint8_t *buf)
{
    sink s = { buf, 0, FNV_BASIS };
    sink_str(&s, k->compiler ? k->compiler : "", k->compiler ? strlen(k->compiler) : 0);
    sink_str(&s, k->target ? k->target : "", k->target ? strlen(k->target) : 0);
    sink_str(&s, k->entry ? k->entry : "", k->entry ? strlen(k->entry) : 0);
    sink_u32(&s, k->flags1);
    sink_u32(&s, k->flags2);
    sink_str(&s, k->src, k->src_len);
    return s.h;
}

uint64_t shader_cache_key_hash(const shader_cache_key *k)
{
    return key_write(k, NULL);
}

/* ---- host file system ---------------------------------------------------- */

#ifdef _WIN32
static int widen(const char *s, wchar_t *w, int n)
{
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n) > 0;
}

static FILE *fs_open(const char *path, const char *mode)
{
    wchar_t w[1024], m[8];
    if (!widen(path, w, 1024) || !widen(mode, m, 8))
        return NULL;
    return _wfopen(w, m);
}

static int fs_remove(const char *path)
{
    wchar_t w[1024];
    return widen(path, w, 1024) && _wremove(w) == 0;
}

/* MoveFileEx replaces an existing entry in one step (rename() would fail
 * on it). A reader holding the old entry open makes this fail: the store
 * then gives up, and the entry already there is as good. */
static int fs_rename(const char *from, const char *to)
{
    wchar_t a[1024], b[1024];
    return widen(from, a, 1024) && widen(to, b, 1024)
        && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING);
}

static int fs_mkdir_one(const char *path)
{
    wchar_t w[1024];
    DWORD a;
    if (!widen(path, w, 1024))
        return 0;
    if (CreateDirectoryW(w, NULL))
        return 1;
    a = GetFileAttributesW(w);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static unsigned fs_pid(void) { return (unsigned)GetCurrentProcessId(); }
#define IS_SEP(ch) ((ch) == '/' || (ch) == '\\')

static void fs_touch(const char *path)
{
    wchar_t w[1024];
    if (widen(path, w, 1024))
        _wutime(w, NULL);
}

/* Seconds since the epoch, from a FILETIME's 100 ns ticks since 1601. */
static int64_t ft_secs(FILETIME t)
{
    uint64_t v = (uint64_t)t.dwHighDateTime << 32 | t.dwLowDateTime;
    return (int64_t)(v / 10000000u) - 11644473600ll;
}

static int64_t fs_now(void)
{
    FILETIME t;
    GetSystemTimeAsFileTime(&t);
    return ft_secs(t);
}

/* Calls fn for every plain file in dir. */
typedef void (*fs_each_fn)(void *ctx, const char *name, uint64_t size, int64_t mtime);
static void fs_each(const char *dir, fs_each_fn fn, void *ctx)
{
    wchar_t w[1100];
    char pat[1100], name[MAX_PATH * 3];
    WIN32_FIND_DATAW fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    if (!widen(pat, w, 1100))
        return;
    h = FindFirstFileW(w, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            continue;
        if (!WideCharToMultiByte(CP_UTF8, 0, fd.cFileName, -1, name, (int)sizeof name, NULL, NULL))
            continue;
        fn(ctx, name, (uint64_t)fd.nFileSizeHigh << 32 | fd.nFileSizeLow,
           ft_secs(fd.ftLastWriteTime));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
#else
static FILE *fs_open(const char *path, const char *mode)
{
    return fopen(path, mode);
}

static int fs_remove(const char *path)
{
    return remove(path) == 0;
}

static int fs_rename(const char *from, const char *to)
{
    return rename(from, to) == 0;
}

static int fs_mkdir_one(const char *path)
{
    struct stat st;
    if (mkdir(path, 0777) == 0)
        return 1;
    return errno == EEXIST && stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static unsigned fs_pid(void) { return (unsigned)getpid(); }
#define IS_SEP(ch) ((ch) == '/')

static void fs_touch(const char *path)
{
    utime(path, NULL);
}

static int64_t fs_now(void)
{
    return (int64_t)time(NULL);
}

typedef void (*fs_each_fn)(void *ctx, const char *name, uint64_t size, int64_t mtime);
static void fs_each(const char *dir, fs_each_fn fn, void *ctx)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    struct stat st;
    char p[1400];
    if (!d)
        return;
    while ((e = readdir(d))) {
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode))
            fn(ctx, e->d_name, (uint64_t)st.st_size, (int64_t)st.st_mtime);
    }
    closedir(d);
}
#endif

/* mkdir -p: every prefix that ends at a separator, then the whole path. A
 * prefix that cannot be made (a drive root, a share) is skipped; only the
 * last one decides. */
static int fs_mkdirs(const char *path)
{
    char p[1024];
    size_t i, n = strlen(path);
    if (n == 0 || n >= sizeof p)
        return 0;
    memcpy(p, path, n + 1);
    for (i = 1; i < n; i++) {
        if (IS_SEP(p[i]) && !IS_SEP(p[i - 1]) && p[i - 1] != ':') {
            char ch = p[i];
            p[i] = 0;
            fs_mkdir_one(p);
            p[i] = ch;
        }
    }
    return fs_mkdir_one(p);
}

/* ---- the cache ----------------------------------------------------------- */

static int lower_eq(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char x = *a >= 'A' && *a <= 'Z' ? (char)(*a - 'A' + 'a') : *a;
        if (x != *b)
            return 0;
    }
    return *a == *b;
}

static int is_absolute(const char *v)
{
#ifdef _WIN32
    if (((v[0] >= 'A' && v[0] <= 'Z') || (v[0] >= 'a' && v[0] <= 'z'))
     && v[1] == ':' && IS_SEP(v[2]))
        return 1;
    return IS_SEP(v[0]) && IS_SEP(v[1]);     /* \\server\share */
#else
    return v[0] == '/';
#endif
}

shader_cache_env_kind shader_cache_env(const char *v)
{
    if (!v)
        return SHADER_CACHE_ENV_DEFAULT;
    if (!*v || *v == '0' || lower_eq(v, "off") || lower_eq(v, "false")
     || lower_eq(v, "no"))
        return SHADER_CACHE_ENV_OFF;
    if (!strcmp(v, "1") || lower_eq(v, "on") || lower_eq(v, "true")
     || lower_eq(v, "yes"))
        return SHADER_CACHE_ENV_DEFAULT;
    return is_absolute(v) ? SHADER_CACHE_ENV_DIR : SHADER_CACHE_ENV_RELATIVE;
}

/* A directory that exists but cannot be written (read-only media, another
 * user's) would cost a failed open per compile and say "on" in the log:
 * write and remove one temporary so init can say off instead. */
static int fs_writable(const char *dir)
{
    char p[1100];
    FILE *f;
    snprintf(p, sizeof p, "%s/probe.%u.tmp", dir, fs_pid());
    f = fs_open(p, "wb");
    if (!f)
        return 0;
    if (fclose(f) != 0) {
        fs_remove(p);
        return 0;
    }
    return fs_remove(p);
}

int shader_cache_init(shader_cache *c, const char *dir)
{
    size_t n;
    memset(c, 0, sizeof *c);
    if (!dir || !*dir)
        return 0;
    n = strlen(dir);
    while (n > 1 && IS_SEP(dir[n - 1]))
        n--;
    /* Room for the separator, 16 hex digits and the temporary suffix. */
    if (n + 64 >= sizeof c->dir)
        return 0;
    memcpy(c->dir, dir, n);
    c->dir[n] = 0;
    if (!fs_mkdirs(c->dir) || !fs_writable(c->dir)) {
        c->dir[0] = 0;
        return 0;
    }
    return 1;
}

int shader_cache_path(const shader_cache *c, const shader_cache_key *k,
                      char *out, size_t n)
{
    int w;
    if (!c->dir[0])
        return 0;
    w = snprintf(out, n, "%s/%016llx.bin", c->dir,
                 (unsigned long long)shader_cache_key_hash(k));
    return w > 0 && (size_t)w < n;
}

void *shader_cache_load(shader_cache *c, const shader_cache_key *k,
                        size_t *len)
{
    char path[1100];
    uint8_t hdr[HDR_SIZE], *key = NULL, *body = NULL;
    size_t klen = key_len(k);
    uint32_t mlen, clen;
    long size;
    uint64_t khash;
    FILE *f;
    int bad = 0;

    *len = 0;
    if (!klen || !shader_cache_path(c, k, path, sizeof path))
        return NULL;
    f = fs_open(path, "rb");
    if (!f) {
        c->misses++;
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0
     || fseek(f, 0, SEEK_SET) != 0 || fread(hdr, 1, HDR_SIZE, f) != HDR_SIZE) {
        bad = 1;
        goto out;
    }
    mlen = get32(hdr + 8);
    clen = get32(hdr + 12);
    if (memcmp(hdr, "XRSC", 4) || get32(hdr + 4) != SHADER_CACHE_FORMAT
     || mlen > SHADER_CACHE_MAX_PART || clen > SHADER_CACHE_MAX_PART || clen == 0
     || (uint64_t)size != (uint64_t)HDR_SIZE + mlen + clen) {
        bad = 1;
        goto out;
    }
    if (mlen != klen) {
        c->misses++;    /* another key under the same hash */
        goto out;
    }
    key = (uint8_t *)malloc(klen * 2);
    body = (uint8_t *)malloc(clen);
    if (!key || !body) {
        c->misses++;
        goto out;
    }
    khash = key_write(k, key);
    if (fread(key + klen, 1, klen, f) != klen || fread(body, 1, clen, f) != clen) {
        bad = 1;
        goto out;
    }
    if (get64(hdr + 16) != fnv(FNV_BASIS, key + klen, klen)
     || get64(hdr + 24) != fnv(FNV_BASIS, body, clen)) {
        bad = 1;        /* the bytes do not match their own header */
        goto out;
    }
    if (get64(hdr + 16) != khash || memcmp(key, key + klen, klen)) {
        c->misses++;    /* an intact entry for another key */
        goto out;
    }
    fclose(f);
    free(key);
    fs_touch(path);     /* what prune keeps is what was used last */
    c->hits++;
    *len = clen;
    return body;

out:
    fclose(f);
    free(key);
    free(body);
    if (bad) {
        c->corrupt++;
        c->misses++;
        fs_remove(path);
    }
    return NULL;
}

int shader_cache_store(shader_cache *c, const shader_cache_key *k,
                       const void *code, size_t len)
{
    char path[1100], tmp[1200];
    uint8_t hdr[HDR_SIZE], *key;
    size_t klen = key_len(k);
    uint64_t khash;
    FILE *f;
    int ok;

    if (!klen || !code || !len || len > SHADER_CACHE_MAX_PART
     || !shader_cache_path(c, k, path, sizeof path))
        return 0;
    key = (uint8_t *)malloc(klen);
    if (!key) {
        c->store_fails++;
        return 0;
    }
    khash = key_write(k, key);
    memcpy(hdr, "XRSC", 4);
    put32(hdr + 4, SHADER_CACHE_FORMAT);
    put32(hdr + 8, (uint32_t)klen);
    put32(hdr + 12, (uint32_t)len);
    put64(hdr + 16, khash);
    put64(hdr + 24, fnv(FNV_BASIS, code, len));
    /* Unique per process and store: two writers never share a temporary. */
    snprintf(tmp, sizeof tmp, "%s.%u-%u.tmp", path, fs_pid(), ++c->seq);
    f = fs_open(tmp, "wb");
    if (!f) {
        free(key);
        c->store_fails++;
        return 0;
    }
    ok = fwrite(hdr, 1, HDR_SIZE, f) == HDR_SIZE
      && fwrite(key, 1, klen, f) == klen
      && fwrite(code, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    free(key);
    if (!ok || !fs_rename(tmp, path)) {
        fs_remove(tmp);
        c->store_fails++;
        return 0;
    }
    c->stores++;
    return 1;
}

/* ---- pruning --------------------------------------------------------------- */

typedef struct { char name[64]; uint64_t size; int64_t mtime; } prune_ent;
typedef struct {
    const char *dir;
    shader_cache *c;
    prune_ent *e;
    size_t n, cap;
    uint64_t total;
    int64_t now;
} prune_ctx;

static int ends_with(const char *s, const char *suf)
{
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && !strcmp(s + a - b, suf);
}

static void prune_one(void *vp, const char *name, uint64_t size, int64_t mtime)
{
    prune_ctx *x = (prune_ctx *)vp;
    char p[1200];
    if (ends_with(name, ".tmp")) {
        /* A live store renames its temporary within milliseconds. */
        if (x->now - mtime > 3600) {
            snprintf(p, sizeof p, "%s/%s", x->dir, name);
            if (fs_remove(p))
                x->c->pruned++;
        }
        return;
    }
    /* Only the cache's own names: "<16 hex>.bin". */
    if (strlen(name) != 20 || !ends_with(name, ".bin"))
        return;
    if (x->n == x->cap) {
        size_t nc = x->cap ? x->cap * 2 : 256;
        prune_ent *ne = (prune_ent *)realloc(x->e, nc * sizeof *ne);
        if (!ne)
            return;
        x->e = ne;
        x->cap = nc;
    }
    memcpy(x->e[x->n].name, name, 21);
    x->e[x->n].size = size;
    x->e[x->n].mtime = mtime;
    x->n++;
    x->total += size;
}

static int by_mtime(const void *a, const void *b)
{
    int64_t x = ((const prune_ent *)a)->mtime, y = ((const prune_ent *)b)->mtime;
    return x < y ? -1 : x > y;
}

unsigned shader_cache_prune(shader_cache *c, uint64_t cap)
{
    prune_ctx x;
    unsigned before = c->pruned;
    size_t i;

    if (!c->dir[0])
        return 0;
    memset(&x, 0, sizeof x);
    x.dir = c->dir;
    x.c = c;
    x.now = fs_now();
    fs_each(c->dir, prune_one, &x);
    if (x.total > cap) {
        uint64_t keep = cap / 4 * 3;
        char p[1200];
        qsort(x.e, x.n, sizeof *x.e, by_mtime);
        for (i = 0; i < x.n && x.total > keep; i++) {
            snprintf(p, sizeof p, "%s/%s", c->dir, x.e[i].name);
            /* One held open elsewhere stays, and still counts: the next
             * oldest go in its place. */
            if (fs_remove(p)) {
                x.total -= x.e[i].size;
                c->pruned++;
            }
        }
    }
    free(x.e);
    return c->pruned - before;
}
