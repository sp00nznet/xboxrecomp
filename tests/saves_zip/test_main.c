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
#include "saves_sign.h"
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
    /* other keys pass and are reported: the import re-signs (saves_sign) */
    CHECK(saves_zip_check(&a, TID, "0123abcd", &back, &trees, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_keys_differ(&back, "0123abcd") && !saves_zip_keys_differ(&back, "zero"));
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

/* ---- the re-sign under a keys mismatch (saves_sign.h; pad-recording-ui R6) ---- */

static const char *const SIGN_OK =
    "# saves-signing.toml\nformat = 1\nsigning = \"resign\"\n\n"
    "[[signed]]\nglob = \"*/Game.BIN\"\nsize = 64\ndata = [0, 40]\ndigest = 40\nkey = \"roamable\"\n";

static int sign_parse(const char *text, struct saves_signing *sg)
{
    s_err[0] = 0;
    return saves_signing_parse(text, strlen(text), sg, s_err, sizeof s_err);
}

static void hex(const unsigned char *b, size_t n, char *out)
{
    for (size_t i = 0; i < n; i++)
        snprintf(out + 2 * i, 3, "%02x", b[i]);
}

static void test_sign_parse(void)
{
    struct saves_signing sg;
    char buf[8192];
    int idx[4];

    CHECK(sign_parse(SIGN_OK, &sg) == 0);
    CHECK(sg.mode == SAVES_SIGNING_RESIGN && sg.n == 1 && sg.r[0].size == 64
          && sg.r[0].start == 0 && sg.r[0].end == 40 && sg.r[0].digest == 40 && sg.r[0].roamable);
    /* globs match any case, '*' spans '/' as fnmatch's does */
    CHECK(!strcmp(sg.r[0].glob, "*/game.bin"));
    CHECK(saves_recipes_for(&sg, "ABC123/game.bin", idx, 4) == 1);
    CHECK(saves_recipes_for(&sg, "abc/def/GAME.BIN", idx, 4) == 1);
    CHECK(saves_recipes_for(&sg, "game.bin", idx, 4) == 0);
    CHECK(saves_glob_match("[!a]?/x[0-9]", "b1/x7") && !saves_glob_match("[!a]?/x[0-9]", "a1/x7"));
    CHECK(saves_glob_match("a[b", "a[b") && !saves_glob_match("a[b]", "a[b"));
    CHECK(sign_parse("format = 1\nsigning = \"none\"\n", &sg) == 0 && sg.mode == SAVES_SIGNING_NONE);
    CHECK(sign_parse("format = 1\n", &sg) == 0 && sg.mode == SAVES_SIGNING_UNKNOWN);
    CHECK(sign_parse("format = 1\r\nsigning = \"resign\"\r\n[[signed]]\r\nglob = \"*/a\"\r\n"
                     "size = 0x40\r\ndata = [ 0 , 40 ]  # range\r\ndigest = 40\r\nkey = \"roamable\"\r\n",
                     &sg) == 0 && sg.n == 1 && sg.r[0].size == 64);

    /* each bound refuses the whole file, and leaves no recipe behind */
#define BAD(text, word)                                                                    \
    do {                                                                                   \
        memset(&sg, 0x55, sizeof sg);                                                      \
        CHECK(sign_parse(text, &sg) == -1 && sg.n == 0 && sg.mode == SAVES_SIGNING_UNKNOWN \
              && has(word));                                                               \
    } while (0)
#define REC(glob, size, data, digest)                                                      \
    "format = 1\nsigning = \"resign\"\n[[signed]]\nglob = \"" glob "\"\nsize = " size     \
    "\ndata = " data "\ndigest = " digest "\nkey = \"roamable\"\n"
    BAD(REC("*/a", "64", "[40, 40]", "0"), "data range");     /* start == end */
    BAD(REC("*/a", "64", "[0, 65]", "44"), "data range");     /* end past size */
    BAD(REC("*/a", "64", "[0, 40]", "45"), "digest is not inside");  /* 45 + 20 > 64 */
    BAD(REC("*/a", "64", "[0, 40]", "20"), "inside recipe");  /* digest in its data */
    BAD(REC("*/a", "64", "[30, 50]", "15"), "inside recipe"); /* overlaps the range's start */
    BAD(REC("*/a", "0", "[0, 1]", "0"), "size");
    BAD(REC("/abs", "64", "[0, 40]", "40"), "glob");
    BAD(REC("../a", "64", "[0, 40]", "40"), "glob");
    BAD(REC("*/a", "64", "[0, 40]", "99999999999"), "digest");
    BAD("format = 2\nsigning = \"none\"\n", "newer");
    BAD("signing = \"none\"\n", "format");
    BAD("format = 1\nsigning = \"resign\"\n", "no [[signed]]");
    BAD("format = 1\nsigning = \"maybe\"\n", "signing");
    BAD("format = 1\nsigning = \"resign\"\n[[signed]]\nglob = \"*/a\"\nsize = 64\n", "lacks");
    BAD("format = 1\nsigning = \"resign\"\n[[signed]]\nglob = \"*/a\"\nglob = \"*/b\"\n", "twice");
    BAD("format = 1\nsigning = \"resign\"\n[[signed]]\nwhat = 1\n", "unknown key");
    BAD("format = 1\n[other]\n", "unknown table");
    BAD("format = 1\nsigning = \"none\"\n" "[[signed]]\nglob = \"*/a\"\nsize = 64\ndata = [0, 40]\n"
        "digest = 40\nkey = \"roamable\"\n", "without");
    /* two recipes of one glob: one size, digests apart and outside each
     * other's data */
    BAD("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"*/a\"\nsize = 64\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"*/A\"\nsize = 80\ndata = [0, 20]\ndigest = 20\nkey = \"roamable\"\n",
        "not a size");
    BAD("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"*/a\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"*/a\"\nsize = 100\ndata = [0, 20]\ndigest = 50\nkey = \"roamable\"\n",
        "overlap");
    BAD("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"*/a\"\nsize = 100\ndata = [0, 50]\ndigest = 60\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"*/a\"\nsize = 100\ndata = [0, 20]\ndigest = 30\nkey = \"roamable\"\n",
        "inside recipe");
    /* two globs that can match one file are checked as one glob would be
     * ("s1/game.bin" matches both), and two that cannot are not (review 1,
     * F5); a set and a '?' meet too, a literal apart does not */
    BAD("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"*/game.bin\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"S1/*\"\nsize = 100\ndata = [0, 20]\ndigest = 50\nkey = \"roamable\"\n",
        "overlap");
    BAD("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"s[0-9]/a?\"\nsize = 100\ndata = [0, 50]\ndigest = 60\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"s?/[!b]x\"\nsize = 100\ndata = [0, 20]\ndigest = 30\nkey = \"roamable\"\n",
        "inside recipe");
    CHECK(sign_parse("format = 1\nsigning = \"resign\"\n"
        "[[signed]]\nglob = \"*/game.bin\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"*/other.bin\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"s[0-9]/a\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n"
        "[[signed]]\nglob = \"s[!0-9]/a\"\nsize = 100\ndata = [0, 20]\ndigest = 40\nkey = \"roamable\"\n",
        &sg) == 0 && sg.n == 4);
    /* ASCII globs only: the match folds case in ASCII (review 1, F7) */
    BAD(REC("*/caf\xc3\xa9", "64", "[0, 40]", "40"), "not ASCII");
    /* a glob of 129 bytes, and 65 recipes */
    {
        char g[140];
        memset(g, 'g', 129);
        g[129] = 0;
        snprintf(buf, sizeof buf, REC("%s", "64", "[0, 40]", "40"), g);
        BAD(buf, "128 bytes");
        g[128] = 0;
        snprintf(buf, sizeof buf, REC("%s", "64", "[0, 40]", "40"), g);
        CHECK(sign_parse(buf, &sg) == 0);
    }
    {
        static char many[SAVES_SIGN_FILE_MAX];
        size_t k = (size_t)snprintf(many, sizeof many, "format = 1\nsigning = \"resign\"\n");
        for (int i = 0; i < SAVES_SIGN_MAX + 1; i++)
            k += (size_t)snprintf(many + k, sizeof many - k,
                                  "[[signed]]\nglob = \"*/f%d\"\nsize = 64\ndata = [0, 40]\n"
                                  "digest = 40\nkey = \"roamable\"\n", i);
        BAD(many, "more than 64");
        /* the first 64 alone pass */
        *strstr(many, "[[signed]]\nglob = \"*/f64\"") = 0;
        CHECK(sign_parse(many, &sg) == 0 && sg.n == SAVES_SIGN_MAX);
    }
#undef REC
#undef BAD
}

/* The C port against importer.py resign on BLiNX 2's recipe (81920 bytes,
 * data [0, 33972), digest at 33972) over a synthetic file, no game data.
 * The expected digest is Python's:
 *   b = bytes(((i * 31 + 7) & 0xff) for i in range(81920))
 *   hmac.new(bytes(16), b[0:33972], hashlib.sha1).hexdigest() */
static void test_sign_blinx2(void)
{
    static const char *const want = "fe9bcf74c6510a8d92b3f58e1ffbd6f38b2cdab6";
    struct saves_signing sg;
    unsigned char *b = malloc(81920), *orig = malloc(81920);
    char got[41];
    int idx[4], n;
    CHECK(sign_parse("format = 1\nsigning = \"resign\"\n[[signed]]\n"
                     "glob = \"*/blinx2data.bin\"\nsize = 81920\ndata = [0, 33972]\n"
                     "digest = 33972\nkey = \"roamable\"\n", &sg) == 0);
    for (int i = 0; i < 81920; i++)
        b[i] = (unsigned char)((i * 31 + 7) & 0xff);
    memcpy(orig, b, 81920);
    n = saves_recipes_for(&sg, "13C91777168C/BLiNX2DATA.bin", idx, 4);
    CHECK(n == 1);
    CHECK(saves_resign(b, 81920, &sg, idx, n) == 0);
    hex(b + 33972, 20, got);
    CHECK(!strcmp(got, want));
    /* only the digest changed */
    CHECK(!memcmp(b, orig, 33972) && !memcmp(b + 33992, orig + 33992, 81920 - 33992));
    /* idempotent: a save this build wrote comes back byte for byte */
    memcpy(orig, b, 81920);
    CHECK(saves_resign(b, 81920, &sg, idx, n) == 0 && !memcmp(b, orig, 81920));
    /* the size is checked before any write */
    CHECK(saves_resign(b, 81919, &sg, idx, n) == -1 && !memcmp(b, orig, 81920));
    free(b);
    free(orig);
}

/* A saves zip with three saves of a fake game: one with a recipe file
 * (64 bytes, data [0, 40), digest at 40), one with a file of the wrong
 * size, one with no recipe file; and a TDATA file. */
static unsigned char *sign_zip(size_t *len, const char *keys)
{
    struct sz_writer w;
    struct saves_manifest m = { 0 };
    unsigned char good[64], *z;
    char man[512];
    int ml;
    for (int i = 0; i < 64; i++)
        good[i] = (unsigned char)((i * 13 + 1) & 0xff);
    m.format = 1;
    snprintf(m.kind, sizeof m.kind, "saves");
    m.title_id = TID;
    snprintf(m.keys, sizeof m.keys, "%s", keys);
    m.files = 7;
    m.bytes = 64 + 2 + 63 + 2 + 2 + 2 + 2;
    ml = saves_manifest_write(&m, man, sizeof man);
    sz_writer_init(&w);
    sz_add_file(&w, SZ_MANIFEST, man, (size_t)ml, 0);
    sz_add_dir(&w, "hdd/UDATA/4d530065/", 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/TitleMeta.xbx", "tm", 2, 0);
    sz_add_dir(&w, "hdd/UDATA/4d530065/GOOD/", 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/GOOD/Game.bin", good, 64, 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/GOOD/SaveMeta.xbx", "sm", 2, 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/SHORT/game.bin", good, 63, 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/SHORT/SaveMeta.xbx", "sm", 2, 0);
    sz_add_file(&w, "hdd/UDATA/4d530065/NONE/SaveMeta.xbx", "sm", 2, 0);
    sz_add_file(&w, "hdd/TDATA/4d530065/t.bin", "td", 2, 0);
    CHECK(sz_finish(&w, &z, len) == 0);
    return z;
}

static const struct saves_resign_save *verdict_of(const struct saves_resign_report *r,
                                                   const char *sid)
{
    for (int i = 0; i < r->n && i < SAVES_RESIGN_MAX_SAVES; i++)
        if (!strcmp(r->s[i].sid, sid))
            return &r->s[i];
    return NULL;
}

static void test_sign_archive(void)
{
    static const char *const want = "25be166520185d509e0fe30954915859afec07fd";
    struct saves_signing sg;
    struct sz_archive a, out;
    struct saves_manifest m;
    struct saves_resign_report rep;
    const struct sz_entry *e;
    unsigned char *z, *blob;
    size_t len;
    int trees, ot;
    char got[41], lines[2048];

    z = sign_zip(&len, "0123abcd");
    CHECK(sz_read(z, len, &a, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_check(&a, TID, "zero", &m, &trees, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_keys_differ(&m, "zero") && trees == 3);

    /* resign: GOOD re-signed, SHORT and NONE skipped with their reasons */
    CHECK(sign_parse(SIGN_OK, &sg) == 0);
    CHECK(saves_resign_archive(&a, TID, trees, &sg, &out, &blob, &ot, &rep, s_err,
                               sizeof s_err) == 0);
    CHECK(ot == 3 && rep.n == 3 && rep.resigned == 1 && rep.skipped == 2 && rep.kept == 0);
    CHECK(verdict_of(&rep, "GOOD") && verdict_of(&rep, "GOOD")->verdict == SAVES_RESIGNED);
    CHECK(verdict_of(&rep, "SHORT") && verdict_of(&rep, "SHORT")->verdict == SAVES_SKIPPED
          && strstr(verdict_of(&rep, "SHORT")->why, "63 bytes, not 64"));
    CHECK(verdict_of(&rep, "NONE") && verdict_of(&rep, "NONE")->verdict == SAVES_SKIPPED
          && strstr(verdict_of(&rep, "NONE")->why, "no recipe"));
    CHECK((e = sz_find(&out, "hdd/UDATA/4d530065/GOOD/Game.bin")) && e->size == 64);
    if (e) {
        hex(e->data + 40, 20, got);
        CHECK(!strcmp(got, want));
        CHECK(e->crc == sz_crc32(0, e->data, 64));
        CHECK(e->data != sz_find(&a, "hdd/UDATA/4d530065/GOOD/Game.bin")->data);
    }
    CHECK(sz_find(&out, "hdd/UDATA/4d530065/GOOD/SaveMeta.xbx"));
    CHECK(sz_find(&out, "hdd/UDATA/4d530065/TitleMeta.xbx"));
    CHECK(sz_find(&out, "hdd/TDATA/4d530065/t.bin"));
    CHECK(!sz_find(&out, "hdd/UDATA/4d530065/SHORT/game.bin"));
    CHECK(!sz_find(&out, "hdd/UDATA/4d530065/NONE/SaveMeta.xbx"));
    /* the input is left as it was */
    hex(sz_find(&a, "hdd/UDATA/4d530065/GOOD/Game.bin")->data + 40, 20, got);
    CHECK(strcmp(got, want));
    saves_resign_lines(&rep, 0, lines, sizeof lines);
    CHECK(strstr(lines, "GOOD: " SAVES_SAID_RESIGNED " [" SAVES_SAID_MARK "]"));
    CHECK(strstr(lines, "SHORT: " SAVES_SAID_SKIPPED "\n"));
    CHECK(strstr(lines, SAVES_SAID_WITHOUT));
    saves_resign_lines(&rep, 1, lines, sizeof lines);
    CHECK(strstr(lines, "NONE: " SAVES_SAID_SKIPPED " (no recipe matches its files)"));
    /* a short buffer keeps whole lines and never runs past its end (review
     * 1, F6) */
    {
        char small[40];
        memset(small, 'x', sizeof small);
        CHECK(saves_resign_lines(&rep, 1, small, sizeof small - 8) == strlen(small)
              && strlen(small) < sizeof small - 8 && small[sizeof small - 1] == 'x');
        memset(small, 'x', sizeof small);
        CHECK(saves_resign_lines(&rep, 1, small, 0) == 0 && small[0] == 'x');
    }
    /* the worst case fits SAVES_RESIGN_LINES_MAX: 64 saves skipped with a
     * reason of 95 bytes, more past them, and the closing line */
    {
        static struct saves_resign_report big;
        static char all[SAVES_RESIGN_LINES_MAX];
        memset(&big, 0, sizeof big);
        big.n = SAVES_RESIGN_MAX_SAVES + 5;
        big.skipped = big.n;
        for (int i = 0; i < SAVES_RESIGN_MAX_SAVES; i++) {
            memset(big.s[i].sid, 'S', SZ_NAME_MAX);
            big.s[i].verdict = SAVES_SKIPPED;
            memset(big.s[i].why, 'w', sizeof big.s[i].why - 1);
        }
        CHECK(saves_resign_lines(&big, 1, all, sizeof all) < sizeof all - 1);
        CHECK(strstr(all, "and 5 more\n") && strstr(all, SAVES_SAID_WITHOUT "\n"));
    }
    sz_archive_free(&out);
    free(blob);

    /* a console-locked recipe skips its save */
    CHECK(sign_parse("format = 1\nsigning = \"resign\"\n[[signed]]\nglob = \"*/game.bin\"\n"
                     "size = 64\ndata = [0, 40]\ndigest = 40\nkey = \"console\"\n", &sg) == 0);
    CHECK(saves_resign_archive(&a, TID, trees, &sg, &out, &blob, &ot, &rep, s_err,
                               sizeof s_err) == 0);
    CHECK(rep.resigned == 0 && rep.skipped == 3 && ot == SAVES_TREE_TDATA
          && strstr(verdict_of(&rep, "GOOD")->why, "console-locked"));
    /* no save left: UDATA is dropped whole, so the live saves stay */
    CHECK(!sz_find(&out, "hdd/UDATA/4d530065/TitleMeta.xbx") && sz_find(&out, "hdd/TDATA/4d530065/t.bin"));
    sz_archive_free(&out);
    free(blob);

    /* signing = "none": every save goes in as it is */
    CHECK(sign_parse("format = 1\nsigning = \"none\"\n", &sg) == 0);
    CHECK(saves_resign_archive(&a, TID, trees, &sg, &out, &blob, &ot, &rep, s_err,
                               sizeof s_err) == 0);
    CHECK(rep.kept == 3 && rep.skipped == 0 && ot == 3 && out.n == a.n && !blob);
    saves_resign_lines(&rep, 1, lines, sizeof lines);
    CHECK(!strstr(lines, SAVES_SAID_WITHOUT));
    sz_archive_free(&out);

    /* no file (or no signing key): every save is skipped */
    memset(&sg, 0, sizeof sg);
    CHECK(saves_resign_archive(&a, TID, trees, &sg, &out, &blob, &ot, &rep, s_err,
                               sizeof s_err) == 0);
    CHECK(rep.skipped == 3 && ot == SAVES_TREE_TDATA
          && strstr(verdict_of(&rep, "NONE")->why, "saves-signing.toml"));
    sz_archive_free(&out);
    free(blob);
    sz_archive_free(&a);
    free(z);

    /* a saves-signing.toml inside a zip is never read: the saves rules
     * refuse it as an entry outside the save trees, so it can never reach
     * the parser (the menu reads the file only from beside the exe) */
    {
        struct sz_writer w;
        unsigned char *zz;
        size_t zl;
        char man[512];
        struct saves_manifest mm = { 0 };
        int ml;
        mm.format = 1;
        snprintf(mm.kind, sizeof mm.kind, "saves");
        mm.title_id = TID;
        snprintf(mm.keys, sizeof mm.keys, "0123abcd");
        mm.files = 2;
        mm.bytes = 2 + (uint64_t)strlen(SIGN_OK);
        ml = saves_manifest_write(&mm, man, sizeof man);
        sz_writer_init(&w);
        sz_add_file(&w, SZ_MANIFEST, man, (size_t)ml, 0);
        sz_add_file(&w, SAVES_SIGN_FILE, SIGN_OK, strlen(SIGN_OK), 0);
        sz_add_file(&w, "hdd/UDATA/4d530065/GOOD/SaveMeta.xbx", "sm", 2, 0);
        CHECK(sz_finish(&w, &zz, &zl) == 0);
        CHECK(sz_read(zz, zl, &a, s_err, sizeof s_err) == 0);
        CHECK(saves_zip_check(&a, TID, "zero", &m, &trees, s_err, sizeof s_err) == -1
              && has("outside the game's saves"));
        sz_archive_free(&a);
        free(zz);
    }
}

/* The Saves page's mismatch path on disk: the derived archive imported,
 * the re-signed bytes on the disk, the skipped saves gone with the tree
 * they were in (the import replaces the title's saves whole, after the
 * page's backup). */
static void test_sign_store(void)
{
    static const char *const want = "25be166520185d509e0fe30954915859afec07fd";
    struct saves_paths d;
    struct saves_signing sg;
    struct sz_archive a, out;
    struct saves_manifest m;
    struct saves_resign_report rep;
    unsigned char *z, *blob;
    char p[FB_PATH_MAX], *buf, got[41];
    size_t len, bl;
    int trees, ot;

    memset(&d, 0, sizeof d);
    path(d.root[0], "K/hdd/UDATA");
    path(d.root[1], "K/hdd/TDATA");
    d.tid = TID;
    put("K/hdd/UDATA/4d530065/OLD/SaveMeta.xbx", "old", 3);
    z = sign_zip(&len, "0123abcd");
    CHECK(sz_read(z, len, &a, s_err, sizeof s_err) == 0);
    CHECK(saves_zip_check(&a, TID, "zero", &m, &trees, s_err, sizeof s_err) == 0);
    CHECK(sign_parse(SIGN_OK, &sg) == 0);
    CHECK(saves_resign_archive(&a, TID, trees, &sg, &out, &blob, &ot, &rep, s_err,
                               sizeof s_err) == 0);
    CHECK(saves_import(&d, &out, ot, NULL, NULL, s_err, sizeof s_err) == 0);
    path(p, "K/hdd/UDATA/4d530065/GOOD/Game.bin");
    CHECK(fb_read_file(p, 1 << 20, &buf, &bl) == FB_OK && bl == 64);
    if (bl == 64) {
        hex((unsigned char *)buf + 40, 20, got);
        CHECK(!strcmp(got, want));
        free(buf);
    }
    CHECK(!there("K/hdd/UDATA/4d530065/SHORT") && !there("K/hdd/UDATA/4d530065/NONE"));
    CHECK(!there("K/hdd/UDATA/4d530065/OLD"));
    CHECK(same("K/hdd/TDATA/4d530065/t.bin", "td"));
    CHECK(!stage_left("K/hdd"));
    sz_archive_free(&out);
    free(blob);
    sz_archive_free(&a);
    free(z);
}

/* ---- a bug-report bundle (pad-recording-ui R6, task 3.7) ---- */

/* sz_read then report_zip_check; 0 when both pass */
static int check_report(const unsigned char *z, size_t len, uint32_t tid,
                        struct report_manifest *m, int *trees)
{
    struct sz_archive a;
    int r;
    s_err[0] = 0;
    if (sz_read(z, len, &a, s_err, sizeof s_err))
        return -1;
    r = report_zip_check(&a, tid, m, trees, s_err, sizeof s_err);
    sz_archive_free(&a);
    return r;
}

/* a raw report: the manifest with chain, then the entries given */
static int raw_report(const char *chain, int files, int bytes, const struct raw *more, int n)
{
    static struct report_manifest m;
    char man[1024];
    struct raw e[8];
    size_t len;
    unsigned char *z;
    int trees, r;
    snprintf(man, sizeof man,
             "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\n"
             "files = %d\nbytes = %d\nchain = [%s]\n", files, bytes, chain);
    e[0] = (struct raw){ SZ_REPORT, man, (uint32_t)strlen(man), 0, 0, 0, 0 };
    for (int i = 0; i < n; i++)
        e[i + 1] = more[i];
    z = raw_zip(e, n + 1, &len);
    r = check_report(z, len, TID, &m, &trees);
    free(z);
    return r;
}

static void test_report(void)
{
    struct report_manifest m, back;
    struct saves_paths p, q;
    struct report_pad pads[2];
    struct sz_archive arch;
    char man[2048], name[SZ_PATH_MAX];
    const char *why;
    unsigned char *zip;
    size_t len;
    int ml, trees;
    static const char setup[] = "# xbr-export: pad 1\ng0:!debug_menu.open\ng1:@end\n";
    static const char run[] = "# xbr-export: pad 1\ng0:a:4\ng9:@end\n";

    /* the manifest, both ways */
    memset(&m, 0, sizeof m);
    m.s.format = 1;
    snprintf(m.s.kind, sizeof m.s.kind, "report");
    m.s.title_id = TID;
    snprintf(m.s.title, sizeof m.s.title, "BLiNX 2");
    snprintf(m.s.keys, sizeof m.s.keys, "zero");
    m.s.files = 2;
    m.s.bytes = 9;
    snprintf(m.game, sizeof m.game, "blinx-2");
    snprintf(m.host_os, sizeof m.host_os, "macos");
    snprintf(m.note, sizeof m.note, "falls \"through\" the floor \\ at 2-1\nafter the gate");
    m.nchain = 2;
    snprintf(m.chain[0], sizeof m.chain[0], "pad/1-stage-select.pad");
    snprintf(m.chain[1], sizeof m.chain[1], "pad/2-blinx-2-20261107-1200.pad");
    ml = report_manifest_write(&m, man, sizeof man);
    CHECK(ml > 0 && strstr(man, "note = \"falls \\\"through"));
    CHECK(report_manifest_parse(man, (size_t)ml, &back, &why) == 0);
    CHECK(back.s.title_id == TID && back.s.files == 2 && back.s.bytes == 9
          && !strcmp(back.s.kind, "report") && !strcmp(back.game, "blinx-2")
          && !strcmp(back.host_os, "macos") && back.nchain == 2
          && !strcmp(back.chain[1], m.chain[1]));
    /* the note's newline dropped, its quotes and backslash kept */
    CHECK(!strcmp(back.note, "falls \"through\" the floor \\ at 2-1after the gate"));
    CHECK(report_manifest_write(&m, man, 64) == -1);
    {
        const char *t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\n"
                        "keys = \"zero\"\nfiles = 0\nbytes = 0\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) && strstr(why, "chain"));
        t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\n"
                        "files = 0\nbytes = 0\nchain = [\"pad/1-a.pad\" \"pad/2-b.pad\"]\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) && strstr(why, "chain"));
        t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\n"
            "files = 0\nbytes = 0\nchain = \"pad/1-a.pad\"\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) && strstr(why, "not a list"));
        t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\n"
            "files = 0\nbytes = 0\nchain = [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\",\"g\",\"h\",\"i\"]\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) && strstr(why, "more than 8"));
        /* control bytes in the strings are dropped on read, as the writer
         * drops them: an ESC from someone else's zip never reaches a row */
        t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\nkeys = \"zero\"\n"
            "files = 0\nbytes = 0\nchain = [\"pad/1-a.pad\"]\n"
            "note = \"a\x1b[2Jb\x07" "c\x7f\"\ngame = \"g\x01x\"\nhost_os = \"\tmac\x1bos\"\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) == 0
              && !strcmp(back.note, "a[2Jbc") && !strcmp(back.game, "gx")
              && !strcmp(back.host_os, "macos"));
        /* the saves manifest's rules still hold: keys missing */
        t = "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\n"
            "files = 0\nbytes = 0\nchain = [\"pad/1-a.pad\"]\n";
        CHECK(report_manifest_parse(t, strlen(t), &back, &why) && strstr(why, "missing"));
    }

    /* the pad names: one component of at most 42 bytes, ".pad" kept */
    CHECK(report_pad_name(1, "stage-select.pad", name, sizeof name) == 0
          && !strcmp(name, "pad/1-stage-select.pad"));
    CHECK(report_pad_name(2, "a-very-long-recording-name-the-player-typed-in.pad", name,
                          sizeof name) == 0
          && strlen(name) == 4 + SZ_NAME_MAX && !strcmp(name + strlen(name) - 4, ".pad")
          && !strncmp(name, "pad/2-a-very", 12));
    /* a cut never splits a character: e-acute is two bytes */
    CHECK(report_pad_name(1, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\xc3\xa9x.pad", name, sizeof name)
          == 0 && strlen(name) == 4 + 2 + 35 + 4);
    CHECK(report_pad_name(1, "notes.txt", name, sizeof name) == -1);
    CHECK(!strcmp(report_pad_file("pad/2-run.pad"), "run.pad"));
    CHECK(!strcmp(report_pad_file("pad/run.pad"), "run.pad"));
    CHECK(!strcmp(report_pad_file("pad/12-3-x.pad"), "3-x.pad"));

    /* the store: a report of R's saves, checked, then its saves imported
     * onto S as a saves zip's are (pad/ passed over) */
    put("R/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-R", 10);
    put("R/hdd/TDATA/4d530065/settings.bin", "tdata-R", 7);
    put("S/hdd/UDATA/4d530065/AAAAAAAAAAAA/blinx2data.bin", "progress-S", 10);
    memset(&p, 0, sizeof p);
    path(p.root[0], "R/hdd/UDATA");
    path(p.root[1], "R/hdd/TDATA");
    p.tid = TID;
    q = p;
    path(q.root[0], "S/hdd/UDATA");
    path(q.root[1], "S/hdd/TDATA");
    pads[0] = (struct report_pad){ "pad/1-stage-select.pad", setup, sizeof setup - 1 };
    pads[1] = (struct report_pad){ "pad/2-run.pad", run, sizeof run - 1 };
    CHECK(report_export(&p, &m, pads, 2, &zip, &len, s_err, sizeof s_err) == 2);
    CHECK(check_report(zip, len, TID, &back, &trees) == 0);
    CHECK(trees == 3 && back.s.files == 2 && back.s.bytes == 17 && back.nchain == 2
          && !strcmp(back.chain[0], "pad/1-stage-select.pad")
          && !strcmp(back.chain[1], "pad/2-run.pad") && !strncmp(back.note, "falls", 5)
          && !strcmp(back.game, "blinx-2"));
    /* another title's report, and the Saves page's check, refuse it */
    CHECK(check_report(zip, len, 0x4d530004u, &back, &trees) && has("another game"));
    CHECK(check_zip(zip, len) && has("not exported from the Saves menu"));
    CHECK(sz_read(zip, len, &arch, s_err, sizeof s_err) == 0);
    {
        const struct sz_entry *e = sz_find(&arch, "pad/2-run.pad");
        CHECK(e && e->size == sizeof run - 1 && !memcmp(e->data, run, e->size));
    }
    CHECK(saves_import(&q, &arch, 3, NULL, NULL, s_err, sizeof s_err) == 0);
    CHECK(same("S/hdd/UDATA/4d530065/13C91777168C/blinx2data.bin", "progress-R"));
    CHECK(!there("S/hdd/UDATA/4d530065/AAAAAAAAAAAA"));
    CHECK(!there("S/hdd/pad") && !there("S/hdd/UDATA/pad"));
    sz_archive_free(&arch);
    free(zip);

    /* a title with no saves yet still reports */
    memset(&p, 0, sizeof p);
    path(p.root[0], "R0/hdd/UDATA");
    path(p.root[1], "R0/hdd/TDATA");
    p.tid = TID;
    CHECK(report_export(&p, &m, pads + 1, 1, &zip, &len, s_err, sizeof s_err) == 0);
    CHECK(check_report(zip, len, TID, &back, &trees) == 0 && trees == 0 && back.nchain == 1);
    free(zip);
    CHECK(report_export(&p, &m, pads, 0, &zip, &len, s_err, sizeof s_err) == -1);
    {
        struct report_pad bad = { "hdd/x.pad", run, sizeof run - 1 };
        CHECK(report_export(&p, &m, &bad, 1, &zip, &len, s_err, sizeof s_err) == -1);
    }

    /* each refusal */
    {
        static const char pd[] = "g0:@end\n";
        struct raw a1 = { "pad/1-a.pad", pd, sizeof pd - 1, 0, 0, 0, 0 };
        struct raw a2 = { "pad/2-b.pad", pd, sizeof pd - 1, 0, 0, 0, 0 };
        struct raw sv = { "hdd/UDATA/4d530065/S/f", "abc", 3, 0, 0, 0, 0 };
        struct raw v[4];
        CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, &a1, 1) == 0);
        v[0] = a1; v[1] = sv;
        CHECK(raw_report("\"pad/1-a.pad\"", 1, 3, v, 2) == 0);
        /* the counts cover the trees only */
        CHECK(raw_report("\"pad/1-a.pad\"", 2, 11, v, 2) && has("incomplete"));
        /* a chain entry with no pad/ file */
        CHECK(raw_report("\"pad/1-a.pad\", \"pad/2-b.pad\"", 0, 0, &a1, 1)
              && has("does not hold") && has("pad/2-b.pad"));
        /* a stray pad/ file the chain does not name */
        v[0] = a1; v[1] = a2;
        CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, v, 2) && has("chain does not name")
              && has("pad/2-b.pad"));
        /* a recording with an empty name would import as ".pad" */
        {
            struct raw e0 = { "pad/1-.pad", pd, sizeof pd - 1, 0, 0, 0, 0 };
            CHECK(raw_report("\"pad/1-.pad\"", 0, 0, &e0, 1) && has("no name"));
        }
        /* the same entry twice */
        CHECK(raw_report("\"pad/1-a.pad\", \"pad/1-a.pad\"", 0, 0, &a1, 1) && has("twice"));
        /* the chain outside pad/ */
        CHECK(raw_report("\"hdd/UDATA/4d530065/S/f\"", 1, 3, &sv, 1) && has("does not hold"));
        /* pad/ one level deep, recordings only */
        {
            struct raw d = { "pad/x/1-a.pad", pd, sizeof pd - 1, 0, 0, 0, 0 };
            struct raw t = { "pad/notes.txt", pd, sizeof pd - 1, 0, 0, 0, 0 };
            struct raw f = { "pad/x/", "", 0, 0, 0, 040755, 0 };
            v[0] = a1; v[1] = d;
            CHECK(raw_report("\"pad/1-a.pad\", \"pad/x/1-a.pad\"", 0, 0, v, 2)
                  && has("not a recording"));
            v[1] = t;
            CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, v, 2) && has("not a recording"));
            v[1] = f;
            CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, v, 2) && has("folder inside pad/"));
        }
        /* entries outside pad/ and the trees: a saves manifest is one */
        {
            struct raw sm = { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 };
            struct raw sg = { "saves-signing.toml", "x", 1, 0, 0, 0, 0 };
            v[0] = a1; v[1] = sm;
            CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, v, 2) && has("outside the game's saves"));
            v[1] = sg;
            CHECK(raw_report("\"pad/1-a.pad\"", 0, 0, v, 2) && has("outside the game's saves"));
        }
        /* a saves zip is no report */
        {
            struct raw e[2] = {
                { SZ_MANIFEST, MAN_OK, (uint32_t)strlen(MAN_OK), 0, 0, 0, 0 },
                { "hdd/UDATA/4d530065/A/f", "abc", 3, 0, 0, 0, 0 },
            };
            unsigned char *z = raw_zip(e, 2, &len);
            CHECK(check_report(z, len, TID, &back, &trees) && has("not a bug report"));
            free(z);
        }
    }
    /* the cap: a pad/ file over 4 MB */
    {
        struct sz_writer w;
        char *big = calloc(1, REPORT_PAD_MAX + 1);
        char mt[512];
        int k = snprintf(mt, sizeof mt,
                         "format = 1\nkind = \"report\"\ntitle_id = \"4d530065\"\n"
                         "keys = \"zero\"\nfiles = 0\nbytes = 0\nchain = [\"pad/1-a.pad\"]\n");
        sz_writer_init(&w);
        sz_add_file(&w, SZ_REPORT, mt, (size_t)k, 0);
        sz_add_file(&w, "pad/1-a.pad", big, REPORT_PAD_MAX + 1, 0);
        CHECK(sz_finish(&w, &zip, &len) == 0);
        CHECK(check_report(zip, len, TID, &back, &trees) && has("over 4 MB"));
        free(zip);
        free(big);
    }
    /* a newer format and another kind */
    {
        static const char nf[] = "format = 2\nkind = \"report\"\ntitle_id = \"4d530065\"\n"
                                 "keys = \"zero\"\nfiles = 0\nbytes = 0\nchain = [\"pad/1-a.pad\"]\n";
        static const char pd[] = "g0:@end\n";
        struct raw e[2] = {
            { SZ_REPORT, nf, sizeof nf - 1, 0, 0, 0, 0 },
            { "pad/1-a.pad", pd, sizeof pd - 1, 0, 0, 0, 0 },
        };
        unsigned char *z = raw_zip(e, 2, &len);
        CHECK(check_report(z, len, TID, &back, &trees) && has("newer build"));
        free(z);
    }
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
    test_sign_parse();
    test_sign_blinx2();
    test_sign_archive();
    test_sign_store();
    test_report();
    printf("%d checks, %d failed\n", s_checks, s_failed);
    return s_failed ? 1 : 0;
}
