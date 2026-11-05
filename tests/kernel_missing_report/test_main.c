/*
 * The missing-game-file report (kernel_missing.c).
 *
 * A title fails opens on purpose all the time -- cache probes, numbered-file
 * scans, save metadata, loose-file fallbacks -- so the report has to name the
 * file a dump lacks and stay quiet about the rest. This drives each condition
 * that decides it on and off over a temporary game directory, the three
 * modes, the repeat suppression, the table cap and concurrent callers.
 *
 * Each failed open is simulated the way the bridge sees it: the path is
 * translated first (which fills the thread's last host path, as the failed
 * NtCreateFile would), then the bridge's own sequence runs -- ask whether the
 * FAILED line is a repeat, then note the failure.
 *
 * Cross-platform: Win32 backend on Windows and Proton, POSIX elsewhere.
 */
#include "kernel.h"
#include "kernel_missing.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

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

#if defined(_WIN32)
#define SEP "\\"
#else
#define SEP "/"
#endif

#define NAME_NF 0xC0000034u
#define PATH_NF 0xC000003Au
#define READ    0x80100080u     /* GENERIC_READ | SYNCHRONIZE | FILE_READ_ATTRIBUTES */
#define OPTS    0x00000060u     /* FILE_NON_DIRECTORY_FILE | FILE_SYNCHRONOUS_IO_NONALERT */

static int failures;
static char g_root[512], g_game[512], g_log[600], g_list[600];
static volatile LONG g_printed;     /* FAILED lines the bridge would print */

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

/* One failed open, as bridge_create_file_impl handles it. */
static void fail(const char *guest, uint32_t status, uint32_t access,
                 uint32_t disp, uint32_t opts)
{
    xbox_host_char host[MAX_PATH];
    if (guest[0] == '\\' || guest[1] == ':')
        xbox_translate_path(guest, host, MAX_PATH);
    if (!xbox_missing_is_reported(guest))
        InterlockedIncrement(&g_printed);
    xbox_missing_note(guest, status, access, disp, opts, 0);
}

static void probe(const char *guest)
{
    xbox_host_char host[MAX_PATH];
    xbox_translate_path(guest, host, MAX_PATH);
    xbox_missing_note(guest, NAME_NF, 0, XBOX_FILE_OPEN, 0, 1);
}

/* Drop the '\r' the Win32 CRT writes before each '\n' of a text-mode
 * stream, so one expected line matches on every host. */
static void unix_eol(char *b)
{
    char *w = b;
    for (; *b; b++)
        if (*b != '\r') *w++ = *b;
    *w = '\0';
}

/* Occurrences of s in the captured stderr so far. */
static int log_count(const char *s)
{
    FILE *f;
    static char buf[1 << 22];
    size_t n;
    int c = 0;
    const char *p;

    fflush(stderr);
    f = fopen(g_log, "rb");
    if (!f) return -1;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = '\0';
    unix_eol(buf);
    for (p = buf; (p = strstr(p, s)) != NULL; p += strlen(s))
        c++;
    return c;
}

static void read_file(const char *path, char *out, size_t cap)
{
    FILE *f = fopen(path, "rb");
    size_t n = 0;
    if (f) { n = fread(out, 1, cap - 1, f); fclose(f); }
    out[n] = '\0';
    unix_eol(out);
}

static void mode(const char *v)
{
    recomp_env_set(RENV_MISSING, v);
    xbox_missing_reset();
    g_printed = 0;
}

