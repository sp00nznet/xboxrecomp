/*
 * Where the path layer puts a title's writes on E: (kernel_path.c).
 *
 * E: is \Device\Harddisk0\Partition1. Most of it maps to the game dir, since
 * a title may read its own assets through it, but the directories a title
 * writes -- UDATA and TDATA (saves) and CACHE (scratch) -- must land under
 * the user-data root. Burnout 3 opens E:\CACHE\LocalCache00.bin and
 * LocalCache08.bin with FILE_OPEN_IF; while CACHE fell through to the
 * Partition1 rule, every run wrote them into the user's dump. CACHE maps to
 * TitleCache, not CACHE, since Z: and Partition3-5 (the cache partitions
 * behind X:, Y: and Z:) use <save>/Cache and the two names are one folder on
 * a case-insensitive host.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere, so
 * both forms of each rule (sub_win, sub_posix) are checked by the same cases.
 */
#include "kernel.h"
#include "recomp_env.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <shellapi.h>
#include <io.h>
#include <fcntl.h>
#else
#include <fcntl.h>
#include <ftw.h>
#include <pthread.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <dirent.h>
#include <signal.h>
#endif

/* Provided by the generated title; nothing here calls a guest function. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(uint32_t xbox_va);
recomp_func_t recomp_lookup(uint32_t xbox_va) { (void)xbox_va; return NULL; }
recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
recomp_func_t recomp_lookup_manual(uint32_t xbox_va) { (void)xbox_va; return NULL; }

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

static int failures;
static char g_root[512], g_game[600], g_save[600], g_user[600];

static void check(int ok, const char *what)
{
    printf("  %-66s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
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

/* Remove the temporary root. xbox_path_init sizes five partition images in
 * the save dir (sparse, but ~2.6 GB apparent per run), so leaving it behind
 * fills /tmp or %TEMP% one run at a time. */
#if !defined(_WIN32)
static int rm_one(const char *p, const struct stat *st, int flag, struct FTW *f)
{
    (void)st; (void)flag; (void)f;
    return remove(p);
}
#endif

static void remove_root(void)
{
    if (!g_root[0])
        return;
#if defined(_WIN32)
    {
        char from[sizeof g_root + 2];
        SHFILEOPSTRUCTA op;
        size_t n = strlen(g_root);
        memcpy(from, g_root, n);
        from[n] = from[n + 1] = '\0';      /* the list is double-NUL ended */
        memset(&op, 0, sizeof op);
        op.wFunc = FO_DELETE;
        op.pFrom = from;
        op.fFlags = FOF_NO_UI;
        SHFileOperationA(&op);
    }
#else
    nftw(g_root, rm_one, 16, FTW_DEPTH | FTW_PHYS);
#endif
}

/* Translate a guest path and return the host path as narrow text. */
static const char *host(const char *guest)
{
    static char out[MAX_PATH * 2];
    xbox_host_char buf[MAX_PATH];

    out[0] = '\0';
    if (!xbox_translate_path(guest, buf, MAX_PATH))
        return out;
#if defined(_WIN32)
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out, (int)sizeof out, NULL, NULL);
#else
    snprintf(out, sizeof out, "%s", buf);
#endif
    /* The rules with a sub-directory join it to a remainder that keeps its
     * leading separator ("UDATA//x"); the host accepts that, so compare the
     * collapsed form. */
    {
        char *r = out, *w = out;
        for (; *r; r++)
            if (!(*r == SEP[0] && w > out && w[-1] == SEP[0]))
                *w++ = *r;
        *w = '\0';
    }
    return out;
}

static int same(const char *a, const char *b)
{
#if defined(_WIN32)
    return _stricmp(a, b) == 0;
#else
    return strcmp(a, b) == 0;
#endif
}

static int under(const char *path, const char *dir)
{
    size_t n = strlen(dir);
#if defined(_WIN32)
    if (_strnicmp(path, dir, n) != 0) return 0;
#else
    if (strncmp(path, dir, n) != 0) return 0;
#endif
    return path[n] == SEP[0];
}

/* The directory part of a host path, folded to lower case: Windows, Proton
 * and macOS treat names that differ only in case as one folder. */
static void folded_dir(const char *path, char *out, size_t n)
{
    char *slash;
    size_t i;

    snprintf(out, n, "%s", path);
    slash = strrchr(out, SEP[0]);
    if (slash) *slash = '\0';
    for (i = 0; out[i]; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] = (char)(out[i] - 'A' + 'a');
}

static void expect(const char *guest, const char *want, const char *what)
{
    const char *got = host(guest);
    check(same(got, want), what);
    if (!same(got, want))
        printf("    %s\n    got  %s\n    want %s\n", guest, got, want);
}

/* With RECOMP_SAVE_DIR unset the user-data root is the save dir. */
static void test_default_root(void)
{
    char want[1200];

    printf("user-data root = save dir\n");
    recomp_env_set(RENV_SAVE_DIR, "");
    xbox_path_init(g_game, g_save);

    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_save);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin", want,
           "Partition1\\CACHE goes to <save>/TitleCache");
    check(!under(host("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin"), g_game),
          "Partition1\\CACHE is not under the game dir");

    /* The host-side root the D3D11 shader cache sits under. */
    {
        char root[1200];
        check(xbox_path_hdd_root(root, sizeof root) && same(root, g_save),
              "xbox_path_hdd_root is the save dir");
        check(!xbox_path_hdd_root(root, 2), "xbox_path_hdd_root refuses a short buffer");
    }

    /* E:\CACHE and the utility-drive caches share the save root here, so
     * they must differ by more than case: <save>/CACHE was <save>/Cache on
     * a case-insensitive host, and the title's files mixed with Z:'s. */
    {
        static const char *const others[][2] = {
            { "Z:\\LocalCache08.bin",                               "Z:\\"         },
            { "\\Device\\Harddisk0\\Partition3\\LocalCache08.bin", "Partition3" },
            { "\\Device\\Harddisk0\\Partition5\\LocalCache08.bin", "Partition5" },
        };
        char title[1200], other[1200], what[128];
        size_t i;
        folded_dir(host("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin"),
                   title, sizeof title);
        for (i = 0; i < sizeof others / sizeof others[0]; i++) {
            folded_dir(host(others[i][0]), other, sizeof other);
            snprintf(what, sizeof what, "E:\\CACHE and %s are different folders, ignoring case",
                     others[i][1]);
            check(title[0] && other[0] && strcmp(title, other) != 0, what);
        }
    }
}

