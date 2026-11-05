/* The saves zip and its store (saves_zip.h, saves_store.h; openspec change
 * overlay-saves D2, D3, D5, D6). The zips are built here, good ones with
 * the writer and crafted ones with raw_zip below, which writes whatever it
 * is told. The store runs on a fresh temp folder with made-up saves. */
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
#include <unistd.h>
#endif

#include "file_browser.h"
#include "saves_store.h"
#include "saves_zip.h"

static int s_checks, s_failed;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        s_checks++;                                                                                \
        if (!(x)) {                                                                                \
            s_failed++;                                                                            \
            printf("FAIL line %d: %s\n", __LINE__, #x);                                            \
        }                                                                                          \
    } while (0)

#define TID 0x4d530065u
static const char *const MAN_OK =
    "format = 1\nkind = \"saves\"\ntitle_id = \"4d530065\"\ntitle = \"T\"\n"
    "keys = \"zero\"\nfiles = 1\nbytes = 3\n";

/* ---- a raw zip writer: no checks, so a test can make any zip ---- */

struct raw {
    const char *name;
    const void *data;
    uint32_t len;
    uint16_t method;
    uint16_t flags;
    uint32_t mode;      /* unix mode for the external attributes; 0: 0100644 */
    int bad_crc;
};

static void w16(unsigned char *p, uint32_t v) { p[0] = (unsigned char)v; p[1] = (unsigned char)(v >> 8); }
static void w32(unsigned char *p, uint32_t v) { w16(p, v & 0xFFFF); w16(p + 2, v >> 16); }

static unsigned char *raw_zip(const struct raw *e, int n, size_t *len)
{
    unsigned char *b = calloc(1, 1 << 20), *cd = calloc(1, 1 << 16);
    size_t at = 0, cl = 0;
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(e[i].name);
        uint32_t crc = sz_crc32(0, e[i].data, e[i].len) ^ (e[i].bad_crc ? 1u : 0u);
        unsigned char *h = b + at, *c = cd + cl;
        w32(h, 0x04034b50); w16(h + 4, 20); w16(h + 6, e[i].flags); w16(h + 8, e[i].method);
        w32(h + 14, crc); w32(h + 18, e[i].len); w32(h + 22, e[i].len);
        w16(h + 26, (uint32_t)nl); w16(h + 28, 0);
        memcpy(h + 30, e[i].name, nl);
        if (e[i].len) memcpy(h + 30 + nl, e[i].data, e[i].len);
        w32(c, 0x02014b50); w16(c + 4, 3 << 8 | 20); w16(c + 6, 20); w16(c + 8, e[i].flags);
        w16(c + 10, e[i].method); w32(c + 16, crc); w32(c + 20, e[i].len); w32(c + 24, e[i].len);
        w16(c + 28, (uint32_t)nl);
        w32(c + 38, (e[i].mode ? e[i].mode : 0100644u) << 16);
        w32(c + 42, (uint32_t)at);
        memcpy(c + 46, e[i].name, nl);
        at += 30 + nl + e[i].len;
        cl += 46 + nl;
    }
    memcpy(b + at, cd, cl);
    w32(b + at + cl, 0x06054b50);
    w16(b + at + cl + 8, (uint32_t)n); w16(b + at + cl + 10, (uint32_t)n);
    w32(b + at + cl + 12, (uint32_t)cl); w32(b + at + cl + 16, (uint32_t)at);
    *len = at + cl + 22;
    free(cd);
    return b;
}

/* sz_read then saves_zip_check; 0 when both pass, with the error kept */
static char s_err[SZ_ERR_MAX];
static int check_zip(const unsigned char *z, size_t len)
{
    struct sz_archive a;
    struct saves_manifest m;
    int trees, r;
    s_err[0] = 0;
    if (sz_read(z, len, &a, s_err, sizeof s_err))
        return -1;
    r = saves_zip_check(&a, TID, "zero", &m, &trees, s_err, sizeof s_err);
    sz_archive_free(&a);
    return r;
}

/* a manifest plus one more entry */
static int one(const char *name, const char *data, uint16_t method, uint32_t mode, int bad_crc)
{
    struct raw e[2] = {
        { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 },
        { name, data, (uint32_t)strlen(data), method, 0, mode, bad_crc },
    };
    size_t len;
    unsigned char *z = raw_zip(e, 2, &len);
    int r = check_zip(z, len);
    free(z);
    return r;
}

