/* The file browser's model (file_browser.h, openspec change overlay-revamp
 * D6 and FB.4) on a fresh temp folder: nothing here reads or writes the
 * developer's own folders (HOME is pointed into the temp folder too). The
 * links and the unreadable folder are POSIX-only; the rest also runs on
 * Windows. */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>
#endif

#include "file_browser.h"

static int s_checks, s_failed;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        s_checks++;                                                                                \
        if (!(x)) {                                                                                \
            s_failed++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)

static char T[FB_PATH_MAX]; /* the temp folder */

static void path(char *out, const char *rel)
{
    snprintf(out, FB_PATH_MAX, "%s/%s", T, rel);
#ifdef _WIN32
    for (; *out; out++)
        if (*out == '/')
            *out = '\\';
#endif
}

static void put(const char *rel, const char *text)
{
    char p[FB_PATH_MAX];
    FILE *f;
    path(p, rel);
    f = fopen(p, "wb");
    if (!f) {
        printf("cannot write %s\n", p);
        exit(2);
    }
    fputs(text, f);
    fclose(f);
}

static void dir(const char *rel)
{
    char p[FB_PATH_MAX];
    path(p, rel);
    if (fb_mkdirs(p)) {
        printf("cannot make %s\n", p);
        exit(2);
    }
}

static int row(const struct file_browser *fb, const char *name)
{
    int i;
    for (i = 0; i < fb->n; i++)
        if (!strcmp(fb->entries[i].name, name))
            return i;
    return -1;
}