/* RECOMP_SAVE_DIR moves CACHE with UDATA and TDATA. */
static void test_save_dir_root(void)
{
    char want[1200], dir[700];
    XBOX_ANSI_STRING l, t;
    const char *p;

    printf("user-data root = RECOMP_SAVE_DIR\n");
    recomp_env_set(RENV_SAVE_DIR, g_user);
    xbox_path_init(g_game, g_save);

    /* Burnout 3's exact spelling: lower-case "partition1". */
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache00.bin", g_user);
    expect("\\Device\\Harddisk0\\partition1\\CACHE\\LocalCache00.bin", want,
           "partition1\\CACHE (B3's case) goes to <user>/TitleCache");
    p = host("\\Device\\Harddisk0\\partition1\\CACHE\\LocalCache08.bin");
    check(!under(p, g_game) && !under(p, g_save), "it is under neither the game dir nor the save dir");

    /* The title opens its file with FILE_OPEN_IF, which needs the parent. */
    snprintf(dir, sizeof dir, "%s" SEP "TitleCache", g_user);
    check(is_dir(dir), "translating it made <user>/TitleCache");

    snprintf(want, sizeof want, "%s" SEP "TitleCache", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE", want, "the bare CACHE directory too");
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\", want, "and with a trailing separator");

    snprintf(want, sizeof want, "%s" SEP "UDATA" SEP "4541005b" SEP "SaveMeta.xbx", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\4541005b\\SaveMeta.xbx", want,
           "UDATA still goes to <user>/UDATA");
    snprintf(want, sizeof want, "%s" SEP "TDATA" SEP "4541005b", g_user);
    expect("\\Device\\Harddisk0\\Partition1\\TDATA\\4541005b", want,
           "TDATA still goes to <user>/TDATA");

    /* Asset reads through E: stay in the game dir (Wreckless loads every
     * asset through z:, which it links to Partition1\). */
    snprintf(want, sizeof want, "%s" SEP "Data" SEP "x.dat", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\Data\\x.dat", want,
           "the rest of Partition1 stays in the game dir");

    l.Buffer = (PCHAR)"\\??\\Q:"; l.Length = 6; l.MaximumLength = 7;
    t.Buffer = (PCHAR)"\\Device\\Harddisk0\\Partition1\\";
    t.Length = (USHORT)strlen(t.Buffer);
    t.MaximumLength = (USHORT)(t.Length + 1);
    xbox_IoCreateSymbolicLink(&l, &t);
    snprintf(want, sizeof want, "%s" SEP "media" SEP "a.ipk", g_game);
    expect("q:\\media\\a.ipk", want, "a title link to Partition1\\ still reads the game dir");
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_user);
    expect("q:\\CACHE\\LocalCache08.bin", want, "and its CACHE goes to <user>/TitleCache");

    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\CACHE\\x") == XBOX_TREE_USER,
          "xbox_path_tree: CACHE is the user tree");
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\x") == XBOX_TREE_GAME,
          "xbox_path_tree: the rest of Partition1 is the game tree");

    /* A rule without a trailing separator needs a boundary after it, or
     * any asset name that starts with CACHE/UDATA/TDATA moved too. */
    {
        static const char *const names[][2] = {
            { "CACHEX\\a",       "CACHEX" SEP "a"      },
            { "cached.dat",      "cached.dat"          },
            { "CACHEDATA\\a.bin", "CACHEDATA" SEP "a.bin" },
            { "UDATAX\\a",       "UDATAX" SEP "a"      },
            { "TDATA2\\a",       "TDATA2" SEP "a"      },
        };
        char guest[256], what[128];
        size_t i;
        for (i = 0; i < sizeof names / sizeof names[0]; i++) {
            snprintf(guest, sizeof guest, "\\Device\\Harddisk0\\Partition1\\%s", names[i][0]);
            snprintf(want, sizeof want, "%s" SEP "%s", g_game, names[i][1]);
            snprintf(what, sizeof what, "Partition1\\%s stays in the game dir", names[i][0]);
            expect(guest, want, what);
            snprintf(what, sizeof what, "xbox_path_tree: Partition1\\%s is the game tree", names[i][0]);
            check(xbox_path_tree(guest) == XBOX_TREE_GAME, what);
        }
    }
}

/* The Xbox file system ignores case and titles rely on it (MEDIA\Sound.xwb
 * for media/sound.xwb). Windows and default macOS volumes resolve that by
 * themselves; on a case-sensitive one (Linux, the Deck) the POSIX backend
 * matches each missing component against its directory. Checked on every
 * host by whether the result opens, and by its exact spelling where the
 * host is case-sensitive. */
static void test_case_fallback(void)
{
    char dir[700], file[800], probe[800], want[1200];
    const char *p;
    FILE *f;
    int sensitive;

    printf("case-insensitive guest paths\n");
    recomp_env_set(RENV_SAVE_DIR, "");
    xbox_path_init(g_game, g_save);

    snprintf(dir, sizeof dir, "%s" SEP "Media", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "Media" SEP "Sound", g_game);
    make_dir(dir);
    snprintf(file, sizeof file, "%s" SEP "Bank.xwb", dir);
    f = fopen(file, "wb");
    if (f) fclose(f);
    snprintf(probe, sizeof probe, "%s" SEP "MEDIA", g_game);
    sensitive = !is_dir(probe);
    printf("  (host file system is case-%s)\n", sensitive ? "sensitive" : "insensitive");

    expect("\\Device\\Harddisk0\\Partition1\\Media\\Sound\\Bank.xwb", file,
           "an exact path is left alone");

    p = host("\\Device\\Harddisk0\\Partition1\\MEDIA\\sound\\BANK.XWB");
    f = fopen(p, "rb");
    check(f != NULL, "MEDIA\\sound\\BANK.XWB opens Media/Sound/Bank.xwb");
    if (f) fclose(f);
    if (sensitive)
        check(same(p, file), "and is rewritten to the name on disk");

    /* A create: the directories resolve, the new name is kept as written. */
    p = host("\\Device\\Harddisk0\\Partition1\\media\\SOUND\\New.bin");
    snprintf(want, sizeof want, "%s" SEP "New.bin", dir);
    if (sensitive)
        expect("\\Device\\Harddisk0\\Partition1\\media\\SOUND\\New.bin", want,
               "a new file lands in the existing directory");
    else
        check(p[0] != '\0', "a new file lands in the existing directory");

    /* A missing directory ends the walk; the rest stays as written. */
    snprintf(want, sizeof want, "%s" SEP "Media" SEP "Nope" SEP "a.bin", g_game);
    if (sensitive)
        expect("\\Device\\Harddisk0\\Partition1\\MEDIA\\Nope\\a.bin", want,
               "a missing directory keeps the rest as written");
}

/* Saves only in the game dir stay there (a build before the UDATA/TDATA
 * rules put them there); the cache never does, even with a CACHE left in
 * the game dir by such a build. */
