/*
 * FATX overwrite: regrowing after an overwrite keeps the data.
 *
 * A title can build its z: texture cache as:
 *
 *   NtCreateFile(x.tmp, FILE_OVERWRITE_IF, opts 0x48)   no buffering
 *   set end of file (size rounded up to 512); write all of it; close
 *   NtCreateFile(x.tmp, FILE_OVERWRITE_IF, opts 0x60)
 *   query position; set end of file (true size); rename to x.dat; close
 *
 * FATX keeps no valid-data length, so the second open's trim exposes the
 * bytes already on disk. The host truncate (O_TRUNC, CREATE_ALWAYS) zero-
 * fills instead, and every cache file came out as zeros of the right length
 * (seen on Proton: a 1 MB cache file that was all zeros).
 *
 * Cross-platform: it calls the xbox_Nt* file functions directly, which are
 * the Win32 backend on Windows (and Proton) and the POSIX one elsewhere.
 */
#include "kernel.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <unistd.h>
#include <sys/stat.h>
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

static int failures;
static char g_cache[512];

static void check(int ok, const char *what, const char *detail)
{
    printf("  %-60s %s", what, ok ? "PASS" : "FAIL");
    if (!ok && detail) printf(" -- %s", detail);
    putchar('\n');
    if (!ok) failures++;
}

static NTSTATUS open_file(const char *path, ULONG disposition, ULONG options,
                          HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;

    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *out = NULL;
    return xbox_NtCreateFile(out, 0xC0110000u /* R|W|DELETE|SYNC */, &oa, &ios,
                             NULL, 0x80, 0, disposition, options);
}

static NTSTATUS set_eof(HANDLE h, LONGLONG size)
{
    XBOX_FILE_END_OF_FILE_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    info.EndOfFile.QuadPart = size;
    return xbox_NtSetInformationFile(h, &ios, &info, sizeof info,
                                     XboxFileEndOfFileInformation);
}

static NTSTATUS query_pos(HANDLE h)
{
    XBOX_FILE_POSITION_INFORMATION info;
    XBOX_IO_STATUS_BLOCK ios;
    return xbox_NtQueryInformationFile(h, &ios, &info, sizeof info,
                                       XboxFilePositionInformation);
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

static NTSTATUS write_at(HANDLE h, const void *buf, ULONG len, LONGLONG off)
{
    XBOX_IO_STATUS_BLOCK ios;
    LARGE_INTEGER o;
    o.QuadPart = off;
    return xbox_NtWriteFile(h, NULL, NULL, NULL, &ios, (PVOID)buf, len, &o);
}

static NTSTATUS rename_to(HANDLE h, const char *path)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    return xbox_RenameFileByHandle(h, &oa, TRUE);
}

/* Host-side read of <cache>/<name>; -1 if it does not exist. */
static long host_file(const char *name, unsigned char *buf, size_t cap)
{
    char p[700];
    FILE *f;
    long n;
    snprintf(p, sizeof p, "%s" SEP_S "%s", g_cache, name);
    f = fopen(p, "rb");
    if (!f) return -1;
    n = (long)fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

static unsigned char pattern(unsigned i) { return (unsigned char)(i * 7 + 1); }

static void set_env(const char *k, const char *v)
{
#if defined(_WIN32)
    _putenv_s(k, v ? v : "");
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    CreateDirectoryA(p, NULL);
#else
    mkdir(p, 0755);
#endif
}

static int is_dir(const char *p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesA(p);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

/* Translate a guest path and compare it with <want>. */
static void check_maps(const char *guest, const char *want, const char *what)
{
    xbox_host_char host[MAX_PATH];
    char got[MAX_PATH * 2], d[MAX_PATH * 2 + 32];
    int ok = xbox_translate_path(guest, host, MAX_PATH);
#if defined(_WIN32)
    if (ok) WideCharToMultiByte(CP_UTF8, 0, host, -1, got, sizeof got, NULL, NULL);
#else
    if (ok) snprintf(got, sizeof got, "%s", host);
#endif
    if (!ok) snprintf(got, sizeof got, "(not translated)");
    {   /* the join can double a separator; the host APIs don't mind */
        char *r = got, *w = got;
        for (; *r; r++)
            if (!((*r == '/' || *r == '\\') && w > got && w[-1] == *r)) *w++ = *r;
        *w = 0;
    }
    snprintf(d, sizeof d, "got %s", got);
    check(ok && strcmp(got, want) == 0, what, d);
}

/* A search abandoned after one result must not outlive its handle.
 *
 * A title that looks for one save (FindFirstFile, then FindClose without
 * reading on) leaves its enumeration part-way. Host handle values are reused,
 * so a later directory opened at the same value carried the old search on:
 * its first answer was the old directory's next entry, or "no more files",
 * and with no reuse the table of searches filled up. */
#define DIRS 8
#define SEARCHES 300

static NTSTATUS open_dir(const char *path, HANDLE *out)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;

    name.Length = (USHORT)strlen(path);
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)path;
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    *out = NULL;
    /* what XAPI's FindFirstFile asks for: list, synchronous, shared */
    return xbox_NtCreateFile(out, 0x00100001u, &oa, &ios, NULL, 0, 3,
                             XBOX_FILE_OPEN, 0x21);
}

