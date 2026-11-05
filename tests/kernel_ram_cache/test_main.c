/*
 * The cache partition in memory (RECOMP_STORAGE_RAM_CACHE).
 *
 * BLiNX 1 rewrites a ring of 300 z:\pelonNNN.rec files (1.9 MB each) at about
 * one a second; on disk that is about 7 GB of host writes an hour. With the
 * store on, z: and Partition3-5 live in memory: capped at the stock 750 MB,
 * seeded read-only from <hdd>/Cache, never written back.
 *
 * Covered: on by default; off when the variable says 0 or false
 * (the RAMCACHE_OFF_VALUE builds) or when xbox_RamCacheEnable(0) runs with
 * the variable unset (RAMCACHE_OFF_ENABLE, the `ram_cache = false` case);
 * on, with a warning, when the variable is not a bool (RAMCACHE_BAD_VALUE).
 * A Cache dir over the cap (RAMCACHE_OVER_CAP): the seed stops at the
 * cap, and a write that needs room drops the seeds neither opened nor listed
 * nor queried, oldest first (ties: the first seeded), from subdirectories too,
 * never one the title met or its own files; free space counts them. A Cache
 * dir that fit (RAMCACHE_UNDER_CAP) never loses a seed; a directory past the
 * cap is left out like a file (RAMCACHE_DIR_OVER_CAP).
 * One way per process, since the store fixes on or off at its first use.
 * Then seed reads; create, rewrite, seek and size; the
 * FATX overwrite semantics the disk backends keep; the cap (STATUS_DISK_FULL)
 * and the volume size; delete and delete-on-close; directories and patterns;
 * rename; Partition3 reaching the same files; the volume and file index
 * queries; and the host Cache untouched.
 * Then the review's cases: a GENERIC_WRITE handle writing at its offset (on
 * the store and on the host backend), duplicates, a double close, shrink and
 * regrow, a deleted seed staying deleted, seeds listed unopened, a restarted
 * scan and a listing buffer too small for its entry. Last, the directory
 * searches abandoned after one result (kernel_fatx_overwrite's case, e5d407ea)
 * on the store.
 *
 * Cross-platform: it calls the xbox_Nt* file functions directly.
 */
#include "kernel.h"
#include "kernel_ramfs.h"
#include "mem_stats.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utime.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#define SEP_S "/"
#if defined(_WIN32)
#undef SEP_S
#define SEP_S "\\"
#endif

#define ST_NOT_FOUND     0xC0000034u
#define ST_COLLISION     0xC0000035u
#define ST_END_OF_FILE   0xC0000011u
#define ST_DISK_FULL     0xC000007Fu
#define ST_NO_MORE_FILES 0x80000006u
#define ST_NOT_SAME_DEV  0xC00000D4u
#define ST_BUF_OVERFLOW  0x80000005u
#define ST_BUF_TOO_SMALL 0xC0000023u

/* What XAPI's CreateFile(GENERIC_READ | GENERIC_WRITE) passes once mapped:
 * FILE_GENERIC_READ | FILE_GENERIC_WRITE, so WRITE_DATA and APPEND_DATA. */
#define ACC_GENERIC_RW   0x0012019Fu
/* FILE_APPEND_DATA | SYNCHRONIZE: an append-only handle. */
#define ACC_APPEND_ONLY  0x00100004u

#define CAP (1024u * 1024u)   /* 64 clusters of 16 KB */
#define CLUSTER 16384u

static int failures;
static char g_cache[512];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-60s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static void set_name(XBOX_ANSI_STRING *name, XBOX_OBJECT_ATTRIBUTES *oa,
                     const char *path, HANDLE root)
{
    name->Length = (USHORT)strlen(path);
    name->MaximumLength = (USHORT)(name->Length + 1);
    name->Buffer = (PCHAR)path;
    oa->RootDirectory = root;
    oa->ObjectName = name;
    oa->Attributes = 0x40;
}

static NTSTATUS open_at(HANDLE root, const char *path, ULONG disposition,
                        ULONG options, HANDLE *out, ULONG_PTR *info)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;

    set_name(&name, &oa, path, root);
    *out = NULL;
    ios.Information = 0xFFFF;
    st = xbox_NtCreateFile(out, 0xC0110000u /* R|W|DELETE|SYNC */, &oa, &ios,
                           NULL, 0x80, 0, disposition, options);
    if (info) *info = ios.Information;
    return st;
}

static NTSTATUS open_access(const char *path, ACCESS_MASK access, ULONG disposition,
                            HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;

    set_name(&name, &oa, path, NULL);
    *out = NULL;
    return xbox_NtCreateFile(out, access, &oa, &ios, NULL, 0x80, 0, disposition, 0x60);
}

static NTSTATUS open_file(const char *path, ULONG disposition, HANDLE *out)
{
    return open_at(NULL, path, disposition, 0x60, out, NULL);
}

static NTSTATUS set_eof(HANDLE h, LONGLONG size)
{
    XBOX_FILE_END_OF_FILE_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.EndOfFile.QuadPart = size;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileEndOfFileInformation);
}

static NTSTATUS set_pos(HANDLE h, LONGLONG pos)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.CurrentByteOffset.QuadPart = pos;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFilePositionInformation);
}

static LONGLONG query_pos(HANDLE h)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    if (xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                    XboxFilePositionInformation) != 0)
        return -1;
    return info.CurrentByteOffset.QuadPart;
}

static LONGLONG query_size(HANDLE h)
{
    XBOX_FILE_STANDARD_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    if (xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                    XboxFileStandardInformation) != 0)
        return -1;
    return info.EndOfFile.QuadPart;
}

/* off < 0: at the current position */
static NTSTATUS write_at(HANDLE h, const void *buf, ULONG len, LONGLONG off)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    o.QuadPart = off;
    return xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)buf, len,
                            off < 0 ? NULL : &o);
}

static NTSTATUS read_at(HANDLE h, void *buf, ULONG len, LONGLONG off, ULONG *got)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    NTSTATUS st;
    o.QuadPart = off;
    ios.Information = 0;
    st = xbox_NtReadFile(h, NULL, NULL, NULL, &ios, buf, len, off < 0 ? NULL : &o);
    if (got) *got = (ULONG)ios.Information;
    return st;
}

static NTSTATUS rename_to(HANDLE h, const char *path, BOOLEAN replace)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    set_name(&name, &oa, path, NULL);
    return xbox_RenameFileByHandle(h, &oa, replace);
}

static int exists(const char *path)
{
    HANDLE h;
    if (open_file(path, XBOX_FILE_OPEN, &h) != 0) return 0;
    xbox_NtClose(h);
    return 1;
}

static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + 1); }

static int matches(const unsigned char *b, unsigned from, unsigned n)
{
    unsigned i;
    for (i = 0; i < n; i++)
        if (b[i] != pattern(from + i)) return 0;
    return 1;
}