static void test_legacy_saves(void)
{
    char want[1200], dir[700];

    printf("saves left in the game dir\n");
    recomp_env_set(RENV_SAVE_DIR, NULL);
    snprintf(dir, sizeof dir, "%s" SEP "UDATA", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "TDATA", g_game);
    make_dir(dir);
    snprintf(dir, sizeof dir, "%s" SEP "CACHE", g_game);
    make_dir(dir);
    xbox_path_init(g_game, g_save);

    snprintf(want, sizeof want, "%s" SEP "UDATA" SEP "4541005b", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\UDATA\\4541005b", want,
           "UDATA only in the game dir stays there");
    snprintf(want, sizeof want, "%s" SEP "TDATA" SEP "4541005b", g_game);
    expect("\\Device\\Harddisk0\\Partition1\\TDATA\\4541005b", want,
           "TDATA only in the game dir stays there");
    snprintf(want, sizeof want, "%s" SEP "TitleCache" SEP "LocalCache08.bin", g_save);
    expect("\\Device\\Harddisk0\\Partition1\\CACHE\\LocalCache08.bin", want,
           "CACHE still goes to <save>/TitleCache");
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) { fputs(text, f); fclose(f); }
}

/* The file's first line, or "" when it cannot be read. */
static const char *read_text(const char *path)
{
    static char buf[1024];
    FILE *f = fopen(path, "rb");
    buf[0] = '\0';
    if (f) {
        size_t n = fread(buf, 1, sizeof buf - 1, f);
        buf[n] = '\0';
        fclose(f);
    }
    return buf;
}

static int exists(const char *path)
{
    FILE *f;
    if (is_dir(path)) return 1;
    f = fopen(path, "rb");
    if (f) fclose(f);
    return f != NULL;
}

/* A file's or a folder's last write, in seconds since 1970. */
static long long mtime_of(const char *p)
{
#if defined(_WIN32)
    WIN32_FILE_ATTRIBUTE_DATA a;
    ULARGE_INTEGER t;
    if (!GetFileAttributesExA(p, GetFileExInfoStandard, &a)) return -1;
    t.LowPart = a.ftLastWriteTime.dwLowDateTime;
    t.HighPart = a.ftLastWriteTime.dwHighDateTime;
    return (long long)(t.QuadPart / 10000000ULL) - 11644473600LL;
#else
    struct stat st;
    return stat(p, &st) == 0 ? (long long)st.st_mtime : -1;
#endif
}

static void set_mtime(const char *p, long long t)
{
#if defined(_WIN32)
    FILETIME ft;
    ULARGE_INTEGER u;
    HANDLE h = CreateFileA(p, FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) return;
    u.QuadPart = (ULONGLONG)(t + 11644473600LL) * 10000000ULL;
    ft.dwLowDateTime = u.LowPart;
    ft.dwHighDateTime = u.HighPart;
    SetFileTime(h, NULL, &ft, &ft);
    CloseHandle(h);
#else
    struct timeval tv[2];
    tv[0].tv_sec = tv[1].tv_sec = (time_t)t;
    tv[0].tv_usec = tv[1].tv_usec = 0;
    utimes(p, tv);
#endif
}

/* Any <name>.copying* entry left in dir. */
static int copying_left(const char *dir, const char *name)
{
    char want[64];
    size_t n = (size_t)snprintf(want, sizeof want, "%s.copying", name);
    int found = 0;
#if defined(_WIN32)
    char pat[900];
    WIN32_FIND_DATAA fd;
    HANDLE f;
    snprintf(pat, sizeof pat, "%s\\*", dir);
    f = FindFirstFileA(pat, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do found |= _strnicmp(fd.cFileName, want, n) == 0;
    while (FindNextFileA(f, &fd));
    FindClose(f);
#else
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d) return 0;
    while ((e = readdir(d)) != NULL)
        found |= strncmp(e->d_name, want, n) == 0;
    closedir(d);
#endif
    return found;
}

/* A shared root as builds before the per-title default left it: two
 * titles' saves side by side. */
static void make_legacy(const char *legacy)
{
    char p[900];
    make_dir(legacy);
    snprintf(p, sizeof p, "%s" SEP "UDATA", legacy);             make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", legacy); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", legacy); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", legacy);
    write_text(p, "blinx2 save");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530013", legacy); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530013" SEP "other.dat", legacy);
    write_text(p, "blinx1 save");
    snprintf(p, sizeof p, "%s" SEP "TDATA", legacy);             make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065", legacy); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", legacy);
    write_text(p, "title data");
    snprintf(p, sizeof p, "%s" SEP "Cache", legacy);             make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "Cache" SEP "pelon000.rec", legacy);
    write_text(p, "another game's page");
}

/* xbox_path_adopt_legacy_saves on explicit roots, so both backends run it. */
static void test_adopt_legacy(void)
{
    char legacy[700], root[700], p[900];
    int rc;

    printf("saves copied from the shared root\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);

    snprintf(root, sizeof root, "%s" SEP "new1", g_root);
    make_dir(root);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 2, "UDATA and TDATA of this title copied (returns 2)");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    check(same(read_text(p), "blinx2 save"), "the save file is in the new root, same bytes");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
    check(same(read_text(p), "title data"), "the title data is in the new root");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530013", root);
    check(!exists(p), "another title's saves are not copied");
    snprintf(p, sizeof p, "%s" SEP "Cache", root);
    check(!exists(p), "the shared Cache is not copied");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying", root);
    check(!exists(p), "no .copying tree left");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", legacy);
    check(same(read_text(p), "blinx2 save"), "the shared root still has the save");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "the marker is written");

    /* Once only: a save the player deleted does not come back. */
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    remove(p);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 0 && !exists(p), "with the marker, nothing is copied again");

    /* A root with its own UDATA keeps it; a partial copy from a run that
     * stopped midway is redone. */
    snprintf(root, sizeof root, "%s" SEP "new2", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);                       make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root);        make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "mine.dat", root);
    write_text(p, "newer save");
    snprintf(p, sizeof p, "%s" SEP "TDATA", root);                       make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying", root); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying" SEP "half", root);
    write_text(p, "partial");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 1, "only TDATA copied when the root has its own UDATA");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "mine.dat", root);
    check(same(read_text(p), "newer save"), "the root's own save is left alone");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);
    check(!exists(p), "the shared save is not merged into it");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
    check(same(read_text(p), "title data"), "TDATA copied after a stale partial copy");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "half", root);
    check(!exists(p), "the partial copy is not taken for the save");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying", root);
    check(!exists(p), "the partial copy is gone");

    /* A title with nothing in the shared root. */
    snprintf(root, sizeof root, "%s" SEP "new3", g_root);
    make_dir(root);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x11111111);
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(rc == 0 && exists(p), "nothing to copy: 0, and the marker is written");
    check(xbox_path_adopt_legacy_saves(legacy, root, 0) == 0,
          "title id 0 does nothing");
}