static void test_dir_search(void)
{
    static ULONG info[128];
    PXBOX_FILE_DIRECTORY_INFORMATION e = (PXBOX_FILE_DIRECTORY_INFORMATION)info;
    char p[700], guest[32], want[8], pat[8], d[160], first_bad[96] = "";
    unsigned i, k, fresh = 0, reused = 0;
    HANDLE h, prev = NULL;

    printf("\nDirectory searches closed after one result\n");
    for (k = 0; k < DIRS; k++) {
        FILE *f;
        snprintf(p, sizeof p, "%s" SEP_S "d%u", g_cache, k);
        make_dir(p);
        for (i = 0; i < 2; i++) {   /* two entries: one result leaves it open */
            snprintf(p, sizeof p, "%s" SEP_S "d%u" SEP_S "e%u%c.txt", g_cache, k, k, 'a' + i);
            if ((f = fopen(p, "wb")) != NULL) fclose(f);
        }
    }

    for (i = 0; i < SEARCHES; i++) {
        XBOX_ANSI_STRING mask;
        XBOX_IO_STATUS_BLOCK ios;
        NTSTATUS st;
        int ok;

        k = i % DIRS;
        snprintf(guest, sizeof guest, "z:\\d%u", k);
        snprintf(want, sizeof want, "e%u", k);
        snprintf(pat, sizeof pat, "e%u*", k);
        if (open_dir(guest, &h) != 0 || !h) {
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
            snprintf(first_bad, sizeof first_bad, "search %u in d%u: status 0x%08X, \"%.*s\"",
                     i, k, (unsigned)st, (int)(e->FileNameLength < 16 ? e->FileNameLength : 16),
                     e->FileName);
        xbox_NtClose(h);
    }
    snprintf(d, sizeof d, "%u/%u fresh, handle reused %u times; first stale: %s",
             fresh, SEARCHES, reused, first_bad[0] ? first_bad : "none");
    printf("  (%s)\n", d);
    check(fresh == SEARCHES, "each new search starts in its own directory", d);
}

/* XAPI SetEndOfFile on a handle opened only to read.
 *
 * A title loading its saves opens each slot with GENERIC_READ and FILE_OPEN,
 * reads it, and calls SetEndOfFile at the end of what it read. On the Xbox the
 * I/O manager refuses that (STATUS_ACCESS_DENIED: setting the end of file or
 * the allocation needs FILE_WRITE_DATA) and the file is not touched. Here the
 * host call ran, failed with err=5 and printed a warning per slot loaded, and
 * the status was STATUS_UNSUCCESSFUL. */
static void test_read_only_eof(const unsigned char *buf)
{
    XBOX_ANSI_STRING name;
    XBOX_OBJECT_ATTRIBUTES oa;
    XBOX_IO_STATUS_BLOCK ios;
    XBOX_FILE_END_OF_FILE_INFORMATION alloc;
    unsigned char got[4096];
    static unsigned char rd[3588];
    LARGE_INTEGER zero;
    NTSTATUS st;
    HANDLE h = NULL;
    long n;
    char d[96];
    int same, i;

    printf("\nSetEndOfFile on a read-only handle (a save being loaded)\n");
    st = open_file("z:\\slot00", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    check(st == 0 && h, "create z:\\slot00", NULL);
    if (st) return;
    write_at(h, buf, 3588, 0);
    xbox_NtClose(h);

    name.Length = (USHORT)strlen("z:\\slot00");
    name.MaximumLength = (USHORT)(name.Length + 1);
    name.Buffer = (PCHAR)"z:\\slot00";
    oa.RootDirectory = NULL;
    oa.ObjectName = &name;
    oa.Attributes = 0x40;
    /* what XAPI CreateFile(GENERIC_READ, OPEN_EXISTING) asks for */
    st = xbox_NtCreateFile(&h, 0x80100080u, &oa, &ios, NULL, 0x80, 1,
                           XBOX_FILE_OPEN, 0x60);
    check(st == 0 && h, "open it to read (GENERIC_READ, FILE_OPEN)", NULL);
    if (st) return;
    zero.QuadPart = 0;
    xbox_NtReadFile(h, NULL, NULL, NULL, &ios, rd, sizeof rd, &zero);
    check(query_pos(h) == 0, "position query", NULL);
    st = set_eof(h, 3588);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == STATUS_ACCESS_DENIED, "set end of file: STATUS_ACCESS_DENIED", d);
    alloc.EndOfFile.QuadPart = 0;
    st = xbox_NtSetInformationFile(h, &ios, &alloc, sizeof alloc,
                                   XboxFileAllocationInformation);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == STATUS_ACCESS_DENIED, "set allocation to 0: STATUS_ACCESS_DENIED", d);
    xbox_NtClose(h);

    n = host_file("slot00", got, sizeof got);
    same = n == 3588;
    for (i = 0; same && i < 3588; i++)
        if (got[i] != buf[i]) same = 0;
    snprintf(d, sizeof d, "%ld bytes", n);
    check(same, "the slot keeps its 3588 bytes", d);
}