static void test_tree(void)
{
    XBOX_ANSI_STRING l, t;
    printf("xbox_path_tree\n");
    check(xbox_path_tree("D:\\adx\\a.adx") == XBOX_TREE_GAME, "D: is the game tree");
    check(xbox_path_tree("\\??\\D:\\a") == XBOX_TREE_GAME, "\\??\\D: is the game tree");
    check(xbox_path_tree("\\Device\\CdRom0\\a") == XBOX_TREE_GAME, "CdRom0\\ is the game tree");
    check(xbox_path_tree("Y:\\default.xip") == XBOX_TREE_GAME, "Y: is the game tree");
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\x") == XBOX_TREE_GAME, "Partition1\\ (E:) is the game tree");
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition1\\UDATA\\x") == XBOX_TREE_USER, "UDATA is the user tree");
    check(xbox_path_tree("T:\\x") == XBOX_TREE_HDD, "T: is the HDD tree");
    check(xbox_path_tree("U:\\x") == XBOX_TREE_HDD, "U: is the HDD tree");
    check(xbox_path_tree("Z:\\media\\a.ipk") == XBOX_TREE_HDD, "Z: (no link) is the HDD tree");
    check(xbox_path_tree("\\Device\\Harddisk0\\Partition3") == XBOX_TREE_DEVICE, "a bare partition is a device");
    check(xbox_path_tree("\\Device\\CdRom0") == XBOX_TREE_DEVICE, "the bare CdRom0 is a device");
    check(xbox_path_tree("title.bin") == XBOX_TREE_UNKNOWN, "a relative name is unknown");
    check(xbox_path_tree("Q:\\x") == XBOX_TREE_UNKNOWN, "an unmapped letter is unknown");

    /* Wreckless: z: linked to the game partition loads assets through it. */
    l.Buffer = (PCHAR)"\\??\\Q:"; l.Length = 6; l.MaximumLength = 7;
    t.Buffer = (PCHAR)"\\Device\\Harddisk0\\Partition1\\"; t.Length = (USHORT)strlen(t.Buffer);
    t.MaximumLength = (USHORT)(t.Length + 1);
    xbox_IoCreateSymbolicLink(&l, &t);
    check(xbox_path_tree("q:\\data\\x.dat") == XBOX_TREE_GAME, "a title link to Partition1\\ is the game tree");
}