/* The legacy save of make_legacy that the failure cases make unreadable. */
static void legacy_save(const char *legacy, char *out, size_t n)
{
    snprintf(out, n, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", legacy);
}

/* A copy that fails writes no marker and leaves no empty UDATA behind: an
 * empty UDATA in the root would hide the saves still kept in the game folder
 * until the next run. The next run copies it, with its times. */
static void test_adopt_failure(void)
{
    const long long t_file = 1104537600LL, t_dir = 1107216000LL;   /* 2005 */
    char legacy[700], root[700], save[900], p[900];
    int rc, blocked;
#if defined(_WIN32)
    HANDLE hold;
#endif

    printf("a failed copy is tried again and hides nothing\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);
    legacy_save(legacy, save, sizeof save);
    set_mtime(save, t_file);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", legacy);
    set_mtime(p, t_dir);

    snprintf(root, sizeof root, "%s" SEP "new4", g_root);
    make_dir(root);
#if defined(_WIN32)
    /* No sharing: CopyFileW cannot open it, as with a file another program
     * holds. */
    hold = CreateFileA(save, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    blocked = hold != INVALID_HANDLE_VALUE;
#else
    blocked = geteuid() != 0 && chmod(save, 0) == 0;   /* root reads anyway */
#endif
    if (blocked) {
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == -1, "an unreadable save fails the copy (returns -1)");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(!exists(p), "no marker after a failed copy");
        snprintf(p, sizeof p, "%s" SEP "UDATA", root);
        check(!exists(p), "no empty UDATA left to hide the game folder's saves");
        snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
        check(same(read_text(p), "title data"), "the folder that could be read is copied");
        snprintf(p, sizeof p, "%s" SEP "TDATA", root);
        check(!copying_left(p, "4d530065"), "no partial copy left");
    } else {
        printf("  (skipped: the save cannot be made unreadable here)\n");
    }
#if defined(_WIN32)
    if (blocked) CloseHandle(hold);
#else
    chmod(save, 0644);
#endif

    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == (blocked ? 1 : 2), "the next run copies the save");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    check(same(read_text(p), "blinx2 save"), "the save is in the new root");
    check(mtime_of(p) == t_file, "the save file keeps its last-write time");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);
    check(mtime_of(p) == t_dir,
          "the save folder keeps its date (XFindFirstSaveGame reports it)");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "the marker is written once the copy succeeds");
    if (blocked)
        check(strstr(read_text(p), "copied UDATA") && strstr(read_text(p), "already present TDATA"),
              "the marker names the copied and the already present folders");
}

/* A file the copy cannot read, as one another program holds open. */
typedef struct {
    int on;
#if defined(_WIN32)
    HANDLE h;
#endif
} blocked_file;

static blocked_file block_file(const char *p)
{
    blocked_file b;
#if defined(_WIN32)
    b.h = CreateFileA(p, GENERIC_READ, 0, NULL, OPEN_EXISTING, 0, NULL);
    b.on = b.h != INVALID_HANDLE_VALUE;
#else
    b.on = geteuid() != 0 && chmod(p, 0) == 0;   /* root reads anyway */
#endif
    return b;
}

static void unblock_file(const char *p, blocked_file *b)
{
#if defined(_WIN32)
    (void)p;
    if (b->on) CloseHandle(b->h);
#else
    chmod(p, 0644);
#endif
    b->on = 0;
}

/* The game between two attempts: BLiNX 2 makes an empty TDATA/<tid> at boot
 * (and UDATA/<tid> on its first save). An empty folder does not stop the
 * retry; one the player saved into is never overwritten. */
static void test_adopt_retry(void)
{
    char legacy[700], root[700], save[900], tbin[900], p[900];
    blocked_file bs, bt;
    int rc;

    printf("the game between a failed copy and the retry\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);
    legacy_save(legacy, save, sizeof save);
    snprintf(tbin, sizeof tbin, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", legacy);

    /* Both folders fail; the game then boots with empty ones. */
    snprintf(root, sizeof root, "%s" SEP "new10", g_root);
    make_dir(root);
    bs = block_file(save);
    bt = block_file(tbin);
    if (bs.on && bt.on) {
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == -1, "both folders fail");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        check(exists(p), "the failure is noted for UDATA");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-TDATA", root);
        check(exists(p), "and for TDATA");
        unblock_file(save, &bs);
        unblock_file(tbin, &bt);
        snprintf(p, sizeof p, "%s" SEP "TDATA", root);                make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065", root); make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "UDATA", root);                make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root); make_dir(p);

        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == 2, "the empty folders the game made do not stop the retry");
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
        check(same(read_text(p), "blinx2 save"), "the save is copied over the empty UDATA folder");
        snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
        check(same(read_text(p), "title data"), "the title data over the empty TDATA folder");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        check(!exists(p), "the failure notes are removed");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(strstr(read_text(p), "copied UDATA") && strstr(read_text(p), "TDATA"),
              "the marker names both folders as copied");
    } else {
        printf("  (skipped: the files cannot be made unreadable here)\n");
    }
    unblock_file(save, &bs);
    unblock_file(tbin, &bt);

    /* UDATA fails; the player then saves into the new root. */
    snprintf(root, sizeof root, "%s" SEP "new11", g_root);
    make_dir(root);
    bs = block_file(save);
    if (bs.on) {
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == -1, "UDATA fails");
        unblock_file(save, &bs);
        snprintf(p, sizeof p, "%s" SEP "UDATA", root);                make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root); make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "NEW", root); make_dir(p);
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "NEW" SEP "save.dat", root);
        write_text(p, "made after the failure");

        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == -1, "a folder saved into after a failed copy fails the retry");
        check(same(read_text(p), "made after the failure"), "the new save is kept");
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);
        check(!exists(p), "nothing is copied over it");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(!exists(p), "and no marker is written");

        /* After a copy by hand, deleting the note ends the check. */
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        remove(p);
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == 0, "without the note the folder counts as present");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(strstr(read_text(p), "already present UDATA") != NULL, "the marker says so");
    } else {
        printf("  (skipped: the save cannot be made unreadable here)\n");
    }
    unblock_file(save, &bs);
}

/* xbox_log writes to stderr: its lines go to path until capture_end, so a
 * case can check that a failure was reported. */
static int g_saved_err = -1;

static void capture_begin(const char *path)
{
    int fd;
    fflush(stderr);
#if defined(_WIN32)
    g_saved_err = _dup(2);
    fd = _open(path, _O_WRONLY | _O_CREAT | _O_TRUNC | _O_BINARY, 0644);
    if (fd >= 0) { _dup2(fd, 2); _close(fd); }
#else
    g_saved_err = dup(2);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) { dup2(fd, 2); close(fd); }
#endif
}

static void capture_end(void)
{
    fflush(stderr);
    if (g_saved_err < 0) return;
#if defined(_WIN32)
    _dup2(g_saved_err, 2);
    _close(g_saved_err);
#else
    dup2(g_saved_err, 2);
    close(g_saved_err);
#endif
    g_saved_err = -1;
}