/* Host-side size of <cache>/<name>; -1 if it does not exist. */
static long host_size(const char *name)
{
    char p[700];
    FILE *f;
    long n;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    if (!(f = fopen(p, "rb"))) return -1;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fclose(f);
    return n;
}

static void host_write(const char *name, const unsigned char *buf, size_t n)
{
    char p[700];
    FILE *f;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    if ((f = fopen(p, "wb")) != NULL) {
        fwrite(buf, 1, n, f);
        fclose(f);
    }
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

/* Entries directly in the host Cache dir. */
static int host_entries(void)
{
    int n = 0;
#if defined(_WIN32)
    char p[700];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    snprintf(p, sizeof p, "%s\\*", g_cache);
    f = FindFirstFileA(p, &fd);
    if (f == INVALID_HANDLE_VALUE) return -1;
    do {
        if (strcmp(fd.cFileName, ".") && strcmp(fd.cFileName, "..")) n++;
    } while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    DIR *d = opendir(g_cache);
    struct dirent *e;
    if (!d) return -1;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, "..")) n++;
    closedir(d);
#endif
    return n;
}

/* Lists <dir> with <mask>; returns the names joined by ',' in out. */
static NTSTATUS list_dir(const char *dir, const char *mask, char *out, size_t n)
{
    static ULONG info[256];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    XBOX_ANSI_STRING m;
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    NTSTATUS st;
    HANDLE h;
    int first = 1, guard = 0;

    out[0] = 0;
    set_name(&name, &oa, dir, NULL);
    /* what XAPI's FindFirstFile asks for: list, synchronous, shared */
    st = xbox_NtCreateFile(&h, 0x00100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x21);
    if (st) return st;
    m.Length = (USHORT)strlen(mask);
    m.MaximumLength = (USHORT)(m.Length + 1);
    m.Buffer = (PCHAR)mask;
    for (;;) {
        memset(info, 0, sizeof info);
        st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, e, sizeof info,
                                       XboxFileDirectoryInformation, &m, first);
        if (st || ++guard > 64) break;
        snprintf(out + strlen(out), n - strlen(out), "%s%.*s", first ? "" : ",",
                 (int)e->FileNameLength, e->FileName);
        first = 0;
    }
    xbox_NtClose(h);
    return st;
}

static void test_on_by_default(void)
{
    HANDLE h;
    struct mem_stats m;
    printf("\nOn by default\n");
    check(xbox_ramfs_active(), "unset: the store is on", NULL);
    xbox_RamCacheSetCap(CAP);
    check(open_file("z:\\seed.bin", XBOX_FILE_OPEN, &h) == 0, "seed.bin opens", NULL);
    xbox_NtClose(h);
    xbox_RamCacheEnable(0);
    check(xbox_ramfs_active(), "turning it off after use is ignored", NULL);
    mem_stats_read(&m);
    /* seed.bin, sub, sub/s2.txt and sub/never.dat: a cluster each. */
    check(m.ramcache == CLUSTER * 4 && m.ramcache_peak == CLUSTER * 4,
          "the [MEM] line has the charge: the seed's four entries", NULL);
}

#if defined(RAMCACHE_OFF_VALUE) || defined(RAMCACHE_OFF_ENABLE)
/* Off by the variable (RECOMP_STORAGE_RAM_CACHE=RAMCACHE_OFF_VALUE) or, with
 * the variable unset, by xbox_RamCacheEnable(0) before the first use, which
 * is what enhance.c does for `ram_cache = false`: the store stays off and a
 * write on z: lands in <hdd>/Cache, as stock. */
static void test_explicit_off(void)
{
    HANDLE h;
    struct mem_stats m;
#ifdef RAMCACHE_OFF_VALUE
    printf("\nOff by the variable (%s)\n", RAMCACHE_OFF_VALUE);
    recomp_env_set(RENV_STORAGE_RAM_CACHE, RAMCACHE_OFF_VALUE);
#else
    printf("\nOff by xbox_RamCacheEnable(0), the variable unset\n");
    check(!recomp_env(RENV_STORAGE_RAM_CACHE), "the variable is unset", NULL);
    xbox_RamCacheEnable(0);
#endif
    check(!xbox_ramfs_active(), "the store is off", NULL);
    check(open_file("z:\\new.bin", XBOX_FILE_CREATE, &h) == 0, "z:\\new.bin made", NULL);
    check(write_at(h, "four", 4, 0) == 0, "and written", NULL);
    xbox_NtClose(h);
    check(host_size("new.bin") == 4, "on the host disk under Cache", NULL);
    mem_stats_read(&m);
    check(m.ramcache == MEM_STATS_NA, "no RAM cache fields in the [MEM] line", NULL);
}
#endif

#ifdef RAMCACHE_BAD_VALUE
/* RECOMP_STORAGE_RAM_CACHE=RAMCACHE_BAD_VALUE: not a bool, so it is warned
 * about and ignored; the store stays on and the host Cache gets nothing. */
static void test_bad_value(void)
{
    HANDLE h;
    printf("\nA variable that is not a bool (%s)\n", RAMCACHE_BAD_VALUE);
    recomp_env_set(RENV_STORAGE_RAM_CACHE, RAMCACHE_BAD_VALUE);
    check(xbox_ramfs_active(), "the store is on", NULL);
    check(open_file("z:\\new.bin", XBOX_FILE_CREATE, &h) == 0, "z:\\new.bin made", NULL);
    check(write_at(h, "four", 4, 0) == 0, "and written", NULL);
    xbox_NtClose(h);
    check(host_size("new.bin") < 0, "not on the host disk", NULL);
}
#endif

static void test_seed(const unsigned char *buf)
{
    static unsigned char got[4096];
    char d[160];
    HANDLE h;
    ULONG n = 0;
    NTSTATUS st;

    printf("\nSeeded from <hdd>/Cache\n");
    st = open_file("z:\\SEED.BIN", XBOX_FILE_OPEN, &h);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "z:\\SEED.BIN opens (names ignore case)", d);
    if (st) return;
    check(query_size(h) == 3000, "seed size 3000", NULL);
    st = read_at(h, got, sizeof got, 0, &n);
    check(st == 0 && n == 3000 && memcmp(got, buf, 3000) == 0,
          "seed bytes read back", NULL);
    xbox_NtClose(h);
    st = open_file("z:\\sub\\s2.txt", XBOX_FILE_OPEN, &h);
    check(st == 0, "seeded subdirectory file opens", NULL);
    if (!st) {
        st = read_at(h, got, 5, 0, &n);
        check(st == 0 && n == 5 && memcmp(got, "hello", 5) == 0, "its bytes", NULL);
        xbox_NtClose(h);
    }
    /* Rewrite the seed in memory; disk keeps its copy. */
    st = open_file("z:\\seed.bin", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, "new", 3, 0);
    check(query_size(h) == 3, "overwritten seed is 3 bytes in memory", NULL);
    xbox_NtClose(h);
    check(host_size("seed.bin") == 3000, "host seed.bin still 3000 bytes", NULL);
}

