/* shader_cache_test - src/d3d/shader_cache.c against a temporary directory.
 *
 * The cache may only ever hand back the bytes stored for the very same key,
 * so beyond the round trip this checks that every key field changes the
 * entry, that a damaged entry is removed and reported as a miss (the
 * backend then compiles), that an intact entry for another key under the
 * same name is a miss and is kept, that no store leaves a temporary
 * file behind, whether it succeeds or not, and that pruning removes the
 * least recently used entries and stale temporaries, nothing else, and
 * counts only what it removed. Also: how RECOMP_DXBC_CACHE values read, and
 * that a directory that cannot be written leaves the cache off.
 */
#include "shader_cache.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <time.h>

#ifdef _WIN32
#include <windows.h>
#include <sys/utime.h>
#define SEP "\\"
#define utimbuf _utimbuf
#define utime _utime
#else
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utime.h>
#define SEP "/"
#endif

static int failures;

static void check(int ok, const char *what)
{
    printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok)
        failures++;
}

static int file_exists(const char *p)
{
    FILE *f = fopen(p, "rb");
    if (f)
        fclose(f);
    return f != NULL;
}

/* Files in dir whose name ends in suffix. */
static int count_files(const char *dir, const char *suffix)
{
    int n = 0;
    size_t sl = strlen(suffix);
#ifdef _WIN32
    char pat[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE)
        return 0;
    do {
        size_t l = strlen(fd.cFileName);
        if (l >= sl && !strcmp(fd.cFileName + l - sl, suffix))
            n++;
    } while (FindNextFileA(h, &fd));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return 0;
    while ((e = readdir(d))) {
        size_t l = strlen(e->d_name);
        if (l >= sl && !strcmp(e->d_name + l - sl, suffix))
            n++;
    }
    closedir(d);
#endif
    return n;
}

static void remove_dir(const char *dir)
{
#ifdef _WIN32
    char pat[1200], f[1200];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    h = FindFirstFileA(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.cFileName[0] == '.')
                continue;
            snprintf(f, sizeof f, "%s\\%s", dir, fd.cFileName);
            DeleteFileA(f);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(dir);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    char f[1200];
    if (d) {
        while ((e = readdir(d))) {
            if (e->d_name[0] == '.')
                continue;
            snprintf(f, sizeof f, "%s/%s", dir, e->d_name);
            remove(f);
        }
        closedir(d);
    }
    rmdir(dir);
#endif
}