/* Whether the captured log at path has needle. */
static int log_has(const char *path, const char *needle)
{
    static char buf[16384];
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    if (f) { n = fread(buf, 1, sizeof buf - 1, f); fclose(f); }
    buf[n] = '\0';
    return strstr(buf, needle) != NULL;
}

/* Every failure leaves the note, and a note that cannot be written is
 * logged: without it, a save made later in the new root would count as
 * present and the marker would leave the old saves behind unreported. */
static void test_adopt_notes(void)
{
    char legacy[700], root[700], p[900], q[900], log[700];
    int rc;

    printf("every failed folder leaves its note\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);
    snprintf(log, sizeof log, "%s" SEP "notes.log", g_root);

    /* The parent cannot be made: a file holds its name. */
    snprintf(root, sizeof root, "%s" SEP "new13", g_root);
    make_dir(root);
    snprintf(q, sizeof q, "%s" SEP "UDATA", root);
    write_text(q, "not a folder");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == -1, "UDATA that cannot be made fails the copy");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    check(exists(p), "and leaves the note");
    check(strstr(read_text(p), "copy the old ones by hand") != NULL,
          "the note says what to do");
    check(same(read_text(q), "not a folder"), "the file in the way is left alone");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(!exists(p), "no marker");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
    check(same(read_text(p), "title data"), "TDATA is still copied");

    /* No root at all: neither the folders nor the notes can be written. */
    snprintf(root, sizeof root, "%s" SEP "absent" SEP "new14", g_root);
    capture_begin(log);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    capture_end();
    check(rc == -1, "a root that is not there fails the copy");
    check(log_has(log, ".legacy-saves-failed-UDATA: cannot write the note"),
          "a note that cannot be written is logged");
    check(log_has(log, ".legacy-saves-failed-TDATA: cannot write the note"),
          "for each folder");
    check(!exists(root), "and nothing is made");

    /* A note whose folder is gone from the shared root is removed. */
    snprintf(root, sizeof root, "%s" SEP "new15", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    write_text(p, "from an earlier run");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x22222222);
    check(rc == 0 && !exists(p), "a note with nothing left to copy is removed");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "and the marker is written");

    /* The shared root has no UDATA at all (unmounted for now, say): the
     * note stays and no marker ends the retry. With UDATA back but no
     * folder for the title, the note goes and the marker is written. */
    {
        char bare[700];
        snprintf(bare, sizeof bare, "%s" SEP "legacy-bare", g_root);
        make_dir(bare);
        snprintf(root, sizeof root, "%s" SEP "new18", g_root);
        make_dir(root);
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        write_text(p, "from an earlier run");
        rc = xbox_path_adopt_legacy_saves(bare, root, 0x4d530065);
        check(rc == -1 && exists(p), "a note is kept while the whole UDATA is missing");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(!exists(p), "and no marker is written");
        snprintf(p, sizeof p, "%s" SEP "UDATA", bare);
        make_dir(p);
        rc = xbox_path_adopt_legacy_saves(bare, root, 0x4d530065);
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        check(rc == 0 && !exists(p), "with UDATA back the note is removed");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(exists(p), "and the marker is written");
    }

    /* A root where only the copy's name (it holds the pid) is too long. The
     * note is 27 characters past the root, the copy 24 plus the pid. */
    {
        char pid[24];
        size_t want, n;
#if defined(_WIN32)
        snprintf(pid, sizeof pid, "%lu", (unsigned long)GetCurrentProcessId());
#else
        snprintf(pid, sizeof pid, "%lu", (unsigned long)getpid());
#endif
        want = MAX_PATH - 24 - strlen(pid);
        n = (size_t)snprintf(root, sizeof root, "%s" SEP "long", g_root);
        /* The first step, like the others, needs two characters to go. */
        if (strlen(pid) < 4 || n + 2 > want) {
            printf("  (skipped: the pid is too short or the temp dir too long)\n");
            return;
        }
        make_dir(root);
        while (n < want) {
            /* Each step adds a separator and a part; never leave one
             * character to go, which no step can add. */
            size_t r = want - n;
            size_t part = r - 1 <= 60 ? r - 1 : (r - 61 == 1 ? 59 : 60);
            root[n++] = SEP[0];
            memset(root + n, 'd', part);
            n += part;
            root[n] = '\0';
            make_dir(root);
        }
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        check(rc == -1 && strlen(root) == want, "a copy name too long fails the copy");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
        check(exists(p), "and leaves the note");
        snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
        check(!exists(p), "no marker");
    }
}

/* Another instance while this one copies UDATA/<tid>: once the copy has
 * started, a folder built aside is renamed to the destination, as that
 * instance's finished copy or its game's first save would be. Only a rename
 * that made the destination counts as a hit, so a racer that comes after
 * the copy is a miss, never a false pass. */
typedef struct {
    char tmp[900], side[900], dst[900];
    atomic_int state;   /* 0 waiting, 1 hit, -1 stopped by the test */
} racer_args;

static int rename_dir(const char *from, const char *to)
{
#if defined(_WIN32)
    return MoveFileA(from, to) ? 0 : -1;
#else
    return rename(from, to);
#endif
}

#if defined(_WIN32)
static DWORD WINAPI racer(LPVOID arg)
#else
static void *racer(void *arg)
#endif
{
    racer_args *a = (racer_args *)arg;
    for (int i = 0; i < 60000 && atomic_load(&a->state) == 0; i++) {   /* 60 s at most */
        if (is_dir(a->tmp) && !is_dir(a->dst) && rename_dir(a->side, a->dst) == 0) {
            int waiting = 0;
            atomic_compare_exchange_strong(&a->state, &waiting, 1);
            break;
        }
#if defined(_WIN32)
        Sleep(1);
#else
        usleep(1000);
#endif
    }
#if defined(_WIN32)
    return 0;
#else
    return NULL;
#endif
}

#define BIG_OLD_TIME 1104537000LL

/* A shared root whose UDATA/<tid> holds OLD, many files that each get
 * flushed, so the copy stays open long enough to race. */
static void make_legacy_big(char *legacy, size_t n)
{
    char p[900];
    snprintf(legacy, n, "%s" SEP "legacy-big", g_root);
    make_dir(legacy);
    snprintf(p, sizeof p, "%s" SEP "UDATA", legacy);                         make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", legacy);          make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "OLD", legacy); make_dir(p);
    for (int i = 0; i < 300; i++) {
        snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "OLD" SEP "f%03d", legacy, i);
        write_text(p, "old save");
    }
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "OLD", legacy);
    set_mtime(p, BIG_OLD_TIME);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", legacy);
    set_mtime(p, 1104537600LL);
}

/* Copies legacy into a fresh <name>-<try> root while a racer drops the folder
 * that build(side) makes. A fast copy can beat the racer, so it tries up to
 * five times; every miss is a FAIL. root gets the root of the try that was
 * hit. Returns the copy's result, or -2 when every try missed. */