static void test_file_io(const unsigned char *buf)
{
    static unsigned char got[65536];
    char d[160];
    HANDLE h, h2;
    ULONG_PTR info = 0;
    ULONG n = 0;
    NTSTATUS st;

    printf("\nCreate, rewrite, seek, size\n");
    st = open_at(NULL, "z:\\pelon000.rec", XBOX_FILE_OVERWRITE_IF, 0x60, &h, &info);
    snprintf(d, sizeof d, "status 0x%08X info %lu", (unsigned)st, (unsigned long)info);
    check(st == 0 && info == 2, "create z:\\pelon000.rec (CREATED)", d);
    if (st) return;
    st = write_at(h, buf, 65536, 0);
    check(st == 0, "write 64 KB", NULL);
    check(query_pos(h) == 65536, "position after the write is 64 KB", NULL);
    xbox_NtClose(h);
    check(host_size("pelon000.rec") < 0, "nothing on the host disk", NULL);

    st = open_at(NULL, "z:\\pelon000.rec", XBOX_FILE_OPEN_IF, 0x60, &h, &info);
    check(st == 0 && info == 1, "reopen (OPENED)", NULL);
    check(query_size(h) == 65536, "size 64 KB", NULL);
    check(set_pos(h, 1000) == 0 && query_pos(h) == 1000, "seek to 1000", NULL);
    st = read_at(h, got, 10, -1, &n);
    check(st == 0 && n == 10 && matches(got, 1000, 10), "read 10 at the position", NULL);
    check(query_pos(h) == 1010, "position moved to 1010", NULL);
    st = read_at(h, got, 100, 65500, &n);
    check(st == 0 && n == 36 && matches(got, 65500, 36), "short read at the end", NULL);
    st = read_at(h, got, 10, 65536, &n);
    check(st == ST_END_OF_FILE, "read at the end: STATUS_END_OF_FILE", NULL);
    write_at(h, "XY", 2, -1);   /* at the current position, past the read */
    check(query_pos(h) == 65538, "write at the position extends", NULL);
    check(query_size(h) == 65538, "size 65538", NULL);
    check(set_eof(h, 100) == 0 && query_size(h) == 100, "set end of file to 100", NULL);
    /* a sparse write: the gap reads as zeros */
    write_at(h, "Z", 1, 200);
    st = read_at(h, got, 201, 0, &n);
    check(st == 0 && n == 201 && matches(got, 0, 100) && got[150] == 0 && got[200] == 'Z',
          "write past the end zero-fills the gap", NULL);
    xbox_NtClose(h);

    /* A second handle sees the first's writes. */
    open_file("z:\\pelon000.rec", XBOX_FILE_OPEN, &h);
    open_file("z:\\pelon000.rec", XBOX_FILE_OPEN, &h2);
    write_at(h, "Q", 1, 0);
    read_at(h2, got, 1, 0, &n);
    check(got[0] == 'Q', "two handles share the file", NULL);
    xbox_NtClose(h);
    xbox_NtClose(h2);

    st = open_file("z:\\pelon000.rec", XBOX_FILE_CREATE, &h);
    check(st == ST_COLLISION, "FILE_CREATE of an existing file: collision", NULL);
    st = open_file("z:\\missing.rec", XBOX_FILE_OPEN, &h);
    check(st == ST_NOT_FOUND, "FILE_OPEN of a missing file: not found", NULL);
    st = open_file("z:\\missing.rec", XBOX_FILE_OVERWRITE, &h);
    check(st != 0, "FILE_OVERWRITE of a missing file fails", NULL);
}

/* The texture-cache copy, as tests/kernel_fatx_overwrite runs it on disk. */
static void test_fatx_overwrite(const unsigned char *buf)
{
    static unsigned char got[4096];
    HANDLE h;
    ULONG n = 0;
    int same;
    unsigned i;

    printf("\nFATX overwrite\n");
    open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, &h);
    set_eof(h, 4096);
    write_at(h, buf, 4096, 0);
    xbox_NtClose(h);
    open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, &h);
    check(query_pos(h) == 0, "position query after the overwrite", NULL);
    check(set_eof(h, 3000) == 0, "set end of file to 3000", NULL);
    check(rename_to(h, "z:\\o.dat", TRUE) == 0, "rename to z:\\o.dat", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\o.tmp"), "o.tmp is gone", NULL);
    open_file("z:\\o.dat", XBOX_FILE_OPEN, &h);
    check(query_size(h) == 3000, "o.dat is 3000 bytes", NULL);
    read_at(h, got, 3000, 0, &n);
    check(n == 3000 && matches(got, 0, 3000), "the bytes below the new end survive", NULL);
    xbox_NtClose(h);

    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, buf, 4, 0);
    check(query_size(h) == 4, "overwrite + 4-byte write leaves 4 bytes", NULL);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE, &h);
    check(query_size(h) == 0, "overwrite + size query reports 0", NULL);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OPEN, &h);
    check(query_size(h) == 0, "overwrite + close leaves 0 bytes", NULL);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, &h);
    set_eof(h, 200);
    read_at(h, got, 200, 0, &n);
    same = n == 200;
    for (i = 0; same && i < 200; i++)
        if (got[i] != (i < 100 ? pattern(i) : 0)) same = 0;
    check(same, "regrow past the old end: old bytes, then zeros", NULL);
    xbox_NtClose(h);
}