/* Where Partition1's UDATA and TDATA go: RECOMP_SAVE_DIR, the save dir, or
 * the game dir when saves are only there. */
static void test_save_dir(const char *root)
{
    char game[600], save[600], ov[600], p[700], want[700];

    printf("\nSave directories\n");
    snprintf(game, sizeof game, "%s" SEP_S "g", root);
    snprintf(save, sizeof save, "%s" SEP_S "s", root);
    snprintf(ov, sizeof ov, "%s" SEP_S "ov", root);
    make_dir(game);
    make_dir(save);

    set_env("RECOMP_SAVE_DIR", ov);
    xbox_path_init(game, save);
    check(is_dir(ov), "RECOMP_SAVE_DIR is created", ov);
    snprintf(want, sizeof want, "%s" SEP_S "UDATA" SEP_S "x", ov);
    check_maps("\\Device\\Harddisk0\\Partition1\\UDATA\\x", want,
               "UDATA\\x goes under RECOMP_SAVE_DIR");

    set_env("RECOMP_SAVE_DIR", NULL);
    snprintf(p, sizeof p, "%s" SEP_S "TDATA", game);
    make_dir(p);                   /* saves only in the game dir */
    xbox_path_init(game, save);
    snprintf(want, sizeof want, "%s" SEP_S "UDATA" SEP_S "x", save);
    check_maps("\\Device\\Harddisk0\\Partition1\\UDATA\\x", want,
               "unset: UDATA\\x goes under the save dir");
    snprintf(want, sizeof want, "%s" SEP_S "TDATA" SEP_S "y", game);
    check_maps("\\Device\\Harddisk0\\Partition1\\TDATA\\y", want,
               "TDATA only in the game dir stays there");
    snprintf(want, sizeof want, "%s" SEP_S "UDATAx" SEP_S "z", game);
    check_maps("\\Device\\Harddisk0\\Partition1\\UDATAx\\z", want,
               "UDATAx is not UDATA");
}