static int race_copy(const char *legacy, const char *name,
                     void (*build)(const char *side), char *root, size_t n)
{
    unsigned long pid;
    char p[900];
#if defined(_WIN32)
    pid = (unsigned long)GetCurrentProcessId();
#else
    pid = (unsigned long)getpid();
#endif
    for (int t = 0; t < 5; t++) {
        racer_args a;
        int rc;
#if defined(_WIN32)
        HANDLE th;
#else
        pthread_t th;
#endif
        snprintf(root, n, "%s" SEP "%s-%d", g_root, name, t);
        make_dir(root);
        snprintf(p, sizeof p, "%s" SEP "UDATA", root);
        make_dir(p);
        memset(&a, 0, sizeof a);
        atomic_init(&a.state, 0);
        snprintf(a.tmp, sizeof a.tmp, "%s" SEP "4d530065.copying.%lu", p, pid);
        snprintf(a.side, sizeof a.side, "%s" SEP "racer", p);
        snprintf(a.dst, sizeof a.dst, "%s" SEP "4d530065", p);
        make_dir(a.side);
        build(a.side);
#if defined(_WIN32)
        th = CreateThread(NULL, 0, racer, &a, 0, NULL);
        if (!th) { check(0, "thread"); return -2; }
#else
        if (pthread_create(&th, NULL, racer, &a) != 0) { check(0, "thread"); return -2; }
#endif
        rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
        {
            int waiting = 0;
            atomic_compare_exchange_strong(&a.state, &waiting, -1);
        }
#if defined(_WIN32)
        WaitForSingleObject(th, INFINITE);
        CloseHandle(th);
#else
        pthread_join(th, NULL);
#endif
        if (atomic_load(&a.state) == 1)
            return rc;
        printf("  (try %d: the copy ended before the racer)\n", t + 1);
    }
    check(0, "the racer reaches the copy in one of five tries");
    return -2;
}

/* Another instance's game saves into UDATA/<tid>. */
static void build_save(const char *side)
{
    char p[1000];
    snprintf(p, sizeof p, "%s" SEP "NEW", side);
    make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "NEW" SEP "save.dat", side);
    write_text(p, "saved during the copy");
}

/* Another instance's finished copy (OLD with the shared root's time), into
 * which its game has saved since. */
static void build_copy(const char *side)
{
    char p[1000];
    snprintf(p, sizeof p, "%s" SEP "OLD", side);
    make_dir(p);
    set_mtime(p, BIG_OLD_TIME);
    build_save(side);
}

/* A save made in the new root during the copy: the rename meets a folder
 * that does not hold the copy. The new save is kept, and the folder counts
 * as failed, with its note, since the old saves were not copied. */
static void test_adopt_save_during_copy(void)
{
    char legacy[700], root[700], p[900];
    int rc;

    printf("a save made in the new root during the copy\n");
    make_legacy_big(legacy, sizeof legacy);
    rc = race_copy(legacy, "new16", build_save, root, sizeof root);
    if (rc == -2)
        return;
    check(rc == -1, "the copy fails");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "NEW" SEP "save.dat", root);
    check(same(read_text(p), "saved during the copy"), "the new save is kept");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "OLD", root);
    check(!exists(p), "nothing is copied over it");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    check(exists(p), "the note says the old saves wait");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(!exists(p), "no marker");
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);
    check(!copying_left(p, "4d530065"), "no partial copy left");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == -1, "the next run still reports them");
}

/* Another instance's copy lands during this one's, and its game has already
 * saved into it: the folder holds the copy, so it counts as present. */
static void test_adopt_copy_during_copy(void)
{
    char legacy[700], root[700], p[900];
    int rc;

    printf("another instance's copy lands during the copy\n");
    make_legacy_big(legacy, sizeof legacy);
    rc = race_copy(legacy, "new19", build_copy, root, sizeof root);
    if (rc == -2)
        return;
    check(rc == 0, "the folder counts as present");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    check(!exists(p), "no note");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "the marker is written");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "NEW" SEP "save.dat", root);
    check(same(read_text(p), "saved during the copy"), "its new save is kept");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "OLD" SEP "f000", root);
    check(!exists(p), "this instance's copy is not mixed in");
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);
    check(!copying_left(p, "4d530065"), "no partial copy left");
}

/* A note from a run that lost the rename to another instance's copy, or
 * before a copy by hand that kept the times, is removed once the folder
 * holds the copy; it stays while the folder does not. */
static void test_adopt_note_cleared(void)
{
    char legacy[700], root[700], p[900];
    int rc;

    printf("a note for a folder that holds the copy\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy-held", g_root);
    make_dir(legacy);
    snprintf(p, sizeof p, "%s" SEP "UDATA", legacy);                           make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", legacy);            make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", legacy); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", legacy);
    write_text(p, "old save");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", legacy);
    set_mtime(p, BIG_OLD_TIME);

    snprintf(root, sizeof root, "%s" SEP "new20", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);                             make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root);              make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);  make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    write_text(p, "newer save");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);
    set_mtime(p, BIG_OLD_TIME + 60);
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    write_text(p, "from an earlier run");

    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == -1 && exists(p), "a folder with another time keeps the note");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);
    set_mtime(p, BIG_OLD_TIME);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    check(rc == 0 && !exists(p), "one that holds the copy removes it");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "and the marker is written");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    check(same(read_text(p), "newer save"), "the save there is left alone");

    /* An empty source has nothing to copy, so any folder holds it. A
     * listing that fails must never read this way. */
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy-empty", g_root);
    make_dir(legacy);
    snprintf(p, sizeof p, "%s" SEP "UDATA", legacy);                           make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", legacy);            make_dir(p);
    snprintf(root, sizeof root, "%s" SEP "new21", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);                             make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root);              make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1", root);  make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    write_text(p, "newer save");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-UDATA", root);
    write_text(p, "from an earlier run");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 0 && !exists(p), "an empty source removes the note");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(exists(p), "and the marker is written");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    check(same(read_text(p), "newer save"), "the save there is left alone");
}

#if defined(_WIN32)
/* An empty TDATA/<tid> that a running instance's game holds open is not
 * deleted from under it: the copy fails and the next run, with the folder
 * closed, copies it. */
static void test_adopt_held_open(void)
{
    char legacy[700], root[700], p[900];
    HANDLE h;
    int rc;

    printf("an empty folder another instance holds open\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);
    snprintf(root, sizeof root, "%s" SEP "new17", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "TDATA", root);                make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065", root); make_dir(p);
    h = CreateFileA(p, GENERIC_READ,
                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                    NULL, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
    if (h == INVALID_HANDLE_VALUE) { check(0, "open the folder"); return; }
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == -1, "a folder held open fails the copy");
    CloseHandle(h);
    check(is_dir(p), "and is still there once it is closed");
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves-failed-TDATA", root);
    check(exists(p), "the note is left");
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 1, "the next run copies TDATA");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
    check(same(read_text(p), "title data"), "the title data is in place");
}
#endif