/* The cap, the ring, and the volume size. */
static void test_cap(const unsigned char *buf)
{
    XBOX_FILE_FS_SIZE_INFORMATION fs;
    XBOX_IO_STATUS_BLOCK ios;
    char name[32], d[160];
    uint64_t used0, used;
    HANDLE h;
    NTSTATUS st;
    unsigned i;

    printf("\nThe cap\n");
    used0 = xbox_RamCacheUsed();
    open_file("z:\\big.rec", XBOX_FILE_OVERWRITE_IF, &h);
    st = set_eof(h, CAP + 1);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == ST_DISK_FULL, "end of file past the cap: STATUS_DISK_FULL", d);
    st = write_at(h, buf, 65536, (LONGLONG)CAP);
    check(st == ST_DISK_FULL, "write past the cap: STATUS_DISK_FULL", NULL);
    check(xbox_RamCacheUsed() == used0, "a refused write charges nothing", NULL);
    st = set_eof(h, (LONGLONG)(CAP - used0 - CLUSTER));
    check(st == 0, "a file up to the space left fits", NULL);
    st = xbox_NtQueryVolumeInformationFile(h, &ios, &fs, sizeof fs, XboxFileFsSizeInformation);
    snprintf(d, sizeof d, "total %lld avail %lld", (long long)fs.TotalAllocationUnits.QuadPart,
             (long long)fs.AvailableAllocationUnits.QuadPart);
    check(st == 0 && fs.TotalAllocationUnits.QuadPart == CAP / CLUSTER &&
          fs.AvailableAllocationUnits.QuadPart == 1 && fs.BytesPerSector == 512 &&
          fs.SectorsPerAllocationUnit == 32, "volume size: the cap, one cluster free", d);
    xbox_NtClose(h);
    open_file("z:\\big.rec", XBOX_FILE_OVERWRITE_IF, &h);
    xbox_NtClose(h);
    check(xbox_RamCacheUsed() == used0, "an overwrite gives the space back", NULL);

    /* BLiNX 1's ring: the same names rewritten over and over stay bounded. */
    for (i = 0; i < 200; i++) {
        snprintf(name, sizeof name, "z:\\pelon%03u.rec", 100 + i % 5);
        st = open_file(name, XBOX_FILE_OVERWRITE_IF, &h);
        if (st) break;
        if (write_at(h, buf, 65536, 0) != 0) st = 1;
        xbox_NtClose(h);
        if (st) break;
    }
    used = xbox_RamCacheUsed();
    snprintf(d, sizeof d, "%u rewrites, used %llu", i, (unsigned long long)used);
    check(i == 200 && used == used0 + 5 * 65536, "200 ring rewrites of 5 files: 5 files' worth", d);
    for (i = 0; i < 5; i++) {
        snprintf(name, sizeof name, "z:\\pelon%03u.rec", 100 + i);
        {
            XBOX_ANSI_STRING nm;
            XBOX_OBJECT_ATTRIBUTES oa;
            set_name(&nm, &oa, name, NULL);
            xbox_NtDeleteFile(&oa);
        }
    }
    check(xbox_RamCacheUsed() == used0, "deleting the ring gives the space back", NULL);
}

/* XAPI GetFileInformationByHandle asks for the volume and the file index
 * before the size; either refused, the whole call fails. */
static void test_info_classes(void)
{
    XBOX_FILE_FS_VOLUME_INFORMATION vol[2];
    XBOX_FILE_INTERNAL_INFORMATION ia, ib;
    XBOX_IO_STATUS_BLOCK ios;
    HANDLE a, b;
    NTSTATUS st, sa, sb;
    char d[160];

    printf("\nInfo classes\n");
    open_file("z:\\ia.rec", XBOX_FILE_OVERWRITE_IF, &a);
    open_file("z:\\ib.rec", XBOX_FILE_OVERWRITE_IF, &b);
    sa = xbox_NtQueryInformationFile(a, &ios, &ia, sizeof ia, XboxFileInternalInformation);
    sb = xbox_NtQueryInformationFile(b, &ios, &ib, sizeof ib, XboxFileInternalInformation);
    snprintf(d, sizeof d, "status 0x%08X 0x%08X, index %lld %lld", (unsigned)sa, (unsigned)sb,
             (long long)ia.IndexNumber.QuadPart, (long long)ib.IndexNumber.QuadPart);
    check(sa == 0 && sb == 0 && ios.Information == sizeof ib &&
          ia.IndexNumber.QuadPart != ib.IndexNumber.QuadPart,
          "file index: answered, one per file", d);
    ios.Status = 0;
    ios.Information = 99;
    st = xbox_NtQueryInformationFile(a, &ios, &ia, 4, XboxFileInternalInformation);
    snprintf(d, sizeof d, "status 0x%08X, ios 0x%08X/%u", (unsigned)st,
             (unsigned)ios.Status, (unsigned)ios.Information);
    check((uint32_t)st == 0xC0000004u && (uint32_t)ios.Status == 0xC0000004u &&
          ios.Information == 0, "file index: a 4-byte buffer is refused, in the status block too", d);
    memset(vol, 0xCC, sizeof vol);
    st = xbox_NtQueryVolumeInformationFile(a, &ios, vol, sizeof vol[0],
                                           XboxFileFsVolumeInformation);
    snprintf(d, sizeof d, "status 0x%08X, info %u, label %u", (unsigned)st,
             (unsigned)ios.Information, (unsigned)vol[0].VolumeLabelLength);
    check(st == 0 && ios.Information == 17 && vol[0].VolumeLabelLength == 0 &&
          vol[0].VolumeSerialNumber != 0 && vol[0].VolumeCreationTime.QuadPart == 0,
          "volume: answered, a serial and no label", d);
    xbox_NtClose(a);
    xbox_NtClose(b);
    {
        XBOX_ANSI_STRING nm;
        XBOX_OBJECT_ATTRIBUTES oa;
        set_name(&nm, &oa, "z:\\ia.rec", NULL);
        xbox_NtDeleteFile(&oa);
        set_name(&nm, &oa, "z:\\ib.rec", NULL);
        xbox_NtDeleteFile(&oa);
    }
}