int main(int argc, char **argv)
{
    static unsigned char got[8192];
    unsigned char *buf;
    char root[512], d[160];
    HANDLE h;
    NTSTATUS st;
    long n;
    unsigned i;
    int same;

    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        snprintf(root, sizeof root, "%sfatxow-%lu", tmp,
                 (unsigned long)GetCurrentProcessId());
        if (!CreateDirectoryA(root, NULL)) return 2;
        snprintf(g_cache, sizeof g_cache, "%s\\Cache", root);
        CreateDirectoryA(g_cache, NULL);
        /* unbuffered I/O wants a sector-aligned buffer */
        buf = VirtualAlloc(NULL, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    }
#else
    {
        char tmpl[] = "/tmp/fatxowXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(root, sizeof root, "%s", tmpl);
        snprintf(g_cache, sizeof g_cache, "%s/Cache", root);
        mkdir(g_cache, 0755);
        if (posix_memalign((void **)&buf, 4096, 4096) != 0) buf = NULL;
    }
#endif
    if (!buf) return 2;
    xbox_path_init(root, root);
    for (i = 0; i < 4096; i++) buf[i] = pattern(i);
    memcpy(buf, "IPK1", 4);

    /* First, so the overwrite and rename checks below run on handle values
     * that directory searches have used and closed. */
    test_dir_search();

    printf("\nFATX overwrite (the texture-cache copy)\n");

    /* First pass: create, size, write unbuffered, close. */
    st = open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, 0x48, &h);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0 && h, "create z:\\o.tmp (OVERWRITE_IF, no buffering)", d);
    if (st) goto done;
    check(set_eof(h, 4096) == 0, "set end of file to 4096", NULL);
    st = write_at(h, buf, 4096, 0);
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "write 4096 bytes starting \"IPK1\"", d);
    xbox_NtClose(h);
    check(host_file("o.tmp", got, sizeof got) == 4096, "host file is 4096 bytes", NULL);

    /* Second pass: overwrite, position query, trim, rename, close. */
    st = open_file("z:\\o.tmp", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    check(st == 0 && h, "reopen with OVERWRITE_IF", NULL);
    check(query_pos(h) == 0, "position query (XAPI SetEndOfFile does one)", NULL);
    check(set_eof(h, 3000) == 0, "set end of file to the true size, 3000", NULL);
    st = rename_to(h, "z:\\o.dat");
    snprintf(d, sizeof d, "status 0x%08X", (unsigned)st);
    check(st == 0, "rename to z:\\o.dat", d);
    xbox_NtClose(h);
    n = host_file("o.dat", got, sizeof got);
    same = n == 3000;
    for (i = 0; same && i < 3000; i++)
        if (got[i] != (i < 4 ? (unsigned char)"IPK1"[i] : pattern(i))) same = 0;
    snprintf(d, sizeof d, "%ld bytes, first 0x%02X", n, n > 0 ? got[0] : 0);
    check(n == 3000, "o.dat is 3000 bytes", d);
    check(same, "the bytes below the new end survive", d);
    check(host_file("o.tmp", got, sizeof got) < 0, "o.tmp is gone", NULL);

    /* An overwrite followed by a write is a plain truncate. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    check(st == 0, "overwrite o.dat again", NULL);
    write_at(h, buf, 4, 0);
    xbox_NtClose(h);
    check(host_file("o.dat", got, sizeof got) == 4,
          "overwrite + 4-byte write leaves 4 bytes", NULL);

    /* A size query sees the overwrite. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE, 0x60, &h);
    check(st == 0, "FILE_OVERWRITE of an existing file opens", NULL);
    snprintf(d, sizeof d, "size %lld", (long long)query_size(h));
    check(query_size(h) == 0, "overwrite + size query reports 0", d);
    xbox_NtClose(h);

    /* An overwrite that is just closed leaves an empty file. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    write_at(h, buf, 16, 0);
    xbox_NtClose(h);
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    xbox_NtClose(h);
    check(host_file("o.dat", got, sizeof got) == 0,
          "overwrite + close leaves 0 bytes", NULL);

    /* An overwrite that regrows past the old end zero-fills only the tail. */
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    write_at(h, buf, 100, 0);
    xbox_NtClose(h);
    st = open_file("z:\\o.dat", XBOX_FILE_OVERWRITE_IF, 0x60, &h);
    set_eof(h, 200);
    xbox_NtClose(h);
    n = host_file("o.dat", got, sizeof got);
    same = n == 200;
    for (i = 0; same && i < 200; i++)
        if (got[i] != (i < 4 ? (unsigned char)"IPK1"[i] : i < 100 ? pattern(i) : 0))
            same = 0;
    check(same, "regrow past the old end: old bytes, then zeros", NULL);

    /* FILE_OVERWRITE of a missing file still fails. */
    st = open_file("z:\\missing.dat", XBOX_FILE_OVERWRITE, 0x60, &h);
    check(st != 0, "FILE_OVERWRITE of a missing file fails", NULL);

    test_read_only_eof(buf);
    test_save_dir(root);

done:
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