static void test_default(void)
{
    char want[1024], list[8192];
    uint32_t hi, lo, at;
    LONG before;
    int i;

    printf("default mode\n");
    mode(NULL);
    check(xbox_missing_verbose() == 0, "the default is not verbose");
    recomp_env_set(RENV_MISSING_LIST, g_list);

    fail("D:\\adx\\track01.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    snprintf(want, sizeof want, "  [FILE] missing D:\\adx\\track01.adx -> %s" SEP "adx" SEP "track01.adx\n", g_game);
    check(log_count(want) == 1, "a miss under an existing directory is named");
    check(g_printed == 1, "its first FAILED line prints");

    for (i = 0; i < 9; i++)
        fail(i & 1 ? "d:\\ADX\\TRACK01.ADX" : "D:\\adx\\track01.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("[FILE] missing") == 1, "repeats, in any case, are named once");
    check(g_printed == 1, "repeats of a reported path print no FAILED line");

    fail("d:\\media\\event\\EVCAMST0101_005.CAM", PATH_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("d:\\media\\event\\EVCAMST0101_005.CAM", PATH_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("not found") == 0, "a miss under an absent directory prints nothing");
    check(g_printed == 3, "its FAILED lines all print (it was never reported)");

    before = g_printed;
    xbox_missing_counts(&hi, &lo, &at);
    fail("D:\\adx\\a.adx", NAME_NF, READ, 3 /* FILE_OPEN_IF */, OPTS);
    fail("D:\\adx\\b.adx", NAME_NF, READ | 0x40000000u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\c.adx", NAME_NF, READ | 0x10000000u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\d.adx", NAME_NF, READ | 0x2u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\e.adx", NAME_NF, READ | 0x4u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\f.adx", NAME_NF, READ | 0x10u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\g.adx", NAME_NF, READ | 0x100u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\h.adx", NAME_NF, READ | 0x10000u, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\dir", NAME_NF, READ, XBOX_FILE_OPEN, OPTS | XBOX_FILE_DIRECTORY_FILE);
    fail("track01.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\i.adx", 0xC0000043u /* SHARING_VIOLATION */, READ, XBOX_FILE_OPEN, OPTS);
    fail("z:\\media\\plcom_tex.ipk", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("U:\\13C91777168C\\SaveMeta.xbx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("\\Device\\Harddisk0\\Partition1\\UDATA\\x", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    probe("D:\\adx\\probe.adx");
    {
        uint32_t h2, l2, a2;
        xbox_missing_counts(&h2, &l2, &a2);
        check(h2 == hi && l2 == lo && a2 == at,
              "creates, writes, dirs, relative names, other statuses, trees, probes: not counted");
    }
    check(log_count("[FILE] missing") == 1 && log_count("not found") == 0, "... and not printed");
    check(g_printed == before + 14, "... and their FAILED lines all print");

    xbox_missing_counts(&hi, &lo, &at);
    check(hi == 1 && lo == 1 && at == 12, "counts: 1 high, 1 low, 12 attempts");

    /* A window's close: the hook the first note registered, then atexit. */
    recomp_exit_hook_run("window");
    xbox_missing_summary("exit");
    check(log_count("  [FILE] summary: 1 game file missing, 1 not found under absent directories, "
                    "12 failed opens, 9 repeats not printed (RECOMP_TRACE=missing=all lists them)\n") == 1,
          "the window exit hook prints the summary once, with the repeats left out");

    read_file(g_list, list, sizeof list);
    snprintf(want, sizeof want, "high 10 D:\\adx\\track01.adx %s" SEP "adx" SEP "track01.adx\n", g_game);
    check(strstr(list, want) != NULL, "missing_list: the high path with its attempts");
    check(strstr(list, "low 2 d:\\media\\event\\EVCAMST0101_005.CAM ") != NULL, "missing_list: the low path");
    /* A second exit (POSIX firmware, then atexit) leaves the list alone. */
    fail("D:\\adx\\other.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    xbox_missing_summary("exit");
    read_file(g_list, list, sizeof list);
    check(strstr(list, "other.adx") == NULL && strstr(list, "track01.adx ") != NULL,
          "missing_list is written once, by the first summary");
    /* A new run (reset) rewrites it rather than appending. */
    xbox_missing_reset();
    fail("D:\\adx\\other.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    xbox_missing_summary("exit");
    read_file(g_list, list, sizeof list);
    check(strstr(list, "other.adx") != NULL && strstr(list, "track01.adx") == NULL,
          "missing_list is rewritten, not appended");
    recomp_env_set(RENV_MISSING_LIST, NULL);
}

static void test_complete_dump(void)
{
    int before;
    printf("a complete dump (probes only)\n");
    mode(NULL);
    before = log_count("[FILE] summary");
    fail("d:\\media\\event\\EVCAMST0101_005.CAM", PATH_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("z:\\media\\uniform_tex.ipk", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    xbox_missing_summary("exit");
    check(log_count("[FILE] summary") == before, "no summary when no game file is missing");
}

static void test_all(void)
{
    uint32_t hi, lo, at;
    int i;

    printf("RECOMP_TRACE=missing=all\n");
    mode("all");
    check(xbox_missing_verbose() == 1, "all is verbose: NtOpenFile and IoCreateFile successes print");
    for (i = 0; i < 5; i++)
        fail("D:\\adx\\track02.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(g_printed == 5, "every FAILED line prints, repeats included");
    check(log_count("[FILE] missing D:\\adx\\track02.adx") == 1, "the miss is still named once");
    fail("d:\\media\\se\\dsp.bin", PATH_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("  [FILE] not found (no such directory) d:\\media\\se\\dsp.bin -> ") == 1,
          "a low miss is listed");
    fail("z:\\media\\stg0101_se.ipk", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("  [FILE] not found (hdd) z:\\media\\stg0101_se.ipk -> ") == 1, "an HDD miss is listed");
    fail("\\Device\\Harddisk0\\Partition1\\TDATA\\x", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("  [FILE] not found (user) \\Device\\Harddisk0\\Partition1\\TDATA\\x -> ") == 1,
          "a save-tree miss is listed");
    fail("title.bin", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("  [FILE] not found (unknown) title.bin -> ?\n") == 1, "a relative miss is listed as unknown");
    probe("D:\\adx\\maybe.adx");
    check(log_count("  [FILE] not found (game, probe) D:\\adx\\maybe.adx -> ") == 1, "a probe is listed");
    fail("D:\\adx\\create.adx", NAME_NF, READ, 2 /* FILE_CREATE */, OPTS);
    /* The Win32 path layer logs the translation ([PATH]); only report lines count. */
    check(log_count("[FILE] missing D:\\adx\\create.adx") == 0 &&
          log_count(") D:\\adx\\create.adx -> ") == 0, "a create is still not a miss");
    xbox_missing_counts(&hi, &lo, &at);
    check(hi == 1 && lo == 1 && at == 6, "counts: 1 high, 1 low, 6 attempts");
    xbox_missing_summary("exit");
    check(log_count("  [FILE] summary: 1 game file missing, 1 not found under absent directories, "
                    "6 failed opens, 3 other, 1 probe [at exit]\n") == 1,
          "the summary adds other and probes and its exit, no repeats");
}

static void test_off(void)
{
    uint32_t hi, lo, at;
    int lines;
    printf("RECOMP_TRACE=missing=0\n");
    mode("0");
    check(xbox_missing_verbose() == 0, "0 is not verbose");
    lines = log_count("[FILE]");
    fail("D:\\adx\\track01.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    fail("D:\\adx\\track01.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    xbox_missing_summary("exit");
    xbox_missing_counts(&hi, &lo, &at);
    check(log_count("[FILE]") == lines, "no line at all");
    check(hi == 0 && lo == 0 && at == 0, "nothing recorded");
    check(g_printed == 2, "every FAILED line prints, as before the report");
}

static void test_cap(void)
{
    char p[64];
    uint32_t hi;
    int i;
    printf("the 4096-path table\n");
    mode(NULL);
    for (i = 0; i < 4097; i++) {
        snprintf(p, sizeof p, "D:\\adx\\cap%04d.adx", i);
        fail(p, NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    }
    xbox_missing_counts(&hi, NULL, NULL);
    check(hi == 4096, "4096 paths are named");
    check(log_count("missing-file table full") == 1, "the 4097th is counted, and the cap said once");
    fail("D:\\adx\\cap4096.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    check(log_count("missing-file table full") == 1, "... only once");
    check(g_printed == 4098, "a path past the cap keeps printing its FAILED lines");
    xbox_missing_summary("exit");
    check(log_count("4096 game files missing, 0 not found under absent directories, 4098 failed opens, "
                    "2 past the 4096-path table\n") == 1, "the summary counts the unnamed");
}

#define THREADS 8
#define ROUNDS  500

static DWORD WINAPI worker(LPVOID arg)
{
    int t = (int)(intptr_t)arg, i;
    char own[64];
    for (i = 0; i < ROUNDS; i++) {
        fail("D:\\adx\\shared.adx", NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
        snprintf(own, sizeof own, "D:\\adx\\t%d_%d.adx", t, i % 10);
        fail(own, NAME_NF, READ, XBOX_FILE_OPEN, OPTS);
    }
    return 0;
}

static void test_threads(void)
{
    HANDLE th[THREADS];
    uint32_t hi, lo, at;
    int i, lines_before;
    printf("concurrent callers\n");
    mode(NULL);
    lines_before = log_count("[FILE] missing");
    for (i = 0; i < THREADS; i++)
        th[i] = CreateThread(NULL, 0, worker, (LPVOID)(intptr_t)i, 0, NULL);
    for (i = 0; i < THREADS; i++)
        WaitForSingleObject(th[i], INFINITE);
    xbox_missing_counts(&hi, &lo, &at);
    check(at == THREADS * ROUNDS * 2, "every attempt is counted");
    check(hi == 1 + THREADS * 10, "every unique path is counted once");
    check(log_count("[FILE] missing") - lines_before == (int)hi, "every unique path is named once");
    /* A FAILED line prints for each path's first attempt; a racing thread
     * may print one more before the path is classified, never fewer. */
    check(g_printed >= (LONG)hi && g_printed <= (LONG)hi + THREADS * (LONG)hi,
          "FAILED lines: one per path, give or take a race");
    xbox_missing_summary("exit");
    {
        char want[200];
        snprintf(want, sizeof want, "%u failed opens, %ld repeats not printed",
                 (unsigned)at, (long)(at - g_printed));
        check(log_count(want) == 1, "repeats not printed + FAILED lines printed = attempts");
    }
}

int main(int argc, char **argv)
{
    /* argv[1]: write the report there (Proton drops a console exe's stdout) */
    if (argc > 1 && !freopen(argv[1], "w", stdout)) return 2;
    setvbuf(stdout, NULL, _IONBF, 0);
#if defined(_WIN32)
    {
        char tmp[MAX_PATH];
        GetTempPathA(MAX_PATH, tmp);
        unsigned n = 0;
        /* Wine reuses process ids from one run to the next, so the id
         * alone can name a directory an earlier run left behind. */
        do snprintf(g_root, sizeof g_root, "%smissrep-%lu-%u", tmp, (unsigned long)GetCurrentProcessId(), n);
        while (!CreateDirectoryA(g_root, NULL) && ++n < 1000);
        if (n == 1000) return 2;
    }
#else
    {
        char tmpl[] = "/tmp/missrepXXXXXX";
        if (!mkdtemp(tmpl)) return 2;
        snprintf(g_root, sizeof g_root, "%s", tmpl);
    }
#endif
    snprintf(g_game, sizeof g_game, "%s" SEP "game", g_root);
    make_dir(g_game);
    {
        char adx[600];
        snprintf(adx, sizeof adx, "%s" SEP "adx", g_game);
        make_dir(adx);
    }
    snprintf(g_log, sizeof g_log, "%s" SEP "stderr.log", g_root);
    snprintf(g_list, sizeof g_list, "%s" SEP "missing.txt", g_root);
    if (!freopen(g_log, "w", stderr)) return 2;
    {
        char save[600];
        snprintf(save, sizeof save, "%s" SEP "save", g_root);
        xbox_path_init(g_game, save);
    }

    test_tree();
    test_default();
    test_complete_dump();
    test_all();
    test_off();
    test_cap();
    test_threads();

    printf("%s (%d failure%s)\n", failures ? "FAILED" : "OK", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