static void test_delete(void)
{
    XBOX_FILE_DISPOSITION_INFORMATION di;
    XBOX_IO_STATUS_BLOCK ios;
    unsigned char got[8];
    HANDLE h, h2;
    ULONG n = 0;

    printf("\nDelete\n");
    open_at(NULL, "z:\\doc.tmp", XBOX_FILE_CREATE, 0x60 | 0x1000, &h, NULL);
    write_at(h, "abc", 3, 0);
    check(exists("z:\\doc.tmp"), "delete-on-close file exists while open", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\doc.tmp"), "and is gone after the close", NULL);

    open_file("z:\\disp.tmp", XBOX_FILE_CREATE, &h);
    write_at(h, "abc", 3, 0);
    open_file("z:\\disp.tmp", XBOX_FILE_OPEN, &h2);
    di.DeleteFile = TRUE;
    check(xbox_NtSetInformationFile(h, &ios, &di, sizeof di,
                                    XboxFileDispositionInformation) == 0,
          "set disposition delete", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\disp.tmp"), "the name goes at once", NULL);
    check(read_at(h2, got, 3, 0, &n) == 0 && n == 3 && memcmp(got, "abc", 3) == 0,
          "an open handle still reads it", NULL);
    xbox_NtClose(h2);
}

static void test_dirs(void)
{
    char names[512], d[600];
    HANDLE h, dir;
    NTSTATUS st;

    printf("\nDirectories\n");
    st = open_at(NULL, "z:\\d", XBOX_FILE_CREATE, XBOX_FILE_DIRECTORY_FILE | 0x20, &dir, NULL);
    check(st == 0, "create directory z:\\d", NULL);
    if (st) return;
    open_file("z:\\d\\a.txt", XBOX_FILE_CREATE, &h);
    xbox_NtClose(h);
    open_file("z:\\d\\b.dat", XBOX_FILE_CREATE, &h);
    xbox_NtClose(h);
    /* relative to the directory handle */
    st = open_at(dir, "c.txt", XBOX_FILE_CREATE, 0x60, &h, NULL);
    check(st == 0, "create c.txt relative to the directory handle", NULL);
    xbox_NtClose(h);
    xbox_NtClose(dir);

    st = list_dir("z:\\d", "*", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\" end 0x%08X", names, (unsigned)st);
    check(st == ST_NO_MORE_FILES && !strcmp(names, "a.txt,b.dat,c.txt"), "list *", d);
    list_dir("z:\\d", "*.txt", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\"", names);
    check(!strcmp(names, "a.txt,c.txt"), "list *.txt", d);
    st = list_dir("z:\\d", "none*", names, sizeof names);
    check(st != 0 && names[0] == 0, "no match: nothing listed", NULL);
    st = open_file("z:\\nodir\\x.txt", XBOX_FILE_CREATE, &h);
    check(st != 0, "create in a missing directory fails", NULL);

    st = open_file("z:\\d\\c.txt", XBOX_FILE_OPEN, &h);
    check(rename_to(h, "z:\\d\\a.txt", FALSE) == ST_COLLISION,
          "rename onto an existing name without replace: collision", NULL);
    check(rename_to(h, "z:\\e.txt", FALSE) == 0, "rename out of the directory", NULL);
    xbox_NtClose(h);
    check(!exists("z:\\d\\c.txt") && exists("z:\\e.txt"), "moved", NULL);

    st = open_file("\\Device\\Harddisk0\\Partition3\\e.txt", XBOX_FILE_OPEN, &h);
    check(st == 0, "Partition3\\e.txt is the same store", NULL);
    if (!st) xbox_NtClose(h);

    st = open_file("z:\\d\\a.txt", XBOX_FILE_OPEN, &h);
    check(rename_to(h, "c:\\a.txt", TRUE) == ST_NOT_SAME_DEV,
          "rename to another drive: not the same device", NULL);
    xbox_NtClose(h);
}

/* A handle opened for read and write (GENERIC_WRITE, which holds APPEND_DATA)
 * writes at its offset; only an append-only one appends. `path` is on the
 * store or on the host backend. */
static void check_append(const char *path, const char *where)
{
    unsigned char got[8];
    char what[96];
    HANDLE h;
    ULONG n = 0;
    NTSTATUS st;

    st = open_access(path, ACC_GENERIC_RW, XBOX_FILE_OVERWRITE_IF, &h);
    snprintf(what, sizeof what, "%s: open with GENERIC_READ|WRITE", where);
    check(st == 0, what, NULL);
    if (st) return;
    write_at(h, "0123456789", 10, 0);
    write_at(h, "ab", 2, 3);
    read_at(h, got, 8, 0, &n);
    snprintf(what, sizeof what, "%s: GENERIC_WRITE writes at its offset", where);
    check(query_size(h) == 10 && n == 8 && memcmp(got, "012ab567", 8) == 0, what, NULL);
    xbox_NtClose(h);
    st = open_access(path, ACC_APPEND_ONLY, XBOX_FILE_OPEN, &h);
    if (!st) {
        write_at(h, "Z", 1, 0);
        xbox_NtClose(h);
    }
    open_file(path, XBOX_FILE_OPEN, &h);
    read_at(h, got, 1, 10, &n);
    snprintf(what, sizeof what, "%s: an append-only handle appends", where);
    check(st == 0 && query_size(h) == 11 && n == 1 && got[0] == 'Z', what, NULL);
    xbox_NtClose(h);
}

static void test_append(const char *host_path)
{
    printf("\nOffsets on a GENERIC_WRITE handle\n");
    check_append("z:\\gw.bin", "store");
    check_append(host_path, "host");
}

static void test_handles(void)
{
    unsigned char got[8];
    HANDLE h, d, d2;
    ULONG n = 0;

    printf("\nDuplicates and closes\n");
    open_file("z:\\dup.bin", XBOX_FILE_OVERWRITE_IF, &h);
    write_at(h, "abcdef", 6, 0);
    set_pos(h, 2);
    check(xbox_ramfs_duplicate(h, &d, 0) == 1 && d != NULL, "duplicate a store handle", NULL);
    read_at(d, got, 2, -1, &n);
    check(n == 2 && memcmp(got, "cd", 2) == 0 && query_pos(h) == 4,
          "the duplicate shares the position", NULL);
    check(xbox_ramfs_duplicate(d, &d2, 1) == 1 && d2 != NULL,
          "duplicate with close-source", NULL);
    check(xbox_ramfs_close(d) == 0, "the source is closed", NULL);
    check(query_pos(d2) == 4, "the new handle has the position", NULL);
    check(xbox_ramfs_close(h) == 1, "close the first handle", NULL);
    check(xbox_ramfs_close(h) == 0, "a second close is not the store's", NULL);
    read_at(d2, got, 2, 0, &n);
    check(n == 2 && memcmp(got, "ab", 2) == 0, "the file lives on in the last handle", NULL);
    xbox_NtClose(d2);
    check(exists("z:\\dup.bin"), "and keeps its name after the last close", NULL);

    printf("\nShrink, then regrow\n");
    open_file("z:\\dup.bin", XBOX_FILE_OPEN, &h);
    set_eof(h, 2);
    set_eof(h, 6);
    read_at(h, got, 6, 0, &n);
    check(n == 6 && memcmp(got, "ab\0\0\0\0", 6) == 0,
          "bytes cut off by a shrink read back as zero", NULL);
    xbox_NtClose(h);
}

static NTSTATUS query_one(HANDLE h, void *buf, ULONG len, BOOLEAN restart,
                          XBOX_IO_STATUS_BLOCK *ios)
{
    XBOX_ANSI_STRING m;
    m.Length = 1;
    m.MaximumLength = 2;
    m.Buffer = (PCHAR)"*";
    memset(buf, 0, len);
    return xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, ios, buf, len,
                                     XboxFileDirectoryInformation, &m, restart);
}

static void test_seed_listing(void)
{
    static ULONG info[256];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    ULONG header = (ULONG)((ULONG_PTR)&((PXBOX_FILE_DIRECTORY_INFORMATION)0)->FileName);
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    char names[512], d[600], first[64];
    HANDLE h;
    NTSTATUS st;

    printf("\nSeeds in listings and deletes\n");
    st = list_dir("z:\\sub", "*", names, sizeof names);
    snprintf(d, sizeof d, "\"%s\"", names);
    check(!strcmp(names, "never.dat,s2.txt") || !strcmp(names, "s2.txt,never.dat"),
          "a seed never opened is listed", d);
    st = list_dir("z:\\sub", "never*", names, sizeof names);
    check(!strcmp(names, "never.dat"), "and matched by a pattern", NULL);

    set_name(&name, &oa, "z:\\sub", NULL);
    st = xbox_NtCreateFile(&h, 0x00100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x21);
    if (st) return;
    query_one(h, info, sizeof info, TRUE, &ios);
    snprintf(first, sizeof first, "%.*s", (int)e->FileNameLength, e->FileName);
    query_one(h, info, sizeof info, FALSE, &ios);
    st = query_one(h, info, sizeof info, TRUE, &ios);
    check(st == 0 && e->FileNameLength == strlen(first)
          && !memcmp(e->FileName, first, strlen(first)),
          "a restarted scan starts from the first entry again", first);
    st = query_one(h, info, header - 1, TRUE, &ios);
    check(st == ST_BUF_TOO_SMALL && ios.Information == 0,
          "a buffer under the entry header: STATUS_BUFFER_TOO_SMALL", NULL);
    st = query_one(h, info, header + 2, FALSE, &ios);
    snprintf(d, sizeof d, "status 0x%08X info %lu", (unsigned)st,
             (unsigned long)ios.Information);
    check(st == ST_BUF_OVERFLOW && ios.Information == header + 2
          && e->FileNameLength == strlen(first) && !memcmp(e->FileName, first, 2),
          "a name cut short: the same entry, STATUS_BUFFER_OVERFLOW", d);
    st = query_one(h, info, sizeof info, FALSE, &ios);
    check(st == 0 && strncmp(e->FileName, first, e->FileNameLength) != 0,
          "and the cut entry is used up", NULL);
    xbox_NtClose(h);

    set_name(&name, &oa, "z:\\sub\\never.dat", NULL);
    check(xbox_NtDeleteFile(&oa) == 0, "delete a seed never opened", NULL);
    check(!exists("z:\\sub\\never.dat"), "it stays deleted", NULL);
    st = open_file("z:\\sub\\never.dat", XBOX_FILE_OPEN_IF, &h);
    check(st == 0 && query_size(h) == 0, "a new file of that name starts empty", NULL);
    if (!st) xbox_NtClose(h);
    check(host_size("sub" SEP_S "never.dat") == 4, "the host seed is untouched", NULL);
}

/* kernel_fatx_overwrite's directory-search case (e5d407ea) on the store: a
 * search abandoned after one result (FindFirstFile then FindClose) must not
 * carry on into the next directory opened, whatever handle value it gets. */
#define SEARCH_DIRS 4
#define SEARCHES 200
static void test_dir_search(void)
{
    static ULONG info[128];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    char guest[32], want[8], pat[8], d[160], first_bad[96] = "";
    unsigned i, k, fresh = 0, reused = 0;
    HANDLE h, prev = NULL;
    NTSTATUS st;

    printf("\nDirectory searches closed after one result\n");
    for (k = 0; k < SEARCH_DIRS; k++) {
        HANDLE dir;
        snprintf(guest, sizeof guest, "z:\\q%u", k);
        st = open_at(NULL, guest, XBOX_FILE_CREATE, XBOX_FILE_DIRECTORY_FILE | 0x20, &dir, NULL);
        if (st) {
            snprintf(d, sizeof d, "%s: 0x%08X", guest, (unsigned)st);
            check(0, "make the search directories", d);
            return;
        }
        xbox_NtClose(dir);
        for (i = 0; i < 2; i++) {   /* two entries: one result leaves it open */
            snprintf(guest, sizeof guest, "z:\\q%u\\e%u%c.txt", k, k, 'a' + i);
            if (open_file(guest, XBOX_FILE_CREATE, &h) == 0) xbox_NtClose(h);
        }
    }

    for (i = 0; i < SEARCHES; i++) {
        XBOX_ANSI_STRING mask;
        XBOX_ANSI_STRING name;
        XBOX_OBJECT_ATTRIBUTES oa;
        XBOX_IO_STATUS_BLOCK ios;
        int ok;

        k = i % SEARCH_DIRS;
        snprintf(guest, sizeof guest, "z:\\q%u", k);
        snprintf(want, sizeof want, "e%u", k);
        snprintf(pat, sizeof pat, "e%u*", k);
        set_name(&name, &oa, guest, NULL);
        /* what XAPI's FindFirstFile asks for: list, synchronous, shared */
        st = xbox_NtCreateFile(&h, 0x00100001u, &oa, &ios, NULL, 0, 3, XBOX_FILE_OPEN, 0x21);
        if (st || !h) {
            snprintf(first_bad, sizeof first_bad, "search %u: open %s failed", i, guest);
            break;
        }
        if (h == prev) reused++;
        prev = h;
        mask.Length = (USHORT)strlen(pat);
        mask.MaximumLength = (USHORT)(mask.Length + 1);
        mask.Buffer = pat;
        memset(info, 0, sizeof info);
        st = xbox_NtQueryDirectoryFile(h, NULL, NULL, NULL, &ios, e, sizeof info,
                                       XboxFileDirectoryInformation, &mask, FALSE);
        ok = st == 0 && e->FileNameLength >= 2 && memcmp(e->FileName, want, 2) == 0;
        if (ok) fresh++;
        else if (!first_bad[0])
            snprintf(first_bad, sizeof first_bad, "search %u in q%u: status 0x%08X, \"%.*s\"",
                     i, k, (unsigned)st, (int)(e->FileNameLength < 16 ? e->FileNameLength : 16),
                     e->FileName);
        xbox_NtClose(h);
    }
    snprintf(d, sizeof d, "%u/%u fresh, handle reused %u times; first stale: %s",
             fresh, SEARCHES, reused, first_bad[0] ? first_bad : "none");
    printf("  (%s)\n", d);
    check(fresh == SEARCHES, "each new search starts in its own directory", d);
}

#if defined(RAMCACHE_OVER_CAP) || defined(RAMCACHE_UNDER_CAP) || defined(RAMCACHE_DIR_OVER_CAP)
/* A Cache dir past the cap, against the 64-cluster test cap. The
 * seeds are named by state (xbox_RamCacheSeedState), since a listing or a
 * query is what marks a seed seen. Write times are seconds past a fixed base
 * so the oldest is known whatever the host's listing order. */
static void host_backdate(const char *name, unsigned secs)
{
    char p[700];
    long long t = 1500000000ll + (long long)secs;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
#if defined(_WIN32)
    {
        HANDLE f = CreateFileA(p, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ, NULL,
                               OPEN_EXISTING, 0, NULL);
        ULONGLONG ft = 116444736000000000ull + (ULONGLONG)t * 10000000ull;
        FILETIME w;
        w.dwLowDateTime = (DWORD)ft;
        w.dwHighDateTime = (DWORD)(ft >> 32);
        if (f != INVALID_HANDLE_VALUE) {
            SetFileTime(f, NULL, NULL, &w);
            CloseHandle(f);
        }
    }
#else
    {
        struct utimbuf u;
        u.actime = u.modtime = (time_t)t;
        utime(p, &u);
    }
#endif
}

static unsigned char g_big[65u * CLUSTER];

/* A seed of `cl` clusters with write time `secs`. */
static void seed(const char *name, unsigned cl, unsigned secs)
{
    host_write(name, g_big, (size_t)cl * CLUSTER);
    host_backdate(name, secs);
}

static void seed_subdir(const char *name)
{
    char p[700];
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    make_dir(p);
}

static int state(const char *path)
{
    return xbox_RamCacheSeedState(path, NULL);
}

static uint32_t order_of(const char *path)
{
    uint32_t o = 0;
    xbox_RamCacheSeedState(path, &o);
    return o;
}

static LONGLONG avail(HANDLE h)
{
    XBOX_FILE_FS_SIZE_INFORMATION fs;
    XBOX_IO_STATUS_BLOCK ios;
    if (xbox_NtQueryVolumeInformationFile(h, &ios, &fs, sizeof fs,
                                          XboxFileFsSizeInformation) != 0)
        return -1;
    return fs.AvailableAllocationUnits.QuadPart;
}

static void cap_setup(void)
{
    unsigned i;
    for (i = 0; i < sizeof g_big; i++) g_big[i] = pattern(i);
#if defined(RAMCACHE_OVER_CAP)
    /* big.bin alone is past the cap, so it is left out whatever the order,
     * and the other 62 clusters all fit. */
    seed("big.bin", 65, 0);
    seed_subdir("sub");
    seed("sub" SEP_S "t.rec", 10, 100);
    seed_subdir("sub2");
    seed("sub2" SEP_S "e.rec", 5, 50);
    seed("s1.rec", 10, 200);
    seed("s2.rec", 10, 300);
    seed("s3.rec", 10, 300);
    seed("s4.rec", 10, 400);
    seed("s5.rec", 5, 60);
#elif defined(RAMCACHE_UNDER_CAP)
    for (i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "s%u.rec", i);
        seed(name, 10, 100 + i * 100);
    }
#else
    /* A file of the whole cap and a directory: whichever is listed second
     * is left out. */
    seed("a.bin", 64, 100);
    seed_subdir("d");
    seed("d" SEP_S "x.rec", 1, 200);
#endif
}
#endif