#if defined(_WIN32)
/* CopyFileW keeps the read-only attribute: the flush after the copy and the
 * removal of a gone process's partial copy must both get past it. */
static void test_adopt_readonly(void)
{
    char legacy[700], root[700], save[900], p[900];
    DWORD a;
    int rc;

    printf("read-only save files\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
    make_legacy(legacy);
    legacy_save(legacy, save, sizeof save);
    SetFileAttributesA(save, FILE_ATTRIBUTE_READONLY);

    snprintf(root, sizeof root, "%s" SEP "new12", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "TDATA", root); make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying.4294967292", root);
    make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying.4294967292" SEP "ro.bin", root);
    write_text(p, "partial");
    SetFileAttributesA(p, FILE_ATTRIBUTE_READONLY);

    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 2, "a read-only save is copied");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    check(same(read_text(p), "blinx2 save"), "with its contents");
    a = GetFileAttributesA(p);
    check(a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY),
          "and still read-only");
    snprintf(p, sizeof p, "%s" SEP "TDATA", root);
    check(!copying_left(p, "4d530065"), "a partial copy with a read-only file is removed");

    SetFileAttributesA(save, FILE_ATTRIBUTE_NORMAL);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", root);
    SetFileAttributesA(p, FILE_ATTRIBUTE_NORMAL);   /* for the cleanup */
}
#endif

/* Partial copies: removed when their process is gone (or is this one) even
 * when the folder itself is already in place; another live instance's is
 * left alone. */
static void test_adopt_stale(void)
{
    char legacy[700], root[700], p[900];
    unsigned long dead;
    int rc;

    printf("partial copies of gone processes are removed\n");
    snprintf(legacy, sizeof legacy, "%s" SEP "legacy", g_root);
#if defined(_WIN32)
    dead = 0xfffffffcUL;                  /* no such process */
#else
    {
        pid_t c = fork();
        if (c == 0) _exit(0);
        waitpid(c, NULL, 0);
        dead = (unsigned long)c;
    }
#endif
    snprintf(root, sizeof root, "%s" SEP "new5", g_root);
    make_dir(root);
    snprintf(p, sizeof p, "%s" SEP "UDATA", root);                         make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", root);          make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "own.dat", root);
    write_text(p, "the player's own");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying", root);  make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying.%lu", root, dead);
    make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA", root);                         make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying.%lu", root,
#if defined(_WIN32)
             (unsigned long)GetCurrentProcessId());
#else
             (unsigned long)getpid());
#endif
    make_dir(p);
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065.copying.%lu" SEP "half", root,
#if defined(_WIN32)
             (unsigned long)GetCurrentProcessId());
#else
             (unsigned long)getpid());
#endif
    write_text(p, "partial");
#if !defined(_WIN32)
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying.%lu", root,
             (unsigned long)getppid());
    make_dir(p);
#endif

    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    check(rc == 1, "only TDATA is copied");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "own.dat", root);
    check(same(read_text(p), "the player's own"), "the save already in place is kept");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying", root);
    check(!exists(p), "a crash's .copying is removed although UDATA is in place");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying.%lu", root, dead);
    check(!exists(p), "a gone process's copy is removed");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "t.bin", root);
    check(same(read_text(p), "title data"), "a copy under this process's id is redone");
    snprintf(p, sizeof p, "%s" SEP "TDATA" SEP "4d530065" SEP "half", root);
    check(!exists(p), "and its partial file is not kept");
#if !defined(_WIN32)
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065.copying.%lu", root,
             (unsigned long)getppid());
    check(exists(p), "a live process's copy is left alone");
#endif
    snprintf(p, sizeof p, "%s" SEP ".legacy-saves", root);
    check(strstr(read_text(p), "copied TDATA") && strstr(read_text(p), "already present UDATA"),
          "the marker says UDATA was already present");
}

#if !defined(_WIN32)
/* A link to a file is copied as the file; a link to a folder, or one to
 * nothing, fails the copy rather than leave a save with a part missing. */
static void test_adopt_links(void)
{
    char legacy[700], root[700], p[900], q[900];
    struct stat st;
    int rc;

    printf("links in the shared root\n");
    snprintf(legacy, sizeof legacy, "%s/legacy-links", g_root);
    make_dir(legacy);
    snprintf(p, sizeof p, "%s/UDATA", legacy);                 make_dir(p);
    snprintf(p, sizeof p, "%s/UDATA/4d530065", legacy);        make_dir(p);
    snprintf(q, sizeof q, "%s/outside.dat", g_root);
    write_text(q, "linked save");
    snprintf(p, sizeof p, "%s/UDATA/4d530065/link.dat", legacy);
    symlink(q, p);

    snprintf(root, sizeof root, "%s/new6", g_root);
    make_dir(root);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    snprintf(p, sizeof p, "%s/UDATA/4d530065/link.dat", root);
    check(rc == 1 && lstat(p, &st) == 0 && S_ISREG(st.st_mode)
          && same(read_text(p), "linked save"),
          "a link to a file is copied as the file");

    snprintf(p, sizeof p, "%s/UDATA/4d530065/dirlink", legacy);
    symlink(g_game, p);
    snprintf(root, sizeof root, "%s/new7", g_root);
    make_dir(root);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    snprintf(p, sizeof p, "%s/.legacy-saves", root);
    snprintf(q, sizeof q, "%s/UDATA", root);
    check(rc == -1 && !exists(p) && !exists(q),
          "a link to a folder fails the copy: no marker, no UDATA");
    snprintf(p, sizeof p, "%s/UDATA/4d530065/dirlink", legacy);
    remove(p);

    snprintf(p, sizeof p, "%s/UDATA/4d530065/gone", legacy);
    snprintf(q, sizeof q, "%s/nothing-here", g_root);
    symlink(q, p);
    snprintf(root, sizeof root, "%s/new8", g_root);
    make_dir(root);
    rc = xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065);
    snprintf(p, sizeof p, "%s/.legacy-saves", root);
    check(rc == -1 && !exists(p), "a dangling link fails the copy");
}

/* Two or more instances started together on a new root: each folder is
 * copied once, and none sees a half copy. */