static const char *slurp(const char *rel)
{
    static char buf[256];
    char p[FB_PATH_MAX];
    size_t n;
    FILE *f;
    path(p, rel);
    f = fopen(p, "rb");
    if (!f)
        return NULL;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

static int count_one(void *ctx, const struct fb_entry *e)
{
    size_t l = strlen(e->name);
    if (l > 4 && !strcmp(e->name + l - 4, ".tmp"))
        ++*(int *)ctx;
    return 0;
}

/* the *.tmp files left in a folder of the temp tree */
static int count_tmp(const char *rel)
{
    char p[FB_PATH_MAX];
    int n = 0;
    path(p, rel);
    CHECK(fb_list_host(p, count_one, &n, NULL) == 0);
    return n;
}

static void make_temp(void)
{
#ifdef _WIN32
    char base[MAX_PATH];
    GetTempPathA(sizeof base, base);
    snprintf(T, sizeof T, "%sfile_browser_test_%lu", base, (unsigned long)GetCurrentProcessId());
    _mkdir(T);
#else
    char tmpl[FB_PATH_MAX];
    const char *b = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/file_browser_test.XXXXXX", b && *b ? b : "/tmp");
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(T, sizeof T, "%s", tmpl);
#endif
}

/* ---- names ---- */

static void test_names(void)
{
    char out[128], long64[FB_NAME_MAX + 1], long65[FB_NAME_MAX + 2];
    const char *why = "x";
    static const char *const refused[] = {
        "", ".", "..", "   ", "...", "a/b", "a\\b", "c:x", "a\tb", "a\nb", "a\x7f",
        "a*b", "a?b", "a\"b", "a<b", "a>b", "a|b", "CON", "con", "Con.txt", "CON .toml",
        "nul", "PRN", "aux.toml", "COM1", "com9.toml", "LPT1", "lpt9", "COM\xc2\xb9",
        "CONIN$", "conout$", "\xff", "a\xc0\xaf", "\xed\xa0\x80", "\xe2\x82",
    };
    unsigned i;

    CHECK(!fb_name_check("my config", ".toml", out, sizeof out, &why));
    CHECK(!strcmp(out, "my config.toml") && !why);
    CHECK(!fb_name_check("foo.toml", ".toml", out, sizeof out, &why) &&
          !strcmp(out, "foo.toml"));
    /* the trailing space goes before the extension comes off: not
     * "foo.toml.toml" */
    CHECK(!fb_name_check("foo.toml ", ".toml", out, sizeof out, &why) &&
          !strcmp(out, "foo.toml"));
    CHECK(!fb_name_check("foo..toml", ".toml", out, sizeof out, &why) &&
          !strcmp(out, "foo.toml"));
    CHECK(fb_name_check(".hidden", ".toml", out, sizeof out, &why) && why &&
          !strcmp(why, "A name cannot start with a dot"));
    CHECK(fb_name_check(".toml", ".toml", out, sizeof out, &why) && why);
    CHECK(!fb_name_check("foo.TOML", ".toml", out, sizeof out, &why) &&
          !strcmp(out, "foo.toml"));
    /* Windows drops a trailing dot or space */
    CHECK(!fb_name_check("a.", ".toml", out, sizeof out, &why) && !strcmp(out, "a.toml"));
    CHECK(!fb_name_check("a . ", ".toml", out, sizeof out, &why) && !strcmp(out, "a.toml"));
    CHECK(!fb_name_check("caf\xc3\xa9", ".toml", out, sizeof out, &why) &&
          !strcmp(out, "caf\xc3\xa9.toml"));
    /* not reserved: only the exact device names are */
    CHECK(!fb_name_check("COM10", ".toml", out, sizeof out, &why));
    CHECK(!fb_name_check("console", ".toml", out, sizeof out, &why));
    CHECK(!fb_name_check("aux2", ".toml", out, sizeof out, &why));
    CHECK(!fb_name_check("COM0", ".toml", out, sizeof out, &why));
    CHECK(!fb_name_check("a.b", ".toml", out, sizeof out, &why) && !strcmp(out, "a.b.toml"));
    CHECK(!fb_name_check("x", NULL, out, sizeof out, &why) && !strcmp(out, "x"));

    for (i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        why = NULL;
        out[0] = 'z';
        if (fb_name_check(refused[i], ".toml", out, sizeof out, &why) != -1 || !why || out[0])
            printf("  not refused: \"%s\"\n", refused[i]);
        CHECK(fb_name_check(refused[i], ".toml", out, sizeof out, &why) == -1 && why && !out[0]);
    }
    CHECK(fb_name_check(NULL, ".toml", out, sizeof out, &why) == -1);

    /* the reasons name the rule */
    fb_name_check("a/b", ".toml", out, sizeof out, &why);
    CHECK(why && strstr(why, "/"));
    fb_name_check("NUL", ".toml", out, sizeof out, &why);
    CHECK(why && strstr(why, "Reserved"));
    fb_name_check("..", ".toml", out, sizeof out, &why);
    CHECK(why && strstr(why, ".."));

    /* 64 bytes before the extension; a multi-byte character counts bytes */
    memset(long64, 'a', FB_NAME_MAX);
    long64[FB_NAME_MAX] = 0;
    memset(long65, 'a', FB_NAME_MAX + 1);
    long65[FB_NAME_MAX + 1] = 0;
    CHECK(!fb_name_check(long64, ".toml", out, sizeof out, &why) &&
          strlen(out) == FB_NAME_MAX + 5);
    CHECK(fb_name_check(long65, ".toml", out, sizeof out, &why) == -1 && strstr(why, "64"));
    /* a long name that ends in the extension is measured without it */
    {
        char l[FB_NAME_MAX + 8];
        snprintf(l, sizeof l, "%s.toml", long64);
        CHECK(!fb_name_check(l, ".toml", out, sizeof out, &why));
    }
    {
        char l[FB_NAME_MAX + 1];
        memset(l, 'a', FB_NAME_MAX - 1);
        memcpy(l + FB_NAME_MAX - 2, "\xc3\xa9", 3); /* 62 + 2 = 64 bytes */
        CHECK(!fb_name_check(l, ".toml", out, sizeof out, &why));
    }
    /* an out buffer too small is a refusal, never a cut name */
    CHECK(fb_name_check("abcdef", ".toml", out, 8, &why) == -1 && !out[0]);
}

static void test_default_name(void)
{
    struct tm tm;
    time_t t;
    char out[64];
    memset(&tm, 0, sizeof tm);
    tm.tm_year = 2026 - 1900;
    tm.tm_mon = 9;
    tm.tm_mday = 31;
    tm.tm_hour = 7;
    tm.tm_min = 5;
    tm.tm_isdst = -1;
    t = mktime(&tm);
    fb_default_name("config", t, ".toml", out, sizeof out);
    CHECK(!strcmp(out, "config-20261031-0705.toml"));
    fb_default_name("shader", t + 60, ".toml", out, sizeof out);
    CHECK(!strcmp(out, "shader-20261031-0706.toml"));
}

/* ---- the listing ---- */

static int s_fake_calls;
static int fake_denied(const char *d, fb_emit_fn emit, void *ctx, void *user)
{
    (void)d, (void)emit, (void)ctx, (void)user;
    s_fake_calls++;
    return EPERM; /* what macOS returns once TCC has said no */
}

static int fake_list(const char *d, fb_emit_fn emit, void *ctx, void *user)
{
    static const char *const names[] = {"b.toml", "B.toml", "a.TOML", ".", "..", "z", "Y"};
    struct fb_entry e;
    unsigned i;
    (void)d, (void)user;
    for (i = 0; i < sizeof names / sizeof names[0]; i++) {
        memset(&e, 0, sizeof e);
        snprintf(e.name, sizeof e.name, "%s", names[i]);
        e.is_dir = names[i][0] == 'z' || names[i][0] == 'Y' || names[i][0] == '.';
        e.hidden = 0;
        if (emit(ctx, &e))
            break;
    }
    return 0;
}

static void test_listing(void)
{
    struct file_browser fb;
    char p[FB_PATH_MAX];
    int r, i;

    dir("root/sub/deeper");
    dir("root/Zdir");
    dir("root/adir");
    dir("outside");
    put("root/b.toml", "b = 1\n");
    put("root/A.toml", "a = 1\n");
    put("root/c.txt", "c\n");
    put("root/.hidden.toml", "h = 1\n");
    put("root/sub/inner.toml", "i = 1\n");
    put("outside/secret.toml", "s = 1\n");

    fb_init(&fb);
    path(p, "root");
    r = fb_root_add(&fb, "Root", p, FB_ROOT_OTHER, 0);
    CHECK(r == 0 && fb.nroots == 1 && fb.root == -1);
    fb_set_filter(&fb, ".toml, .xsv");
    CHECK(fb.nexts == 2 && !strcmp(fb.exts[1], ".xsv"));

    CHECK(fb_open_root(&fb, 0) == FB_OK && fb.err == 0);
    CHECK(fb_at_root(&fb));
    /* folders first, case folded; the .txt and the dot file left out */
    CHECK(fb.n == 5);
    if (fb.n == 5) {
        CHECK(!strcmp(fb.entries[0].name, "adir") && fb.entries[0].is_dir);
        CHECK(!strcmp(fb.entries[1].name, "sub"));
        CHECK(!strcmp(fb.entries[2].name, "Zdir"));
        CHECK(!strcmp(fb.entries[3].name, "A.toml") && !fb.entries[3].is_dir);
        CHECK(!strcmp(fb.entries[4].name, "b.toml") && fb.entries[4].size == 6);
    }
    CHECK(row(&fb, "c.txt") < 0 && row(&fb, ".hidden.toml") < 0);

    fb.show_hidden = 1;
    fb_refresh(&fb);
    CHECK(row(&fb, ".hidden.toml") >= 0);
    fb.show_hidden = 0;
    fb_set_filter(&fb, NULL);
    fb_refresh(&fb);
    CHECK(row(&fb, "c.txt") >= 0 && fb.n == 6);
    fb_set_filter(&fb, ".toml");
    fb_refresh(&fb);

    /* a file is not a folder; an index past the rows is out of range */
    CHECK(fb_enter(&fb, row(&fb, "A.toml")) == FB_E_NOT_DIR);
    CHECK(fb_enter(&fb, 99) == FB_E_RANGE);

    /* down and up; ".." stops at the root */
    CHECK(fb_enter(&fb, row(&fb, "sub")) == FB_OK);
    CHECK(!fb_at_root(&fb) && fb.n == 2 && row(&fb, "inner.toml") == 1 && row(&fb, "deeper") == 0);
    CHECK(fb_entry_path(&fb, row(&fb, "inner.toml"), p, sizeof p) == FB_OK &&
          strstr(p, "inner.toml"));
    CHECK(fb_enter(&fb, row(&fb, "deeper")) == FB_OK && fb.n == 0);
    CHECK(fb_up(&fb) == FB_OK && row(&fb, "inner.toml") >= 0);
    CHECK(fb_up(&fb) == FB_OK && fb_at_root(&fb) && row(&fb, "sub") >= 0);
    CHECK(fb_up(&fb) == FB_E_ROOT && fb_at_root(&fb));
    CHECK(!strcmp(fb.cwd, fb.roots[0].real));

    /* a folder that cannot be listed: its errno, no rows, still inside it */
    fb.list = fake_denied;
    CHECK(fb_refresh(&fb) == EPERM && fb.err == EPERM && fb.n == 0 && s_fake_calls == 1);
    /* the callback's rows: "." and ".." dropped, sorted, filtered */
    fb.list = fake_list;
    CHECK(fb_refresh(&fb) == 0 && fb.n == 5);
    if (fb.n == 5) {
        CHECK(!strcmp(fb.entries[0].name, "Y") && !strcmp(fb.entries[1].name, "z"));
        CHECK(!strcmp(fb.entries[2].name, "a.TOML"));
        CHECK(!strcmp(fb.entries[3].name, "B.toml") && !strcmp(fb.entries[4].name, "b.toml"));
    }
    fb.list = NULL;

    /* a missing root lists as ENOENT and holds nothing */
    path(p, "nope");
    i = fb_root_add(&fb, "Gone", p, FB_ROOT_OTHER, 0);
    CHECK(fb_open_root(&fb, i) == ENOENT && fb.n == 0 && fb_at_root(&fb));
    CHECK(fb_up(&fb) == FB_E_ROOT);
    CHECK(fb_refresh(&fb) == ENOENT && fb.n == 0);
    /* made since: a refresh finds it */
    dir("nope");
    put("nope/late.toml", "l = 1\n");
    CHECK(fb_refresh(&fb) == FB_OK && fb.n == 1 && fb.roots[i].real[0] && fb_at_root(&fb));
    /* the exports root is made when opened */
    path(p, "user/exports/blinx2");
    i = fb_root_add(&fb, "Exports", p, FB_ROOT_EXPORTS, 1);
    CHECK(fb_open_root(&fb, i) == FB_OK && fb.n == 0 && fb_exists(p));
    CHECK(fb_open_root(&fb, 99) == FB_E_RANGE);
    fb_close_root(&fb);
    CHECK(fb.root == -1 && fb.n == 0);
    fb_free(&fb);
}

#ifndef _WIN32
static void test_links_and_denied(void)
{
    struct file_browser fb;
    char p[FB_PATH_MAX], q[FB_PATH_MAX];
    int i;

    path(p, "root/out_link");
    CHECK(!symlink("../outside", p));
    path(p, "root/in_link");
    CHECK(!symlink("sub", p));
    path(p, "root/zlink.toml");
    CHECK(!symlink("../outside/secret.toml", p));
    path(p, "root/inlink.toml");
    CHECK(!symlink("b.toml", p));
    path(p, "root/dangling.toml");
    CHECK(!symlink("does-not-exist.toml", p));
    /* sub/up_link climbs out through ".." */
    path(p, "root/sub/up_link");
    CHECK(!symlink("../../outside", p));
    dir("root/locked");
    path(p, "root/locked");
    CHECK(!chmod(p, 0));

    fb_init(&fb);
    fb_set_filter(&fb, ".toml");
    /* the root itself reached through a link: resolved once, and ".." still
     * stops at it */
    path(p, "rootlink");
    path(q, "root");
    CHECK(!symlink(q, p));
    fb_root_add(&fb, "Root", p, FB_ROOT_OTHER, 0);
    CHECK(fb_open_root(&fb, 0) == FB_OK);
    CHECK(strcmp(fb.roots[0].real, p) != 0 && fb_at_root(&fb));

    i = row(&fb, "out_link");
    CHECK(i >= 0 && fb.entries[i].is_dir && fb.entries[i].is_link && !fb.entries[i].link_ok);
    CHECK(fb_enter(&fb, i) == FB_E_ROOT && fb_at_root(&fb));
    i = row(&fb, "zlink.toml");
    CHECK(i >= 0 && fb.entries[i].is_link && !fb.entries[i].is_dir && !fb.entries[i].link_ok);
    i = row(&fb, "inlink.toml");
    CHECK(i >= 0 && fb.entries[i].is_link && fb.entries[i].link_ok);
    i = row(&fb, "dangling.toml");
    CHECK(i >= 0 && fb.entries[i].is_link && !fb.entries[i].link_ok);
    i = row(&fb, "A.toml");
    CHECK(i >= 0 && !fb.entries[i].is_link && fb.entries[i].link_ok);

    /* the import goes through the model: a link out of the root is refused
     * from its path now, whatever the listing said */
    {
        char *buf = NULL;
        size_t len = 0;
        i = row(&fb, "zlink.toml");
        fb.entries[i].link_ok = 1; /* a stale row cannot let it through */
        CHECK(fb_import_entry(&fb, i, FB_IMPORT_CAP, &buf, &len) == FB_E_ROOT && !buf);
        i = row(&fb, "inlink.toml");
        CHECK(fb_import_entry(&fb, i, FB_IMPORT_CAP, &buf, &len) == FB_OK && buf &&
              !strcmp(buf, slurp("root/b.toml")));
        free(buf);
        buf = NULL;
        i = row(&fb, "A.toml");
        CHECK(fb_import_entry(&fb, i, FB_IMPORT_CAP, &buf, &len) == FB_OK && buf);
        free(buf);
        buf = NULL;
        i = row(&fb, "dangling.toml");
        CHECK(fb_import_entry(&fb, i, FB_IMPORT_CAP, &buf, &len) == FB_E_IO && !buf);
        i = row(&fb, "sub");
        CHECK(fb_import_entry(&fb, i, FB_IMPORT_CAP, &buf, &len) == FB_E_NOT_DIR && !buf);
        CHECK(fb_import_entry(&fb, fb.n, FB_IMPORT_CAP, &buf, &len) == FB_E_RANGE);
    }

    /* a link inside the root opens, at its resolved path */
    i = row(&fb, "in_link");
    CHECK(i >= 0 && fb.entries[i].is_link && fb.entries[i].link_ok);
    CHECK(fb_enter(&fb, i) == FB_OK && row(&fb, "inner.toml") >= 0);
    path(q, "root/sub");
    {
        char rq[FB_PATH_MAX];
        CHECK(realpath(q, rq) && !strcmp(fb.cwd, rq));
    }
    i = row(&fb, "up_link");
    CHECK(i >= 0 && !fb.entries[i].link_ok && fb_enter(&fb, i) == FB_E_ROOT);
    CHECK(fb_up(&fb) == FB_OK && fb_at_root(&fb) && fb_up(&fb) == FB_E_ROOT);

    /* the unreadable folder: listed as a row, entered with EACCES and no
     * rows (root ignores the mode, so the check is skipped there) */
    i = row(&fb, "locked");
    CHECK(i >= 0);
    if (geteuid() != 0) {
        int e = fb_enter(&fb, i);
        CHECK((e == EACCES || e == EPERM) && fb.err == e && fb.n == 0 && !fb_at_root(&fb));
        CHECK(fb_up(&fb) == FB_OK && fb_at_root(&fb) && fb.n > 0);
    } else {
        printf("  running as root: the unreadable folder is not checked\n");
    }
    path(p, "root/locked");
    chmod(p, 0755);
    fb_free(&fb);
}
#endif

/* ---- files ---- */

static void test_files(void)
{
    char p[FB_PATH_MAX], q[FB_PATH_MAX], name[64];
    char *big, *buf = NULL;
    size_t len = 0;
    FILE *f;
    struct fb_export ex;

    /* the size cap: refused from the size, before a byte is read */
    big = malloc(FB_IMPORT_CAP + 1);
    memset(big, 'x', FB_IMPORT_CAP + 1);
    path(p, "cap_ok.toml");
    f = fopen(p, "wb");
    fwrite(big, 1, FB_IMPORT_CAP, f);
    fclose(f);
    path(q, "cap_big.toml");
    f = fopen(q, "wb");
    fwrite(big, 1, FB_IMPORT_CAP + 1, f);
    fclose(f);
    free(big);
    CHECK(fb_read_file(p, FB_IMPORT_CAP, &buf, &len) == FB_OK && len == FB_IMPORT_CAP &&
          buf && buf[len] == 0);
    free(buf);
    buf = NULL;
    CHECK(fb_read_file(q, FB_IMPORT_CAP, &buf, &len) == FB_E_TOO_BIG && !buf);
    path(p, "root");
    CHECK(fb_read_file(p, FB_IMPORT_CAP, &buf, &len) == FB_E_IO && !buf);
    path(p, "missing.toml");
    CHECK(fb_read_file(p, FB_IMPORT_CAP, &buf, &len) == FB_E_IO && !buf);

    /* temp+rename: the new text, no .tmp left */
    dir("w");
    put("w/target.toml", "old\n");
    path(p, "w/target.toml");
    CHECK(fb_write_atomic(p, "new\n", 4) == FB_OK);
    CHECK(slurp("w/target.toml") && !strcmp(slurp("w/target.toml"), "new\n"));
    path(q, "w/target.toml.tmp");
    CHECK(!fb_exists(q));
    /* a missing folder fails, leaving nothing */
    path(p, "w/no/such/x.toml");
    CHECK(fb_write_atomic(p, "x", 1) == FB_E_IO);
    path(q, "w/no/such/x.toml.tmp");
    CHECK(!fb_exists(p) && !fb_exists(q));
    /* a rename that fails (the target is a folder): the tmp is removed and
     * the folder untouched */
    dir("w/isdir.toml");
    path(p, "w/isdir.toml");
    CHECK(fb_write_atomic(p, "x", 1) == FB_E_IO);
    CHECK(!count_tmp("w") && fb_exists(p));

    /* write-only-if-absent: a taken name is left as it was */
    path(p, "w/target.toml");
    CHECK(fb_write_new(p, "newer\n", 6) == FB_E_EXISTS);
    CHECK(!strcmp(slurp("w/target.toml"), "new\n") && !count_tmp("w"));
    path(p, "w/brand-new.toml");
    CHECK(fb_write_new(p, "b\n", 2) == FB_OK && !strcmp(slurp("w/brand-new.toml"), "b\n"));
    CHECK(!count_tmp("w"));

    /* an import's write picks the name and writes in one step */
    path(p, "w");
    put("w/imp.toml", "1\n");
    put("w/imp-2.toml", "2\n");
    CHECK(fb_import_unique(p, "imp", ".toml", "3\n", 2, name, sizeof name) == FB_OK &&
          !strcmp(name, "imp-3.toml") && !strcmp(slurp("w/imp-3.toml"), "3\n"));
    CHECK(!strcmp(slurp("w/imp.toml"), "1\n") && !strcmp(slurp("w/imp-2.toml"), "2\n"));
    CHECK(fb_import_unique(p, "../up", ".toml", "x", 1, name, sizeof name) == FB_E_NAME);
    CHECK(fb_import_unique(p, "CON", ".toml", "x", 1, name, sizeof name) == FB_E_NAME);
    CHECK(fb_unique_name(p, "a/b", ".toml", name, sizeof name) == FB_E_NAME);
    CHECK(fb_unique_name(p, ".dot", ".toml", name, sizeof name) == FB_E_NAME);
    CHECK(!count_tmp("w"));

#ifndef _WIN32
    /* the tmp name is never written through: a file and a link already at
     * this pid's first tmp names stay as they were */
    {
        char t0[FB_PATH_MAX], t1[FB_PATH_MAX], rel[96];
        put("victim.txt", "victim\n");
        snprintf(rel, sizeof rel, "w/target.toml.%ld-0.tmp", (long)getpid());
        put(rel, "squatter\n");
        path(t0, rel);
        snprintf(rel, sizeof rel, "w/target.toml.%ld-1.tmp", (long)getpid());
        path(t1, rel);
        path(q, "victim.txt");
        CHECK(!symlink(q, t1));
        path(p, "w/target.toml");
        CHECK(fb_write_atomic(p, "third\n", 6) == FB_OK);
        CHECK(!strcmp(slurp("w/target.toml"), "third\n"));
        CHECK(!strcmp(slurp("victim.txt"), "victim\n"));
        snprintf(rel, sizeof rel, "w/target.toml.%ld-0.tmp", (long)getpid());
        CHECK(slurp(rel) && !strcmp(slurp(rel), "squatter\n"));
        {
            struct stat st;
            CHECK(!lstat(t1, &st) && S_ISLNK(st.st_mode));
        }
        CHECK(count_tmp("w") == 2);
        unlink(t0);
        unlink(t1);
    }

    /* a FIFO is refused at once rather than blocking the read */
    path(p, "pipe.toml");
    CHECK(!mkfifo(p, 0644));
    alarm(5); /* a hang fails the run instead of stalling it */
    errno = 0;
    CHECK(fb_read_file(p, FB_IMPORT_CAP, &buf, &len) == FB_E_IO && !buf && errno == EINVAL);
    alarm(0);
    unlink(p);
#else
    /* the tmp name is never written through: a file already at this pid's
     * first tmp name stays as it was */
    {
        char rel[96];
        snprintf(rel, sizeof rel, "w/target.toml.%lu-0.tmp",
                 (unsigned long)GetCurrentProcessId());
        put(rel, "squatter\n");
        path(p, "w/target.toml");
        CHECK(fb_write_atomic(p, "third\n", 6) == FB_OK);
        CHECK(!strcmp(slurp("w/target.toml"), "third\n"));
        CHECK(slurp(rel) && !strcmp(slurp(rel), "squatter\n"));
        CHECK(count_tmp("w") == 1);
        path(q, rel);
        DeleteFileA(q);
    }
#endif

    /* the export: a free name, then a taken one asks with No focused */
    path(p, "w");
    CHECK(fb_export_begin(&ex, p, "my export", ".toml") == FB_EXP_READY);
    CHECK(fb_export_write(&ex, "one\n", 4) == FB_EXP_DONE);
    CHECK(slurp("w/my export.toml") && !strcmp(slurp("w/my export.toml"), "one\n"));
    CHECK(fb_export_begin(&ex, p, "my export.toml", ".toml") == FB_EXP_CONFIRM);
    CHECK(ex.focus_yes == 0 && !strcmp(ex.prompt, "Replace my export.toml?"));
    CHECK(fb_export_write(&ex, "two\n", 4) == FB_EXP_CONFIRM); /* not before an answer */
    fb_export_answer(&ex, 0);
    CHECK(ex.state == FB_EXP_CANCELLED && fb_export_write(&ex, "two\n", 4) == FB_EXP_CANCELLED);
    CHECK(!strcmp(slurp("w/my export.toml"), "one\n"));
    fb_export_begin(&ex, p, "my export", ".toml");
    fb_export_answer(&ex, 1);
    CHECK(ex.state == FB_EXP_READY && fb_export_write(&ex, "two\n", 4) == FB_EXP_DONE);
    CHECK(!strcmp(slurp("w/my export.toml"), "two\n"));
    fb_export_answer(&ex, 0); /* no confirm open: nothing changes */
    CHECK(ex.state == FB_EXP_DONE);
    CHECK(fb_export_begin(&ex, p, "CON", ".toml") == FB_EXP_REFUSED && ex.reason);
    CHECK(fb_export_begin(&ex, p, "../escape", ".toml") == FB_EXP_REFUSED && ex.reason);
    /* a folder of that name is refused up front, not offered for replacing */
    CHECK(fb_export_begin(&ex, p, "isdir", ".toml") == FB_EXP_REFUSED && ex.reason &&
          !strcmp(ex.reason, "A folder has that name"));
    path(q, "w/no/such");
    CHECK(fb_export_begin(&ex, q, "x", ".toml") == FB_EXP_READY &&
          fb_export_write(&ex, "x", 1) == FB_EXP_FAILED && ex.reason);

    /* unique import slugs: an import never overwrites */
    put("w/profile.toml", "p\n");
    CHECK(fb_unique_name(p, "profile", ".toml", name, sizeof name) == FB_OK &&
          !strcmp(name, "profile-2.toml"));
    put("w/profile-2.toml", "p\n");
    CHECK(fb_unique_name(p, "profile", ".toml", name, sizeof name) == FB_OK &&
          !strcmp(name, "profile-3.toml"));
    CHECK(fb_unique_name(p, "fresh", ".toml", name, sizeof name) == FB_OK &&
          !strcmp(name, "fresh.toml"));
    CHECK(fb_unique_name(p, "fresh", ".toml", name, 4) == FB_E_RANGE);

    /* joins: one separator, none doubled */
    CHECK(fb_path_join("/a", "b", name, sizeof name) == FB_OK);
#ifdef _WIN32
    CHECK(!strcmp(name, "/a\\b"));
    CHECK(fb_path_join("C:\\", "b", name, sizeof name) == FB_OK && !strcmp(name, "C:\\b"));
#else
    CHECK(!strcmp(name, "/a/b"));
    CHECK(fb_path_join("/", "b", name, sizeof name) == FB_OK && !strcmp(name, "/b"));
#endif
    CHECK(fb_path_join("/a", "b", name, 4) == FB_E_RANGE);
}

/* ---- homes and the default roots ---- */

static int fake_dos(const char *u, char *out, size_t n)
{
    if (strcmp(u, "/home/deck"))
        return 0;
    snprintf(out, n, "Z:\\home\\deck");
    return 1;
}

static int no_dos(const char *u, char *out, size_t n)
{
    (void)u, (void)out, (void)n;
    return 0;
}

static void test_homes(void)
{
    char out[FB_PATH_MAX], home[FB_PATH_MAX], user[FB_PATH_MAX];
    struct file_browser fb;
    int i;

    /* under Wine the Unix home, through Z: */
    CHECK(fb_home_pick(1, "/home/deck", "C:\\users\\steamuser", fake_dos, out, sizeof out) &&
          !strcmp(out, "Z:\\home\\deck"));
    /* without the export, Z: is still the Unix root */
    CHECK(fb_home_pick(1, "/home/deck", "C:\\users\\steamuser", no_dos, out, sizeof out) &&
          !strcmp(out, "Z:\\home\\deck"));
    CHECK(fb_home_pick(1, "/home/deck", NULL, NULL, out, sizeof out) &&
          !strcmp(out, "Z:\\home\\deck"));
    /* Wine with no Unix HOME: the Wine profile */
    CHECK(fb_home_pick(1, NULL, "C:\\users\\steamuser", fake_dos, out, sizeof out) &&
          !strcmp(out, "C:\\users\\steamuser"));
    /* Windows: %USERPROFILE%, even with a HOME set (MSYS sets one) */
    CHECK(fb_home_pick(0, "/c/Users/player", "C:\\Users\\Pl\xc3\xa4yer", fake_dos, out,
                       sizeof out) &&
          !strcmp(out, "C:\\Users\\Pl\xc3\xa4yer"));
    /* POSIX */
    CHECK(fb_home_pick(0, "/Users/x", NULL, NULL, out, sizeof out) && !strcmp(out, "/Users/x"));
    CHECK(!fb_home_pick(0, NULL, NULL, NULL, out, sizeof out));
    CHECK(!fb_home_pick(1, "/home/deck", NULL, NULL, out, 4));
    CHECK(!fb_under_wine());

    /* the default roots, from a temp user dir and a temp HOME */
    path(home, "home");
    path(user, "user");
    dir("home");
#ifdef _WIN32
    SetEnvironmentVariableA("USERPROFILE", home);
#else
    setenv("HOME", home, 1);
#endif
    fb_init(&fb);
    CHECK(fb_add_default_roots(&fb, user, "blinx2") >= 4);
    CHECK(!strcmp(fb.roots[0].label, "Exports") && fb.roots[0].kind == FB_ROOT_EXPORTS &&
          fb.roots[0].create && strstr(fb.roots[0].path, "exports") &&
          strstr(fb.roots[0].path, "blinx2"));
    CHECK(!strcmp(fb.roots[1].label, "Config") && !strcmp(fb.roots[1].path, user));
    CHECK(fb.roots[2].kind == FB_ROOT_SHADERS && strstr(fb.roots[2].path, "shader-profiles"));
    CHECK(fb.roots[3].kind == FB_ROOT_HOME && !strcmp(fb.roots[3].path, home));
#ifndef _WIN32
    CHECK(fb.nroots == 4);
#else
    for (i = 4; i < fb.nroots; i++)
        CHECK(fb.roots[i].kind == FB_ROOT_DRIVE && fb.roots[i].path[1] == ':');
#endif
    fb_free(&fb);
    fb_init(&fb);
    /* no user config (RECOMP_CONFIG=none): Home (and drives) only */
    fb_add_default_roots(&fb, NULL, NULL);
    CHECK(fb.nroots >= 1 && fb.roots[0].kind == FB_ROOT_HOME);
    fb_free(&fb);
    fb_init(&fb);
    fb_add_default_roots(&fb, user, NULL);
    path(out, "user/exports");
    CHECK(!strcmp(fb.roots[0].path, out));
    for (i = 0; i < fb.nroots; i++)
        CHECK(fb.roots[i].real[0] == 0); /* nothing resolved or made before it opens */
    fb_free(&fb);
}

int main(void)
{
    make_temp();
    printf("temp folder %s\n", T);
    test_names();
    test_default_name();
    test_listing();
#ifndef _WIN32
    test_links_and_denied();
#endif
    test_files();
    test_homes();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    if (!s_failed) {
        /* leave the folder for a look when something failed */
#ifdef _WIN32
        char cmd[FB_PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", T);
        system(cmd);
#else
        char cmd[FB_PATH_MAX + 32];
        snprintf(cmd, sizeof cmd, "chmod -R u+rwx '%s' && rm -rf '%s'", T, T);
        if (system(cmd) != 0)
            printf("could not remove %s\n", T);
#endif
    }
    return s_failed ? 1 : 0;
}