static unsigned char *read_all(const char *p, long *n)
{
    FILE *f = fopen(p, "rb");
    unsigned char *b;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = (unsigned char *)malloc((size_t)*n);
    if (b && fread(b, 1, (size_t)*n, f) != (size_t)*n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

static void write_all(const char *p, const unsigned char *b, long n)
{
    FILE *f = fopen(p, "wb");
    if (!f)
        return;
    fwrite(b, 1, (size_t)n, f);
    fclose(f);
}

static const char SRC[] = "float4 main(float4 p : POSITION) : SV_Position { return p; }\n";
static const unsigned char CODE[] = "DXBC\x01\x02\x03\x04 pretend bytecode 0123456789";

static shader_cache_key base_key(void)
{
    shader_cache_key k;
    k.compiler = "C:\\windows\\system32\\d3dcompiler_47.dll|4488704|133000000000000000";
    k.target = "vs_5_0";
    k.entry = "main";
    k.flags1 = 1u << 15;
    k.flags2 = 0;
    k.src = SRC;
    k.src_len = sizeof SRC - 1;
    return k;
}

static void test_key(void)
{
    shader_cache_key a = base_key(), b;
    uint64_t h = shader_cache_key_hash(&a);

    printf("key\n");
    b = base_key();
    check(shader_cache_key_hash(&b) == h, "the same key hashes the same");
    b = base_key(); b.compiler = "C:\\other\\d3dcompiler_47.dll|1|2";
    check(shader_cache_key_hash(&b) != h, "the compiler is in the key");
    b = base_key(); b.target = "ps_5_0";
    check(shader_cache_key_hash(&b) != h, "the target is in the key");
    b = base_key(); b.entry = "vs";
    check(shader_cache_key_hash(&b) != h, "the entry point is in the key");
    b = base_key(); b.flags1 = 0;
    check(shader_cache_key_hash(&b) != h, "flags1 is in the key");
    b = base_key(); b.flags2 = 1;
    check(shader_cache_key_hash(&b) != h, "flags2 is in the key");
    b = base_key(); b.src_len--;
    check(shader_cache_key_hash(&b) != h, "the source is in the key");
    /* Length prefixes: moving a character between fields is another key. */
    a = base_key(); a.target = "vs_5_"; a.entry = "0main";
    b = base_key();
    check(shader_cache_key_hash(&a) != shader_cache_key_hash(&b),
          "field boundaries are in the key");
}

static void test_cache(const char *root)
{
    char dir[1024], path[1100], other[1100];
    shader_cache c, off;
    shader_cache_key k = base_key(), k2 = base_key();
    size_t n = 0;
    void *got;
    unsigned char *bytes;
    long len;

    printf("cache\n");
    snprintf(dir, sizeof dir, "%s" SEP "hdd" SEP "ShaderCache" SEP "d3d11", root);
    check(!shader_cache_init(&off, NULL) && !shader_cache_init(&off, ""),
          "no directory: the cache is off");
    check(!shader_cache_load(&off, &k, &n) && !shader_cache_store(&off, &k, CODE, sizeof CODE),
          "an off cache loads and stores nothing");
    check(shader_cache_init(&c, dir), "init makes the nested directory");
    check(shader_cache_path(&c, &k, path, sizeof path), "the entry has a path");

    check(!shader_cache_load(&c, &k, &n) && c.misses == 1 && !c.corrupt,
          "an empty cache misses");
    check(shader_cache_store(&c, &k, CODE, sizeof CODE) && file_exists(path),
          "store writes the entry");
    got = shader_cache_load(&c, &k, &n);
    check(got && n == sizeof CODE && !memcmp(got, CODE, n) && c.hits == 1,
          "load returns the stored bytes");
    free(got);
    check(count_files(dir, ".tmp") == 0, "no temporary file after a store");

    /* k2: a source of the same length, one character apart, so a load of
     * k against k2's entry gets past the length check to the comparison. */
    {
        static char src2[sizeof SRC];
        memcpy(src2, SRC, sizeof SRC);
        src2[0] = 'h';
        k2.src = src2;
    }
    check(!shader_cache_load(&c, &k2, &n), "another source misses");

    /* Store again over the entry: the rename replaces it. */
    check(shader_cache_store(&c, &k, CODE, sizeof CODE), "a second store replaces the entry");
    check(count_files(dir, ".bin") == 1 && count_files(dir, ".tmp") == 0,
          "one entry, no temporary file");

    /* An intact entry for k2 under k's name: a miss, and it stays. */
    bytes = read_all(path, &len);
    check(shader_cache_store(&c, &k2, CODE, 8), "store k2");
    shader_cache_path(&c, &k2, other, sizeof other);
    {
        long l2;
        unsigned char *b2 = read_all(other, &l2);
        if (b2) {
            write_all(path, b2, l2);
            free(b2);
        }
    }
    {
        unsigned corrupt = c.corrupt;
        check(!shader_cache_load(&c, &k, &n) && c.corrupt == corrupt && file_exists(path),
              "another key's intact entry under the same name misses and is kept");
    }

    /* Damaged entries: each one is a miss and is removed. */
    if (bytes) {
        static const struct { const char *what; long at; int trunc; } cases[] = {
            { "a flipped code byte",     -1, 0 },
            { "a flipped key byte",      40, 0 },
            { "a flipped magic",          0, 0 },
            { "another format",           4, 0 },
            { "a code length too large", 12, 0 },
            { "a truncated entry",        0, 1 },
            { "a header only",            0, 2 },
        };
        size_t i;
        char what[128];
        for (i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            unsigned corrupt = c.corrupt;
            unsigned char *b = (unsigned char *)malloc((size_t)len);
            long wl = len;
            memcpy(b, bytes, (size_t)len);
            if (cases[i].trunc == 1)
                wl = len - 3;
            else if (cases[i].trunc == 2)
                wl = 32;
            else
                b[cases[i].at < 0 ? len - 1 : cases[i].at] ^= 0x5A;
            write_all(path, b, wl);
            free(b);
            got = shader_cache_load(&c, &k, &n);
            snprintf(what, sizeof what, "%s: a miss, counted, entry removed", cases[i].what);
            check(!got && c.corrupt == corrupt + 1 && !file_exists(path), what);
            free(got);
        }
        /* And after the fallback compile, the store makes it whole again. */
        check(shader_cache_store(&c, &k, CODE, sizeof CODE), "store after a damaged entry");
        got = shader_cache_load(&c, &k, &n);
        check(got && n == sizeof CODE && !memcmp(got, CODE, n), "the new entry loads");
        free(got);
        free(bytes);
    } else {
        check(0, "read the entry back");
    }

    /* A store that cannot write (the directory is gone) leaves nothing. */
    remove_dir(dir);
    check(!shader_cache_store(&c, &k, CODE, sizeof CODE) && c.store_fails == 1,
          "a store into a missing directory fails and is counted");
    check(!file_exists(path), "and leaves no entry");
}

static void set_age(const char *p, long secs)
{
    struct utimbuf t;
    t.actime = t.modtime = time(NULL) - secs;
    utime(p, &t);
}

static void test_prune(const char *root)
{
    char dir[1024], p[4][1100], tmp_old[1200], tmp_new[1200], other[1200];
    static unsigned char code[1000];
    shader_cache c;
    shader_cache_key k[4];
    long size = 0;
    size_t n;
    int i;
    void *got;

    printf("prune\n");
    snprintf(dir, sizeof dir, "%s" SEP "prune", root);
    check(shader_cache_init(&c, dir), "init");
    for (i = 0; i < 4; i++) {
        k[i] = base_key();
        k[i].flags1 = (uint32_t)i;
        shader_cache_store(&c, &k[i], code, sizeof code);
        shader_cache_path(&c, &k[i], p[i], sizeof p[i]);
        set_age(p[i], (4 - i) * 1000L);     /* entry 0 the oldest */
    }
    {
        unsigned char *b = read_all(p[0], &size);
        free(b);
    }
    snprintf(tmp_old, sizeof tmp_old, "%s.1-1.tmp", p[3]);
    snprintf(tmp_new, sizeof tmp_new, "%s.1-2.tmp", p[3]);
    snprintf(other, sizeof other, "%s" SEP "notes.txt", dir);
    write_all(tmp_old, code, 10);
    write_all(tmp_new, code, 10);
    write_all(other, code, 10);
    set_age(tmp_old, 7200);

    check(shader_cache_prune(&c, (uint64_t)size * 8) == 1 && !file_exists(tmp_old)
          && file_exists(tmp_new), "under the cap: only the stale temporary goes");
    check(file_exists(p[0]) && file_exists(p[1]) && file_exists(p[2]) && file_exists(p[3]),
          "under the cap: every entry stays");

    /* A hit makes entry 0 the most recent, so 1 and 2 are the oldest. */
    got = shader_cache_load(&c, &k[0], &n);
    check(got != NULL, "entry 0 hits");
    free(got);
    /* Cap of three entries: trimmed to 2.25, so two go. */
    check(shader_cache_prune(&c, (uint64_t)size * 3) == 2, "over the cap: two entries go");
    check(file_exists(p[0]) && !file_exists(p[1]) && !file_exists(p[2]) && file_exists(p[3]),
          "the least recently used go, the hit stays");
    check(file_exists(other) && file_exists(tmp_new), "files that are not entries stay");
    remove(other);
    remove(tmp_new);
    remove_dir(dir);
}

static void test_env(void)
{
    static const char *const off[] = { "", "0", "00", "0x", "off", "OFF", "Off",
                                       "false", "FALSE", "no", "No" };
    static const char *const on[] = { "1", "on", "ON", "true", "True", "yes", "YES" };
    static const char *const rel[] = { "false2", "cache", "./cache", "nope", "2",
#ifdef _WIN32
                                       "/tmp/cache", "\\cache", "C:cache"
#else
                                       "C:\\cache", "cache/dxbc"
#endif
    };
    char what[96];
    size_t i;

    printf("RECOMP_DXBC_CACHE values\n");
    check(shader_cache_env(NULL) == SHADER_CACHE_ENV_DEFAULT, "unset: the default directory");
    for (i = 0; i < sizeof off / sizeof *off; i++) {
        snprintf(what, sizeof what, "\"%s\": off", off[i]);
        check(shader_cache_env(off[i]) == SHADER_CACHE_ENV_OFF, what);
    }
    for (i = 0; i < sizeof on / sizeof *on; i++) {
        snprintf(what, sizeof what, "\"%s\": the default directory", on[i]);
        check(shader_cache_env(on[i]) == SHADER_CACHE_ENV_DEFAULT, what);
    }
    for (i = 0; i < sizeof rel / sizeof *rel; i++) {
        snprintf(what, sizeof what, "\"%s\": not an absolute directory", rel[i]);
        check(shader_cache_env(rel[i]) == SHADER_CACHE_ENV_RELATIVE, what);
    }
#ifdef _WIN32
    check(shader_cache_env("C:\\x\\dxbc") == SHADER_CACHE_ENV_DIR, "\"C:\\x\\dxbc\": a directory");
    check(shader_cache_env("z:/home/x") == SHADER_CACHE_ENV_DIR, "\"z:/home/x\": a directory");
    check(shader_cache_env("\\\\srv\\share") == SHADER_CACHE_ENV_DIR, "UNC: a directory");
#else
    check(shader_cache_env("/tmp/dxbc") == SHADER_CACHE_ENV_DIR, "\"/tmp/dxbc\": a directory");
    check(shader_cache_env("/offline") == SHADER_CACHE_ENV_DIR, "\"/offline\": a directory");
#endif
}

/* Removals that fail are not counted (and the cap loop goes on to the next
 * oldest). POSIX: a directory without write permission; Windows: an entry
 * held open, which _wremove cannot delete. */
static void test_held(const char *root)
{
    char dir[1024], p[3][1100];
    static unsigned char code[1000];
    shader_cache c;
    shader_cache_key k[3];
    int i;

    printf("removals that fail\n");
    snprintf(dir, sizeof dir, "%s" SEP "held", root);
    check(shader_cache_init(&c, dir), "init");
    for (i = 0; i < 3; i++) {
        k[i] = base_key();
        k[i].flags1 = 100u + (uint32_t)i;
        shader_cache_store(&c, &k[i], code, sizeof code);
        shader_cache_path(&c, &k[i], p[i], sizeof p[i]);
        set_age(p[i], (3 - i) * 1000L);     /* entry 0 the oldest */
    }
#ifdef _WIN32
    {
        FILE *f = fopen(p[0], "rb");        /* the oldest, held */
        unsigned r = shader_cache_prune(&c, 1500);  /* keep 1125 bytes: one entry */
        check(r == 2 && file_exists(p[0]) && !file_exists(p[1]) && !file_exists(p[2]),
              "a held entry stays, uncounted, and the next oldest go");
        if (f)
            fclose(f);
    }
#else
    if (geteuid() == 0) {
        printf("  skip (root writes anywhere)\n");
    } else {
        unsigned r;
        chmod(dir, 0555);
        r = shader_cache_prune(&c, 1500);
        chmod(dir, 0755);
        check(r == 0 && c.pruned == 0, "nothing removed, nothing counted");
        check(file_exists(p[0]) && file_exists(p[1]) && file_exists(p[2]), "every entry stays");
    }
#endif
    for (i = 0; i < 3; i++)
        remove(p[i]);
    remove_dir(dir);

#ifndef _WIN32
    /* Wine ignores a directory's read-only bit, so this half is POSIX only. */
    printf("a directory that cannot be written\n");
    if (geteuid() == 0) {
        printf("  skip (root writes anywhere)\n");
    } else {
        snprintf(dir, sizeof dir, "%s" SEP "ro", root);
        mkdir(dir, 0555);
        check(!shader_cache_init(&c, dir) && !c.dir[0], "init says off");
        chmod(dir, 0755);
        remove_dir(dir);
    }
#endif
}

int main(int argc, char **argv)
{
    char root[512];

    if (argc > 1 && !freopen(argv[1], "w", stdout))
        return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#ifdef _WIN32
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        /* Wine reuses process ids from one run to the next. */
        do snprintf(root, sizeof root, "%sshcache-%lu-%u", tmp,
                    (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(root, NULL) && ++n < 1000);
        if (n == 1000)
            return 2;
    }
#else
    {
        char tmpl[] = "/tmp/shcacheXXXXXX";
        if (!mkdtemp(tmpl))
            return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    test_key();
    test_cache(root);
    test_prune(root);
    test_env();
    test_held(root);
    {
        char p[1100];
        snprintf(p, sizeof p, "%s" SEP "hdd" SEP "ShaderCache" SEP "d3d11", root);
        remove_dir(p);
        snprintf(p, sizeof p, "%s" SEP "hdd" SEP "ShaderCache", root);
        remove_dir(p);
        snprintf(p, sizeof p, "%s" SEP "hdd", root);
        remove_dir(p);
        remove_dir(root);
    }
    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures,
           failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