#if defined(RAMCACHE_OVER_CAP)
static void test_over_cap(const unsigned char *buf)
{
    static const char *const seeds[] = {
        "sub\\t.rec", "sub2\\e.rec", "s1.rec", "s2.rec", "s3.rec", "s4.rec", "s5.rec",
    };
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_FILE_NETWORK_OPEN_INFORMATION net;
    unsigned char got[4096];
    char names[256], d[200];
    const char *first, *second;
    unsigned i, all = 1;
    ULONG n;
    HANDLE h, g;
    NTSTATUS st;

    printf("\nA Cache dir over the cap\n");
    xbox_RamCacheSetCap(CAP);
    /* The first file call starts the store. */
    list_dir("z:\\sub2", "*", names, sizeof names);
    for (i = 0; i < sizeof seeds / sizeof seeds[0]; i++)
        all &= state(seeds[i]) == 1;
    snprintf(d, sizeof d, "used %llu clusters, big.bin %d",
             (unsigned long long)(xbox_RamCacheUsed() / CLUSTER), state("big.bin"));
    check(all && state("big.bin") == -1 && state("sub") == 0
          && xbox_RamCacheUsed() == 62ull * CLUSTER,
          "the seed leaves out what passes the cap: 62 clusters", d);
    check(!strcmp(names, "e.rec"), "sub2 lists e.rec (now seen)", names);

    /* Seen by a query, and opened: neither may go. A first open re-charges
     * what the seed held, so it drops nothing. */
    set_name(&name, &oa, "z:\\s5.rec", NULL);
    st = xbox_NtQueryFullAttributesFile(&oa, &net);
    check(st == 0 && net.EndOfFile.QuadPart == 5 * CLUSTER, "s5.rec queried (now seen)",
          NULL);
    st = open_file("z:\\s1.rec", XBOX_FILE_OPEN, &h);
    check(st == 0 && read_at(h, got, sizeof got, 0, &n) == 0 && n == sizeof got
          && matches(got, 0, sizeof got), "s1.rec opens and reads", NULL);
    xbox_NtClose(h);
    all = 1;
    for (i = 0; i < sizeof seeds / sizeof seeds[0]; i++)
        if (strcmp(seeds[i], "s1.rec")) all &= state(seeds[i]) == 1;
    check(all && state("s1.rec") == 0 && xbox_RamCacheUsed() == 62ull * CLUSTER,
          "a seed's first open drops nothing", NULL);

    /* Free space counts the droppable seeds: 2 free + t, s2, s3, s4. */
    st = open_file("z:\\game.rec", XBOX_FILE_OVERWRITE_IF, &g);
    snprintf(d, sizeof d, "avail %lld", (long long)avail(g));
    check(st == 0 && avail(g) == 42, "free space: 2 clusters + 40 droppable", d);

    /* Up to the cap: nothing dropped. */
    st = write_at(g, buf, 2 * CLUSTER, 0);
    check(st == 0 && state("sub\\t.rec") == 1 && xbox_RamCacheUsed() == CAP,
          "game.rec up to the cap drops nothing", NULL);

    /* One cluster past: e.rec and s5.rec are older but seen, so t.rec in
     * sub goes. */
    st = write_at(g, buf, CLUSTER, 2 * CLUSTER);
    snprintf(d, sizeof d, "status 0x%08X, t %d e %d s5 %d", (unsigned)st,
             state("sub\\t.rec"), state("sub2\\e.rec"), state("s5.rec"));
    check(st == 0 && state("sub\\t.rec") == -1 && state("sub2\\e.rec") == 1
          && state("s5.rec") == 1 && state("sub") == 0,
          "the oldest unseen seed goes, from its subdirectory", d);

    /* s2 and s3 share a write time: the first seeded goes. */
    first = order_of("s2.rec") < order_of("s3.rec") ? "s2.rec" : "s3.rec";
    second = first[1] == '2' ? "s3.rec" : "s2.rec";
    st = set_eof(g, 13ll * CLUSTER);
    snprintf(d, sizeof d, "status 0x%08X, %s %d, %s %d", (unsigned)st, first,
             state(first), second, state(second));
    check(st == 0 && state(first) == -1 && state(second) == 1 && state("s4.rec") == 1,
          "a tie on write time: the first seeded goes", d);

    /* Past every droppable seed: refused, nothing dropped. */
    st = set_eof(g, (LONGLONG)CAP);
    check(st == ST_DISK_FULL && state(second) == 1 && state("s4.rec") == 1,
          "past every droppable seed: STATUS_DISK_FULL, nothing dropped", NULL);

    /* The rest: both droppable seeds go, the seen and opened ones stay. */
    st = set_eof(g, 42ll * CLUSTER);
    snprintf(d, sizeof d, "status 0x%08X, used %llu", (unsigned)st,
             (unsigned long long)(xbox_RamCacheUsed() / CLUSTER));
    check(st == 0 && state(second) == -1 && state("s4.rec") == -1
          && state("s1.rec") == 0 && state("sub2\\e.rec") == 1 && state("s5.rec") == 1
          && xbox_RamCacheUsed() == CAP,
          "the rest of the cap: only the seen and opened seeds are left", d);
    st = write_at(g, buf, 1, 42ll * CLUSTER);
    check(st == ST_DISK_FULL, "then the cap answers STATUS_DISK_FULL", NULL);
    xbox_NtClose(g);

    printf("\nHost Cache\n");
    check(host_entries() == 8 && host_size("s2.rec") == 10l * CLUSTER
          && host_size("s4.rec") == 10l * CLUSTER
          && host_size("sub" SEP_S "t.rec") == 10l * CLUSTER,
          "every host seed is still there, whole", NULL);
}
#elif defined(RAMCACHE_UNDER_CAP)
/* A Cache dir that fit: it may be the title's own state, so the cap answers
 * as stock and nothing is ever dropped. */