static int has(const char *needle) { return strstr(s_err, needle) != NULL; }

/* ---- names ---- */

static void test_names(void)
{
    const char *why;
    CHECK(sz_name_ok("hdd/UDATA/4d530065/ABC/SaveMeta.xbx", &why) == 0);
    CHECK(sz_name_ok("hdd/UDATA/", &why) == 0);
    CHECK(sz_name_ok("hdd/../x", &why) && why);
    CHECK(sz_name_ok("hdd/./x", &why));
    CHECK(sz_name_ok("/etc/passwd", &why));
    CHECK(sz_name_ok("C:/x", &why));
    CHECK(sz_name_ok("a\\b", &why));
    CHECK(sz_name_ok("a//b", &why));
    CHECK(sz_name_ok("a/CON", &why));
    CHECK(sz_name_ok("a/nul.txt", &why));
    CHECK(sz_name_ok("a/COM1", &why));
    CHECK(sz_name_ok("a/com", &why) == 0);
    CHECK(sz_name_ok("a/x.", &why));
    CHECK(sz_name_ok("a/x ", &why));
    CHECK(sz_name_ok("a/x*y", &why));
    CHECK(sz_name_ok("a/\x01", &why));
    CHECK(sz_name_ok("a/\xC0\xAF", &why));   /* an overlong '/' */
    CHECK(sz_name_ok("a/caf\xC3\xA9", &why) == 0);
    CHECK(sz_name_ok("a/123456789012345678901234567890123456789012", &why) == 0);
    CHECK(sz_name_ok("a/1234567890123456789012345678901234567890123", &why));
    CHECK(sz_name_ok("1/2/3/4/5/6/7/8", &why) == 0);
    CHECK(sz_name_ok("1/2/3/4/5/6/7/8/9", &why));
}

/* ---- the writer read back ---- */