static void test_adopt_race(void)
{
    enum { KIDS = 4 };
    char legacy[700], root[700], p[900];
    pid_t kid[KIDS];
    int go[2], total = 0, bad = 0;

    printf("instances started together\n");
    snprintf(legacy, sizeof legacy, "%s/legacy", g_root);
    snprintf(root, sizeof root, "%s/new9", g_root);
    make_dir(root);
    if (pipe(go) != 0) { check(0, "pipe"); return; }
    for (int i = 0; i < KIDS; i++) {
        kid[i] = fork();
        if (kid[i] == 0) {
            char c;
            close(go[1]);
            (void)!read(go[0], &c, 1);    /* all start when the pipe closes */
            _exit(xbox_path_adopt_legacy_saves(legacy, root, 0x4d530065) + 1);
        }
    }
    close(go[0]);
    close(go[1]);
    for (int i = 0; i < KIDS; i++) {
        int status = 0;
        waitpid(kid[i], &status, 0);
        if (!WIFEXITED(status) || WEXITSTATUS(status) == 0) bad++;
        else total += WEXITSTATUS(status) - 1;
    }
    check(!bad, "no instance fails");
    check(total == 2, "UDATA and TDATA are each copied by one instance");
    snprintf(p, sizeof p, "%s/UDATA/4d530065/SAVE1/save.dat", root);
    check(same(read_text(p), "blinx2 save"), "the save is whole");
    snprintf(p, sizeof p, "%s/UDATA", root);
    check(!copying_left(p, "4d530065"), "no partial copy left in UDATA");
    snprintf(p, sizeof p, "%s/TDATA", root);
    check(!copying_left(p, "4d530065"), "nor in TDATA");
    snprintf(p, sizeof p, "%s/.legacy-saves", root);
    check(exists(p), "the marker is written");
}
#endif

/* Point the per-user data folder ($XDG_DATA_HOME, %LOCALAPPDATA%) at v, or
 * restore it (NULL). */
static void set_data_home(const char *v)
{
#if defined(_WIN32)
    static char saved[MAX_PATH];
    static int have_saved = -1;
    if (have_saved < 0) {
        const char *o = getenv("LOCALAPPDATA");
        have_saved = o != NULL;
        snprintf(saved, sizeof saved, "%s", o ? o : "");
    }
    _putenv_s("LOCALAPPDATA", v ? v : (have_saved ? saved : ""));
#else
    if (v) setenv("XDG_DATA_HOME", v, 1);
    else unsetenv("XDG_DATA_HOME");
#endif
}

/* The default root with no save dir from the game: one per title id, under
 * $XDG_DATA_HOME or %LOCALAPPDATA%. */
static void test_title_default(void)
{
    char xdg[700], legacy[800], a[900], b[900], root[900], p[1200];

    printf("default root per title\n");
    recomp_env_set(RENV_SAVE_DIR, NULL);
    snprintf(xdg, sizeof xdg, "%s" SEP "xdg", g_root);
    make_dir(xdg);
    set_data_home(xdg);
    snprintf(legacy, sizeof legacy, "%s" SEP "xboxrecomp", xdg);
    make_legacy(legacy);

    xbox_path_set_title_id(0x4d530065);
    xbox_path_init(g_game, NULL);
    snprintf(a, sizeof a, "%s" SEP "4d530065", legacy);
    check(xbox_path_hdd_root(root, sizeof root) && same(root, a),
          "title 4d530065: root <data>/xboxrecomp/4d530065");
    snprintf(p, sizeof p, "%s" SEP "Cache" SEP "blinx2_000.rec", a);
    expect("Z:\\blinx2_000.rec", p, "Z: goes to the title's own Cache");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", a);
    check(same(host("\\Device\\Harddisk0\\Partition1\\UDATA\\4d530065\\SAVE1\\save.dat"), p)
          && same(read_text(p), "blinx2 save"),
          "the shared root's save is found through the title's UDATA");

    xbox_path_set_title_id(0x4d530013);
    xbox_path_init(g_game, NULL);
    snprintf(b, sizeof b, "%s" SEP "4d530013", legacy);
    check(xbox_path_hdd_root(root, sizeof root) && same(root, b),
          "title 4d530013: root <data>/xboxrecomp/4d530013");
    snprintf(p, sizeof p, "%s" SEP "Cache" SEP "pelon000.rec", b);
    expect("Z:\\pelon000.rec", p, "Z: goes to that title's Cache");
    check(!exists(p), "its Cache starts empty (no shared pages)");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530013" SEP "other.dat", b);
    check(same(read_text(p), "blinx1 save"), "that title's save is found too");
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065", b);
    check(!exists(p), "neither title gets the other's saves");

    /* The second init of a root copies nothing (the marker). */
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d530065" SEP "SAVE1" SEP "save.dat", a);
    remove(p);
    xbox_path_set_title_id(0x4d530065);
    xbox_path_init(g_game, NULL);
    check(!exists(p), "a later run does not copy again");

    /* RECOMP_SAVE_DIR names the saves: nothing is copied. */
    snprintf(p, sizeof p, "%s" SEP "UDATA" SEP "4d5300ff", legacy);
    make_dir(p);
    recomp_env_set(RENV_SAVE_DIR, g_user);
    xbox_path_set_title_id(0x4d5300ff);
    xbox_path_init(g_game, NULL);
    snprintf(p, sizeof p, "%s" SEP "4d5300ff" SEP ".legacy-saves", legacy);
    check(!exists(p), "with RECOMP_SAVE_DIR, no copy and no marker");
    recomp_env_set(RENV_SAVE_DIR, NULL);

    /* Before the title id is known: the shared root, as before. */
    xbox_path_set_title_id(0);
    xbox_path_init(g_game, NULL);
    check(xbox_path_hdd_root(root, sizeof root) && same(root, legacy),
          "no title id: the shared root");

    /* A save dir from the game wins over the per-title default. */
    xbox_path_set_title_id(0x4d530065);
    xbox_path_init(g_game, g_save);
    check(xbox_path_hdd_root(root, sizeof root) && same(root, g_save),
          "a save dir from the game is used as is");
    xbox_path_set_title_id(0);
    set_data_home(NULL);
}

int main(int argc, char **argv)
{
    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        unsigned n = 0;
        GetTempPathA(MAX_PATH, tmp);
        /* Wine reuses process ids from one run to the next. */
        do snprintf(g_root, sizeof g_root, "%skpath-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(g_root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/kpathXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(g_root, sizeof g_root, "%s", tmpl);
    }
#endif
    snprintf(g_game, sizeof g_game, "%s" SEP "game", g_root);
    snprintf(g_save, sizeof g_save, "%s" SEP "save", g_root);
    snprintf(g_user, sizeof g_user, "%s" SEP "user", g_root);
    make_dir(g_game);

    test_default_root();
    test_save_dir_root();
    test_case_fallback();
    test_legacy_saves();
    test_adopt_legacy();
    test_adopt_failure();
    test_adopt_retry();
    test_adopt_stale();
    test_adopt_notes();
    test_adopt_save_during_copy();
    test_adopt_copy_during_copy();
    test_adopt_note_cleared();
#if defined(_WIN32)
    test_adopt_readonly();
    test_adopt_held_open();
#endif
#if !defined(_WIN32)
    test_adopt_links();
    test_adopt_race();
#endif
    test_title_default();
    remove_root();

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