static void test_over_cap(const unsigned char *buf)
{
    unsigned i, all = 1;
    HANDLE g, dir;
    NTSTATUS st;
    ULONG_PTR info;
    char d[120];

    printf("\nA Cache dir under the cap\n");
    xbox_RamCacheSetCap(CAP);
    st = open_file("z:\\game.rec", XBOX_FILE_OVERWRITE_IF, &g);
    check(st == 0 && xbox_RamCacheUsed() == 40ull * CLUSTER, "four seeds, 40 clusters",
          NULL);
    snprintf(d, sizeof d, "avail %lld", (long long)avail(g));
    check(avail(g) == 24, "free space: the cap less the seeds", d);
    st = set_eof(g, 24ll * CLUSTER);
    check(st == 0 && avail(g) == 0, "game.rec up to the cap", NULL);
    st = write_at(g, buf, 1, 24ll * CLUSTER);
    check(st == ST_DISK_FULL, "a write past it: STATUS_DISK_FULL", NULL);
    st = open_at(NULL, "z:\\newdir", XBOX_FILE_CREATE, 0x21, &dir, &info);
    check(st != 0, "a new directory past it fails", NULL);
    if (st == 0) xbox_NtClose(dir);
    for (i = 0; i < 4; i++) {
        char name[16];
        snprintf(name, sizeof name, "s%u.rec", i);
        all &= state(name) == 1;
    }
    check(all, "no seed was dropped", NULL);
    xbox_NtClose(g);
}
#elif defined(RAMCACHE_DIR_OVER_CAP)
/* A directory past the cap is left out like a file, so the charge stays in
 * the cap and the overflow lets a write drop the seed that did fit. */