/* A fixed vector: every other check here round-trips through sz_crc32 */
static void test_crc(void)
{
    CHECK(sz_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(sz_crc32(sz_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u);
    CHECK(sz_crc32(0, "", 0) == 0);
}

static void test_round_trip(void)
{
    struct sz_writer w;
    struct sz_archive a;
    struct saves_manifest m = { 0 }, back;
    unsigned char *z;
    size_t len;
    char man[1024];
    const char *why;
    int trees, ml;
    const struct sz_entry *e;

    m.format = 1;
    snprintf(m.kind, sizeof m.kind, "saves");
    m.title_id = TID;
    snprintf(m.title, sizeof m.title, "BLiNX \"2\" \\ test");
    snprintf(m.created, sizeof m.created, "2026-11-03T12:00:00Z");
    snprintf(m.keys, sizeof m.keys, "zero");
    m.files = 2;
    m.bytes = 8;
    ml = saves_manifest_write(&m, man, sizeof man);
    CHECK(ml > 0);
    CHECK(saves_manifest_parse(man, (size_t)ml, &back, &why) == 0);
    CHECK(back.title_id == TID && !strcmp(back.title, m.title) && back.files == 2
          && back.bytes == 8 && !strcmp(back.keys, "zero"));

    sz_writer_init(&w);
    CHECK(sz_add_file(&w, SZ_MANIFEST, man, (size_t)ml, 1762171200) == 0);
    CHECK(sz_add_dir(&w, "hdd/", 0) == 0);
    CHECK(sz_add_dir(&w, "hdd/UDATA/4d530065/", 1762171200) == 0);
    CHECK(sz_add_file(&w, "hdd/UDATA/4d530065/AB/data.bin", "abcd", 4, 1762171201) == 0);
    CHECK(sz_add_file(&w, "hdd/TDATA/4d530065/t.bin", "efgh", 4, 1762171202) == 0);
    CHECK(sz_finish(&w, &z, &len) == 0);
    CHECK(sz_read(z, len, &a, s_err, sizeof s_err) == 0);
    CHECK(a.n == 5);
    CHECK((e = sz_find(&a, "hdd/UDATA/4d530065/AB/data.bin")) && e->size == 4
          && !memcmp(e->data, "abcd", 4) && e->mtime == 1762171201 && !e->is_dir);
    CHECK((e = sz_find(&a, "hdd/UDATA/4d530065/")) && e->is_dir);
    CHECK(saves_zip_check(&a, TID, "zero", &back, &trees, s_err, sizeof s_err) == 0);
    CHECK(trees == (SAVES_TREE_UDATA | SAVES_TREE_TDATA));
    CHECK(saves_zip_check(&a, 0x4d530004u, "zero", &back, &trees, s_err, sizeof s_err) && has("another game"));
    CHECK(saves_zip_check(&a, TID, "0123abcd", &back, &trees, s_err, sizeof s_err) && has("console keys"));
    sz_archive_free(&a);

    /* a damaged copy: one data byte flipped */
    {
        unsigned char *bad = malloc(len);
        memcpy(bad, z, len);
        for (size_t i = 0; i + 4 <= len; i++)
            if (!memcmp(bad + i, "abcd", 4)) { bad[i] ^= 1; break; }
        CHECK(sz_read(bad, len, &a, s_err, sizeof s_err) && has("checksum"));
        /* cut short */
        CHECK(sz_read(z, len - 1, &a, s_err, sizeof s_err));
        CHECK(sz_read(z, 10, &a, s_err, sizeof s_err) && has("not a zip"));
        free(bad);
    }
    free(z);
    /* the writer refuses a name the reader would */
    sz_writer_init(&w);
    CHECK(sz_add_file(&w, "../x", "a", 1, 0) == -1);
    z = NULL;
    CHECK(sz_finish(&w, &z, &len) == -1 && !z);
}

/* ---- each refusal ---- */

static void test_refusals(void)
{
    size_t len;
    unsigned char *z;

    CHECK(one("hdd/UDATA/4d530065/A/f", "abc", 0, 0, 0) == 0);
    CHECK(one("hdd/UDATA/4d530065/../../x", "abc", 0, 0, 0) && has("'..'"));
    CHECK(one("/hdd/UDATA/4d530065/f", "abc", 0, 0, 0) && has("absolute"));
    CHECK(one("hdd\\UDATA\\4d530065\\f", "abc", 0, 0, 0) && has("backslash"));
    CHECK(one("C:/hdd/f", "abc", 0, 0, 0) && has("drive"));
    CHECK(one("hdd/UDATA/4d530065/AUX", "abc", 0, 0, 0) && has("device"));
    CHECK(one("hdd/UDATA/4d530065/A/f", "abc", 8, 0, 0) && has("compressed"));
    CHECK(one("hdd/UDATA/4d530065/A/f", "abc", 0, 0120777u, 0) && has("link"));
    CHECK(one("hdd/UDATA/4d530065/A/f", "abc", 0, 0020644u, 0) && has("special"));
    CHECK(one("hdd/UDATA/4d530065/A/f", "abc", 0, 0, 1) && has("checksum"));
    CHECK(one("hdd/UDATA/4d530004/A/f", "abc", 0, 0, 0) && has("outside"));
    CHECK(one("hdd/UserData/f", "abc", 0, 0, 0) && has("outside"));
    CHECK(one("evil.exe", "abc", 0, 0, 0) && has("outside"));
    CHECK(one("hdd/UDATA/4d530065/A/f", "abcd", 0, 0, 0) && has("incomplete"));
    CHECK(one("hdd/UDATA/4D530065/A/f", "abc", 0, 0, 0) == 0);  /* the tid's case folds */

    /* encrypted */
    {
        struct raw e[2] = {
            { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 },
            { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 1, 0, 0 },
        };
        z = raw_zip(e, 2, &len);
        CHECK(check_zip(z, len) && has("encrypted"));
        free(z);
    }
    /* case duplicates, and a file and a folder of one name */
    {
        const char *m2 = "format = 1\nkind = \"saves\"\ntitle_id = \"4d530065\"\n"
                         "keys = \"zero\"\nfiles = 2\nbytes = 6\n";
        struct raw e[3] = {
            { SZ_MANIFEST, m2, (uint32_t)strlen(m2), 0, 0, 0, 0 },
            { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 0, 0, 0 },
            { "hdd/UDATA/4d530065/a/F", "abc", 3, 0, 0, 0, 0 },
        };
        z = raw_zip(e, 3, &len);
        CHECK(check_zip(z, len) && has("same name"));
        free(z);
        e[2].name = "hdd/UDATA/4d530065/A/f/";
        e[2].data = "";
        e[2].len = 0;
        z = raw_zip(e, 3, &len);
        CHECK(check_zip(z, len) && has("same name"));
        free(z);
    }
    /* no manifest; a manifest of another kind, a newer format, a bad one */
    {
        struct raw e[1] = { { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 0, 0, 0 } };
        z = raw_zip(e, 1, &len);
        CHECK(check_zip(z, len) && has("xbr-saves.toml"));
        free(z);
    }
    {
        static const char *const bad[] = {
            "format = 1\nkind = \"profile\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\nfiles = 1\nbytes = 3\n",
            "format = 2\nkind = \"saves\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\nfiles = 1\nbytes = 3\n",
            "format = 1\nkind = \"saves\"\ntitle_id = \"0x530065\"\nkeys = \"zero\"\nfiles = 1\nbytes = 3\n",
            "format = 1\nkind = \"saves\"\nkeys = \"zero\"\nfiles = 1\nbytes = 3\n",
            "format = \"1\"\nkind = \"saves\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\nfiles = 1\nbytes = 3\n",
            "this is not toml\n",
        };
        static const char *const want[] = { "not a saves", "newer", "title_id", "missing",
                                             "wrong type", "key = value" };
        for (int i = 0; i < 6; i++) {
            struct raw e[2] = {
                { SZ_MANIFEST, bad[i], (uint32_t)strlen(bad[i]), 0, 0, 0, 0 },
                { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 0, 0, 0 },
            };
            z = raw_zip(e, 2, &len);
            CHECK(check_zip(z, len) && has(want[i]));
            if (!has(want[i])) printf("  manifest %d: %s\n", i, s_err);
            free(z);
        }
    }
    /* two names on one blob */
    {
        struct raw e[2] = {
            { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 },
            { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 0, 0, 0 },
        };
        unsigned char *p;
        z = raw_zip(e, 2, &len);
        /* point the second index entry at the first local header: the names
         * no longer agree, so it is refused as damaged */
        p = z + len - 22;
        p = z + (p[16] | p[17] << 8);
        p += 46 + strlen(SZ_MANIFEST);
        w32(p + 42, 0);
        CHECK(check_zip(z, len) && has("damaged"));
        free(z);
    }
    /* over the caps */
    {
        unsigned char *big = calloc(1, SZ_MAX_ZIP + 1);
        struct sz_archive a;
        CHECK(sz_read(big, SZ_MAX_ZIP + 1, &a, s_err, sizeof s_err) && has("64 MiB"));
        free(big);
    }
    /* the index's count over SZ_MAX_ENTRIES */
    {
        struct raw e[1] = { { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 } };
        struct sz_archive a;
        z = raw_zip(e, 1, &len);
        w16(z + len - 22 + 8, SZ_MAX_ENTRIES + 1);
        w16(z + len - 22 + 10, SZ_MAX_ENTRIES + 1);
        CHECK(sz_read(z, len, &a, s_err, sizeof s_err) && has("too many"));
        free(z);
    }
}

/* ---- SaveMeta.xbx ---- */

static size_t utf16(const char *ascii, unsigned char *out)
{
    size_t n = 0;
    out[n++] = 0xFF;
    out[n++] = 0xFE;
    for (; *ascii; ascii++) {
        out[n++] = (unsigned char)*ascii;
        out[n++] = 0;
    }
    return n;
}

static void test_meta(void)
{
    unsigned char b[256];
    char name[64];
    size_t n = utf16("Name=BLiNX Save 1\r\nNoCopy=0\r\n", b);
    CHECK(saves_meta_name(b, n, name, sizeof name) == 0 && !strcmp(name, "BLiNX Save 1"));
    n = utf16("Other=1\r\nName=Two\r\n", b);
    CHECK(saves_meta_name(b, n, name, sizeof name) == 0 && !strcmp(name, "Two"));
    n = utf16("NoName=1\r\n", b);
    CHECK(saves_meta_name(b, n, name, sizeof name) == -1);
}

/* ---- the store on a temp folder ---- */

static char T[FB_PATH_MAX];

static void make_temp(void)
{
#ifdef _WIN32
    char base[MAX_PATH];
    GetTempPathA(sizeof base, base);
    snprintf(T, sizeof T, "%ssaves_zip_test_%lu", base, (unsigned long)GetCurrentProcessId());
    _mkdir(T);
#else
    char tmpl[FB_PATH_MAX];
    const char *b = getenv("TMPDIR");
    snprintf(tmpl, sizeof tmpl, "%s/saves_zip_test.XXXXXX", b && *b ? b : "/tmp");
    if (!mkdtemp(tmpl)) {
        perror("mkdtemp");
        exit(2);
    }
    snprintf(T, sizeof T, "%s", tmpl);
#endif
}

static void path(char *out, const char *rel)
{
    snprintf(out, FB_PATH_MAX, "%s/%s", T, rel);
#ifdef _WIN32
    for (; *out; out++)
        if (*out == '/') *out = '\\';
#endif
}

static void put(const char *rel, const void *data, size_t len)
{
    char p[FB_PATH_MAX], d[FB_PATH_MAX];
    FILE *f;
    char *s;
    path(p, rel);
    snprintf(d, sizeof d, "%s", p);
    if ((s = strrchr(d, '/')) || (s = strrchr(d, '\\'))) {
        *s = 0;
        fb_mkdirs(d);
    }
    if (!(f = fopen(p, "wb"))) {
        printf("cannot write %s\n", p);
        exit(2);
    }
    fwrite(data, 1, len, f);
    fclose(f);
}

static int same(const char *rel, const char *text)
{
    char p[FB_PATH_MAX], *buf;
    size_t len;
    int r;
    path(p, rel);
    if (fb_read_file(p, 1 << 20, &buf, &len) != FB_OK)
        return 0;
    r = len == strlen(text) && !memcmp(buf, text, len);
    free(buf);
    return r;
}

static int there(const char *rel)
{
    char p[FB_PATH_MAX];
    path(p, rel);
    return fb_exists(p);
}

static int busy_yes(void *ctx) { (void)ctx; return 1; }

/* the folders beside the roots: a stage folder is never left behind */
static int stage_left(const char *rel_parent)
{
    char p[FB_PATH_MAX];
    struct file_browser fb;
    int left = 0;
    path(p, rel_parent);
    fb_init(&fb);
    fb.show_hidden = 1;
    fb_root_add(&fb, "t", p, FB_ROOT_OTHER, 0);
    if (fb_open_root(&fb, 0) == FB_OK)
        for (int i = 0; i < fb.n; i++)
            left |= !strncmp(fb.entries[i].name, ".saves-import", 13);
    fb_free(&fb);
    return left;
}

static void test_store(void)
{
    struct saves_paths a, b;
    struct saves_info info;
    struct saves_manifest base = { 0 };
    struct sz_archive arch;
    struct saves_manifest m;
    unsigned char meta[128], *zip, *zip2;
    size_t mlen = utf16("Name=Stage 3\r\n", meta), len, len2;
    int trees, n;

    /* host A: one save and a TDATA file; host B: other saves */
    put("A/hdd/UDATA/4d530065/TitleMeta.xbx", "tm", 2);
    put("A/hdd/UDATA/4d530065/13C91777168C/SaveMeta.xbx", meta, mlen);
    put("A/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-A", 10);
    put("A/hdd/TDATA/4d530065/settings.bin", "tdata-A", 7);
    put("A/hdd/UDATA/4d530004/other.bin", "another game", 12);
    put("B/hdd/UDATA/4d530065/AAAAAAAAAAAA/blinx2data.bin", "progress-B", 10);
    put("B/hdd/TDATA/4d530065/settings.bin", "tdata-B", 7);
    put("B/hdd/TDATA/4d530065/only-b.bin", "b", 1);

    memset(&a, 0, sizeof a);
    path(a.root[0], "A/hdd/UDATA");
    path(a.root[1], "A/hdd/TDATA");
    a.tid = TID;
    b = a;
    path(b.root[0], "B/hdd/UDATA");
    path(b.root[1], "B/hdd/TDATA");

    CHECK(saves_scan(&a, &info) == 0);
    CHECK(info.saves == 1 && info.n == 1 && !strcmp(info.item[0].name, "Stage 3")
          && !strcmp(info.item[0].id, "13C91777168C"));
    CHECK(info.files == 4);

    snprintf(base.title, sizeof base.title, "BLiNX 2");
    snprintf(base.keys, sizeof base.keys, "zero");
    n = saves_export(&a, &base, &zip, &len, s_err, sizeof s_err);
    CHECK(n == 4);
    CHECK(sz_read(zip, len, &arch, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_check(&arch, TID, "zero", &m, &trees, s_err, sizeof s_err) == 0);
    CHECK(trees == 3 && m.files == 4 && !strcmp(m.title, "BLiNX 2"));
    CHECK(!sz_find(&arch, "hdd/UDATA/4d530004/other.bin"));
    {
        struct saves_info pv;
        saves_scan_archive(&arch, TID, &pv);
        CHECK(pv.saves == 1 && !strcmp(pv.item[0].name, "Stage 3") && pv.files == 4);
    }
    /* the same saves export to the same bytes, but for created */
    CHECK(saves_export(&a, &base, &zip2, &len2, s_err, sizeof s_err) == 4 && len2 == len);
    free(zip2);

    /* busy at the swap: nothing changes on B */
    CHECK(saves_import(&b, &arch, trees, busy_yes, NULL, s_err, sizeof s_err) == -1
          && strstr(s_err, "saving"));
    CHECK(same("B/hdd/UDATA/4d530065/AAAAAAAAAAAA/blinx2data.bin", "progress-B"));
    CHECK(!stage_left("B/hdd"));

    /* the import: B gets A's trees whole */
    CHECK(saves_import(&b, &arch, trees, NULL, NULL, s_err, sizeof s_err) == 0);
    CHECK(same("B/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-A"));
    CHECK(same("B/hdd/TDATA/4d530065/settings.bin", "tdata-A"));
    CHECK(!there("B/hdd/UDATA/4d530065/AAAAAAAAAAAA"));
    CHECK(!there("B/hdd/TDATA/4d530065/only-b.bin"));
    CHECK(!stage_left("B/hdd"));
    CHECK(saves_scan(&b, &info) == 0 && info.saves == 1 && !strcmp(info.item[0].name, "Stage 3"));
    sz_archive_free(&arch);
    free(zip);

    /* a zip with only UDATA leaves TDATA alone; onto a host with no save
     * folders at all it makes them */
    {
        struct sz_writer w;
        char man[512];
        struct saves_manifest mm = { 0 };
        struct saves_paths c = a;
        int ml;
        mm.format = 1;
        snprintf(mm.kind, sizeof mm.kind, "saves");
        mm.title_id = TID;
        snprintf(mm.keys, sizeof mm.keys, "zero");
        mm.files = 1;
        mm.bytes = 2;
        ml = saves_manifest_write(&mm, man, sizeof man);
        sz_writer_init(&w);
        sz_add_file(&w, SZ_MANIFEST, man, (size_t)ml, 0);
        sz_add_dir(&w, "hdd/UDATA/4d530065/", 0);
        sz_add_file(&w, "hdd/UDATA/4d530065/X/d", "u2", 2, 0);
        CHECK(sz_finish(&w, &zip, &len) == 0);
        CHECK(sz_read(zip, len, &arch, s_err, sizeof s_err) == 0);
        CHECK(saves_zip_check(&arch, TID, "zero", &m, &trees, s_err, sizeof s_err) == 0
              && trees == SAVES_TREE_UDATA);
        CHECK(saves_import(&b, &arch, trees, NULL, NULL, s_err, sizeof s_err) == 0);
        CHECK(same("B/hdd/UDATA/4d530065/X/d", "u2"));
        CHECK(same("B/hdd/TDATA/4d530065/settings.bin", "tdata-A"));
        path(c.root[0], "C/hdd/UDATA");
        path(c.root[1], "C/hdd/TDATA");
        CHECK(saves_scan(&c, &info) == 0 && info.saves == 0 && info.files == 0);
        CHECK(saves_export(&c, &base, &zip2, &len2, s_err, sizeof s_err) == 0 && !zip2);
        fb_mkdirs(c.root[0]);   /* C/hdd exists, the stage goes there */
        CHECK(saves_import(&c, &arch, trees, NULL, NULL, s_err, sizeof s_err) == 0);
        CHECK(same("C/hdd/UDATA/4d530065/X/d", "u2"));
        sz_archive_free(&arch);
        free(zip);
    }
}

/* ---- rollback: renames failed on purpose (saves_store_rename_hook) ---- */

static unsigned s_fail_mask;   /* bit k: the k-th rename (from 1) fails */
static int s_renames;

static int fail_some(const char *from, const char *to)
{
    (void)from;
    (void)to;
    s_renames++;
    return s_renames < 32 && (s_fail_mask >> s_renames) & 1;
}

/* R: a host whose saves must come through each failed import as they were */
static void r_reset(const char *host, struct saves_paths *r)
{
    char rel[FB_PATH_MAX];
    snprintf(rel, sizeof rel, "%s/hdd/UDATA/4d530065/RRRRRRRRRRRR/blinx2data.bin", host);
    put(rel, "progress-R", 10);
    snprintf(rel, sizeof rel, "%s/hdd/TDATA/4d530065/settings.bin", host);
    put(rel, "tdata-R", 7);
    memset(r, 0, sizeof *r);
    snprintf(rel, sizeof rel, "%s/hdd/UDATA", host);
    path(r->root[0], rel);
    snprintf(rel, sizeof rel, "%s/hdd/TDATA", host);
    path(r->root[1], rel);
    r->tid = TID;
}

static int r_same(const char *host)
{
    char u[FB_PATH_MAX], t[FB_PATH_MAX];
    snprintf(u, sizeof u, "%s/hdd/UDATA/4d530065/RRRRRRRRRRRR/blinx2data.bin", host);
    snprintf(t, sizeof t, "%s/hdd/TDATA/4d530065/settings.bin", host);
    return same(u, "progress-R") && same(t, "tdata-R");
}

static int import_failing(const struct saves_paths *r, const struct sz_archive *a, int trees,
                          unsigned mask)
{
    int rc;
    s_fail_mask = mask;
    s_renames = 0;
    saves_store_rename_hook(fail_some);
    rc = saves_import(r, a, trees, NULL, NULL, s_err, sizeof s_err);
    saves_store_rename_hook(NULL);
    return rc;
}

static void test_rollback(void)
{
    struct saves_paths a, r;
    struct saves_manifest base = { 0 }, m;
    struct sz_archive arch;
    unsigned char *zip;
    size_t len;
    int trees;

    memset(&a, 0, sizeof a);
    path(a.root[0], "A/hdd/UDATA");
    path(a.root[1], "A/hdd/TDATA");
    a.tid = TID;
    snprintf(base.keys, sizeof base.keys, "zero");
    CHECK(saves_export(&a, &base, &zip, &len, s_err, sizeof s_err) > 0);
    CHECK(sz_read(zip, len, &arch, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_check(&arch, TID, "zero", &m, &trees, s_err, sizeof s_err) == 0
          && trees == 3);

    /* The renames of a two-tree import, in order: 1 UDATA aside, 2 UDATA
     * in, 3 TDATA aside, 4 TDATA in; the undo then goes TDATA first. */
    r_reset("R", &r);
    CHECK(import_failing(&r, &arch, trees, 1u << 3) == -1 && has("current saves aside")
          && !has("undoing"));
    CHECK(r_same("R") && !stage_left("R/hdd") && s_renames == 5);

    CHECK(import_failing(&r, &arch, trees, 1u << 4) == -1 && has("new saves in")
          && !has("undoing"));
    CHECK(r_same("R") && !stage_left("R/hdd") && s_renames == 7);

    CHECK(import_failing(&r, &arch, trees, 1u << 2) == -1 && has("new saves in"));
    CHECK(r_same("R") && !stage_left("R/hdd") && s_renames == 3);

    /* UDATA's old saves will not go back: said, and kept in the stage */
    CHECK(import_failing(&r, &arch, trees, (1u << 4) | (1u << 7)) == -1
          && has("new saves in") && has("undoing it failed too")
          && has("old UDATA saves are kept in") && has(".saves-import-"));
    CHECK(stage_left("R/hdd") && !there("R/hdd/UDATA/4d530065"));
    CHECK(same("R/hdd/TDATA/4d530065/settings.bin", "tdata-R"));
    {
        /* the kept path is the one err gives */
        const char *at = strstr(s_err, "kept in ");
        char kept[FB_PATH_MAX], f[FB_PATH_MAX];
        CHECK(at != NULL);
        if (at) {
            snprintf(kept, sizeof kept, "%s", at + 8);
            snprintf(f, sizeof f, "%s/RRRRRRRRRRRR/blinx2data.bin", kept);
#ifdef _WIN32
            for (char *c = f; *c; c++)
                if (*c == '/') *c = '\\';
#endif
            CHECK(fb_exists(f));
        }
    }

    /* the zip's UDATA will not come out again: the live tree is the zip's,
     * the old one kept */
    r_reset("S", &r);
    CHECK(import_failing(&r, &arch, trees, (1u << 4) | (1u << 6)) == -1
          && has("UDATA holds the zip's saves") && has("old UDATA saves are kept in"));
    CHECK(same("S/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-A"));
    CHECK(same("S/hdd/TDATA/4d530065/settings.bin", "tdata-R"));
    CHECK(stage_left("S/hdd"));

    /* A retry in the same second never reuses a kept stage folder: these
     * stand in for one, under the name the first try of this second (and
     * the next) would take. */
    r_reset("Q", &r);
    {
        char rel[FB_PATH_MAX];
        unsigned long now = (unsigned long)time(NULL);
#ifdef _WIN32
        unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
        unsigned long pid = (unsigned long)getpid();
#endif
        for (unsigned long d = 0; d < 2; d++)
            for (int t = 0; t < 2; t++) {
                snprintf(rel, sizeof rel, "Q/hdd/.saves-import-%lu-%lu-0-%s/old/keep.bin",
                         now + d, pid, t ? "TDATA" : "UDATA");
                put(rel, "kept", 4);
            }
        CHECK(saves_import(&r, &arch, trees, NULL, NULL, s_err, sizeof s_err) == 0);
        CHECK(same("Q/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-A"));
        for (unsigned long d = 0; d < 2; d++)
            for (int t = 0; t < 2; t++) {
                snprintf(rel, sizeof rel, "Q/hdd/.saves-import-%lu-%lu-0-%s/old/keep.bin",
                         now + d, pid, t ? "TDATA" : "UDATA");
                CHECK(same(rel, "kept"));
            }
    }
    sz_archive_free(&arch);
    free(zip);
}

/* More saves than the list holds: each counted once, however many files */
static void test_many_saves(void)
{
    struct sz_writer w;
    struct sz_archive arch;
    struct saves_info pv;
    unsigned char *zip;
    size_t len;
    char name[96];
    sz_writer_init(&w);
    sz_add_file(&w, SZ_MANIFEST, "x", 1, 0);
    for (int i = 0; i < SAVES_LIST_MAX + 5; i++)
        for (int f = 0; f < 3; f++) {
            snprintf(name, sizeof name, "hdd/UDATA/4d530065/SAVE%08d/f%d", i, f);
            sz_add_file(&w, name, "d", 1, 0);
        }
    /* one past the list again, after the others */
    sz_add_file(&w, "hdd/UDATA/4d530065/SAVE00000013/late", "d", 1, 0);
    CHECK(sz_finish(&w, &zip, &len) == 0);
    CHECK(sz_read(zip, len, &arch, s_err, sizeof s_err) == 0);
    saves_scan_archive(&arch, TID, &pv);
    CHECK(pv.n == SAVES_LIST_MAX && pv.saves == SAVES_LIST_MAX + 5);
    CHECK(pv.files == (SAVES_LIST_MAX + 5) * 3 + 1);
    sz_archive_free(&arch);
    free(zip);
}

int main(void)
{
    make_temp();
    printf("temp folder %s\n", T);
    test_crc();
    test_names();
    test_round_trip();
    test_refusals();
    test_meta();
    test_store();
    test_rollback();
    test_many_saves();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