static void test_over_cap(const unsigned char *buf)
{
    const char *kept;
    HANDLE g;
    NTSTATUS st;
    char d[160];
    (void)buf;

    printf("\nA directory past the cap\n");
    xbox_RamCacheSetCap(CAP);
    st = open_file("z:\\game.rec", XBOX_FILE_OVERWRITE_IF, &g);
    kept = state("a.bin") == 1 ? "a.bin" : "d\\x.rec";
    snprintf(d, sizeof d, "a.bin %d, d %d, d\\x.rec %d, used %llu", state("a.bin"),
             state("d"), state("d\\x.rec"),
             (unsigned long long)(xbox_RamCacheUsed() / CLUSTER));
    check(st == 0 && xbox_RamCacheUsed() <= CAP
          && ((state("a.bin") == 1 && state("d") == -1 && state("d\\x.rec") == -1)
              || (state("a.bin") == -1 && state("d") == 0 && state("d\\x.rec") == 1)),
          "the one listed second is left out, the charge within the cap", d);
    st = set_eof(g, 63ll * CLUSTER);
    snprintf(d, sizeof d, "status 0x%08X, %s %d", (unsigned)st, kept, state(kept));
    check(st == 0 && state(kept) == -1, "the seed that fit is dropped for the write", d);
    xbox_NtClose(g);
}
#endif

int main(int argc, char **argv)
{
    static unsigned char buf[65536];
    char root[512], p[700];
    unsigned i;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%sramcache-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/ramcacheXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
    }
#endif
    snprintf(g_cache, sizeof g_cache, "%s" SEP_S "Cache", root);
    make_dir(g_cache);
    for (i = 0; i < sizeof buf; i++) buf[i] = pattern(i);
#if defined(RAMCACHE_OVER_CAP) || defined(RAMCACHE_UNDER_CAP) || defined(RAMCACHE_DIR_OVER_CAP)
    cap_setup();
    snprintf(p, sizeof p, "%s" SEP_S "UDATA", root);
    make_dir(p);
    xbox_path_init(root, root);
    test_over_cap(buf);
#else
    host_write("seed.bin", buf, 3000);
    snprintf(p, sizeof p, "%s" SEP_S "sub", g_cache);
    make_dir(p);
    host_write("sub" SEP_S "s2.txt", (const unsigned char *)"hello", 5);
    host_write("sub" SEP_S "never.dat", (const unsigned char *)"seed", 4);
    snprintf(p, sizeof p, "%s" SEP_S "UDATA", root);
    make_dir(p);
    xbox_path_init(root, root);

#if defined(RAMCACHE_OFF_VALUE) || defined(RAMCACHE_OFF_ENABLE)
    test_explicit_off();
#elif defined(RAMCACHE_BAD_VALUE)
    test_bad_value();
#else
    test_on_by_default();
    test_seed(buf);
    test_file_io(buf);
    test_fatx_overwrite(buf);
    test_cap(buf);
    test_info_classes();
    test_delete();
    test_dirs();
    test_append("\\Device\\Harddisk0\\Partition1\\UDATA\\gw.bin");
    test_handles();
    test_seed_listing();
    test_dir_search();

    printf("\nHost Cache\n");
    check(host_entries() == 2, "only the two seeded entries on disk", NULL);
    check(host_size("seed.bin") == 3000, "seed.bin unchanged in size", NULL);
#endif
#endif

    {
        char cmd[700];
#if defined(_WIN32)
        snprintf(cmd, sizeof cmd, "cmd /c rmdir /s /q \"%s\"", root);
#else
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", root);
#endif
        (void)system(cmd);
    }
    printf("\n%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
