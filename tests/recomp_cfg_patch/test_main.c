/*
 * recomp_cfg_patch test: the menu's Save rewrites only what it was asked to.
 * Each case patches a text and compares the whole result byte for byte, so
 * a comment, a blank line or a key's spelling that moves is a failure. The
 * file cases write rcp_* in the working directory and remove them.
 */
#include "recomp_cfg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

/* Patch `in` and expect exactly `want` (NULL: a refusal). */
static void expect(int line, const char *in, const char *const *k, const char *const *v,
                   size_t n, const char *want)
{
    char err[256];
    char *got = recomp_cfg_patch_text(in, "t.toml", k, v, n, err, sizeof err);
    s_checks++;
    if (want ? !got || strcmp(got, want) : got != NULL) {
        s_fail++;
        fprintf(stderr, "FAIL line %d\n--- want\n%s\n--- got\n%s\n--- err: %s\n", line,
                want ? want : "(refused)", got ? got : "(refused)", err);
    }
    if (!want && !got)
        CHECK(err[0] != 0);
    free(got);
}
#define EXPECT(in, k, v, want) expect(__LINE__, in, k, v, sizeof k / sizeof k[0], want)

static const char *const s_file =
    "# my settings\n"
    "\n"
    "[render]\n"
    "scale = 2          # sharp enough\n"
    "filter = 'nearest'\n"
    "\n"
    "# the pack\n"
    "[textures]\n"
    "pack = \"auto\"\n";

static void test_replace(void)
{
    const char *k[] = { "render.scale" }, *v[] = { "3" };
    EXPECT(s_file, k, v,
           "# my settings\n"
           "\n"
           "[render]\n"
           "scale = 3          # sharp enough\n"
           "filter = 'nearest'\n"
           "\n"
           "# the pack\n"
           "[textures]\n"
           "pack = \"auto\"\n");
    {
        /* the same value: the same bytes, even the user's quote style */
        const char *k2[] = { "render.filter" }, *v2[] = { "\"nearest\"" };
        EXPECT(s_file, k2, v2,
               "# my settings\n\n[render]\nscale = 2          # sharp enough\n"
               "filter = \"nearest\"\n\n# the pack\n[textures]\npack = \"auto\"\n");
    }
}

static void test_add(void)
{
    /* under the table's own header, after its last key */
    const char *k[] = { "render.aniso", "textures.font" }, *v[] = { "8", "\"off\"" };
    EXPECT(s_file, k, v,
           "# my settings\n\n[render]\nscale = 2          # sharp enough\n"
           "filter = 'nearest'\naniso = 8\n\n# the pack\n[textures]\npack = \"auto\"\n"
           "font = \"off\"\n");
    {
        /* new tables at the end, in the order given, one header each */
        const char *k2[] = { "present.filter", "overlay.stats", "present.fullscreen" };
        const char *v2[] = { "\"linear\"", "true", "true" };
        EXPECT(s_file, k2, v2,
               "# my settings\n\n[render]\nscale = 2          # sharp enough\n"
               "filter = 'nearest'\n\n# the pack\n[textures]\npack = \"auto\"\n"
               "\n[present]\nfilter = \"linear\"\nfullscreen = true\n"
               "\n[overlay]\nstats = true\n");
    }
    {
        /* an empty or missing file */
        const char *k3[] = { "render.scale" }, *v3[] = { "2" };
        EXPECT("", k3, v3, "[render]\nscale = 2\n");
        EXPECT(NULL, k3, v3, "[render]\nscale = 2\n");
    }
    {
        /* a header with no keys yet takes the key right below it */
        const char *k4[] = { "render.scale" }, *v4[] = { "4" };
        EXPECT("[render]\n# later\n", k4, v4, "[render]\nscale = 4\n# later\n");
    }
    {
        /* a root key goes before the first header */
        const char *k5[] = { "verbose" }, *v5[] = { "true" };
        EXPECT("a = 1\n[render]\nscale = 2\n", k5, v5, "a = 1\nverbose = true\n[render]\nscale = 2\n");
        EXPECT("[render]\nscale = 2\n", k5, v5, "verbose = true\n\n[render]\nscale = 2\n");
    }
    {
        /* a table made by a root dotted key stays dotted (a [render] header
         * would be invalid TOML there) */
        const char *k6[] = { "render.filter" }, *v6[] = { "\"linear\"" };
        EXPECT("render.scale = 2\n[x]\ny = 1\n", k6, v6,
               "render.scale = 2\nrender.filter = \"linear\"\n[x]\ny = 1\n");
    }
    {
        /* no final newline */
        const char *k7[] = { "render.aniso" }, *v7[] = { "4" };
        EXPECT("[render]\nscale = 2", k7, v7, "[render]\nscale = 2\naniso = 4\n");
    }
}

static void test_remove(void)
{
    {
        const char *k[] = { "render.filter" }, *v[] = { NULL };
        EXPECT(s_file, k, v,
               "# my settings\n\n[render]\nscale = 2          # sharp enough\n"
               "\n# the pack\n[textures]\npack = \"auto\"\n");
    }
    {
        /* the last key of a bare section takes its header with it */
        const char *k[] = { "x.a", "x.b" }, *v[] = { NULL, NULL };
        EXPECT("[r]\ns = 1\n\n[x]\na = 1\nb = 2\n\n[z]\nq = 1\n", k, v,
               "[r]\ns = 1\n\n[z]\nq = 1\n");
        EXPECT("[r]\ns = 1\n[x]\na = 1\nb = 2\n", k, v, "[r]\ns = 1\n");
    }
    {
        /* ...but never a comment: the header stays with it */
        const char *k[] = { "textures.pack" }, *v[] = { NULL };
        EXPECT("[textures]\n# keep me\npack = \"auto\"\n", k, v, "[textures]\n# keep me\n");
        EXPECT("[textures] # mine\npack = \"auto\"\n", k, v, "[textures] # mine\n");
    }
    {
        /* removing what is not there changes nothing */
        const char *k[] = { "render.aniso" }, *v[] = { NULL };
        EXPECT(s_file, k, v, s_file);
        EXPECT("[r]\na = 1", k, v, "[r]\na = 1");
    }
    {
        /* remove one and add one in the same section: the header stays */
        const char *k[] = { "x.a", "x.c" }, *v[] = { NULL, "3" };
        EXPECT("[x]\na = 1\n", k, v, "[x]\nc = 3\n");
    }
}

static void test_actions_mods(void)
{
    {
        /* binding arrays, existing and new, with dotted action names */
        const char *k[] = { "input.actions.menu.open", "input.actions.overlay.stats" };
        const char *v[] = { "[\"F11\"]", "[\"F3\", \"guide+b\"]" };
        EXPECT("[input.actions]\n\"menu.open\" = [\"F10\",\n  \"guide+a\"]  # two\n", k, v,
               "[input.actions]\n\"menu.open\" = [\"F11\"]  # two\n"
               "\"overlay.stats\" = [\"F3\", \"guide+b\"]\n");
        EXPECT("", k, v,
               "[input.actions]\n\"menu.open\" = [\"F11\"]\n"
               "\"overlay.stats\" = [\"F3\", \"guide+b\"]\n");
    }
    {
        /* an empty array unbinds; removing the key goes back to defaults */
        const char *k[] = { "input.actions.menu.open" }, *v[] = { "[]" };
        EXPECT("[input.actions]\nmenu.open = [\"F10\"]\n", k, v,
               "[input.actions]\nmenu.open = []\n");
        v[0] = NULL;
        EXPECT("[input]\nkeyboard = true\n\n[input.actions]\n\"menu.open\" = [\"F10\"]\n", k, v,
               "[input]\nkeyboard = true\n\n");
    }
    {
        /* a mod's name is one key under [mods] */
        const char *k[] = { "mods.camera.free", "mods.hud" }, *v[] = { "false", "true" };
        EXPECT("[mods]\n\"camera.free\" = true\n", k, v,
               "[mods]\n\"camera.free\" = false\nhud = true\n");
        EXPECT("", k, v, "[mods]\n\"camera.free\" = false\nhud = true\n");
    }
}

/* Flat tables: a mod's switch and its settings side by side, captured
 * controls, keys the patch was not given, and a [mods.<name>] table. */
static int read_file(const char *p, char *buf, size_t n);
static void write_file(const char *p, const char *s);

static void test_flat(void)
{
    {
        const char *k[] = { "mods.alpha", "mods.alpha.speed" }, *v[] = { "false", "4" };
        EXPECT("[mods]\n\"alpha.speed\" = 3\n\"y.extra\" = 1  # not ours\n", k, v,
               "[mods]\n\"alpha.speed\" = 4\n\"y.extra\" = 1  # not ours\n"
               "alpha = false\n");
        EXPECT("", k, v, "[mods]\nalpha = false\n\"alpha.speed\" = 4\n");
        /* a bare dotted name under [mods] is the same flat key */
        EXPECT("[mods]\nalpha = true\nalpha.speed = 3\n", k, v,
               "[mods]\nalpha = false\nalpha.speed = 4\n");
    }
    {
        const char *k[] = { "input.controls.camera.free.look" }, *v[] = { "[\"mouse\", \"pad.rs\"]" };
        EXPECT("", k, v, "[input.controls]\n\"camera.free.look\" = [\"mouse\", \"pad.rs\"]\n");
        EXPECT("[input.controls]\n\"camera.free.look\" = [\"pad.ls\", \"mouse\"]\n"
               "\"camera.free\" = [\"x\"]\n", k, v,
               "[input.controls]\n\"camera.free.look\" = [\"mouse\", \"pad.rs\"]\n"
               "\"camera.free\" = [\"x\"]\n");
        v[0] = NULL;
        EXPECT("[input.controls]\n\"camera.free.look\" = [\"mouse\"]\n", k, v, "");
    }
    {
        /* [mods.alpha] makes alpha a table: writing the switch is refused
         * with a line that says where the keys go; its own keys still edit */
        const char *k[] = { "mods.alpha" }, *v[] = { "false" };
        char err[256];
        char *got = recomp_cfg_patch_text("[mods.alpha]\nspeed = 3\n", "t.toml", k, v, 1,
                                          err, sizeof err);
        /* left out and named; with nothing else, the text comes back as is */
        CHECK(got && !strcmp(got, "[mods.alpha]\nspeed = 3\n"));
        CHECK(strstr(err, "mods.alpha is the table [mods.alpha]") != NULL);
        CHECK(strstr(err, "flat under [mods]") != NULL);
        free(got);
        const char *k2[] = { "mods.alpha.speed" }, *v2[] = { "4" };
        EXPECT("[mods.alpha]\nspeed = 3\n", k2, v2, "[mods.alpha]\nspeed = 4\n");
    }
    {
        /* A mixed batch (a menu Save): only the refused keys are left
         * out, each named, and the rest are written (review F5). The
         * deeper table [mods.camera.free] refuses "camera" by its own
         * wording. */
        const char *k[] = { "render.scale", "mods.alpha", "mods.camera", "mods.beta" };
        const char *v[] = { "3", "false", "true", "false" };
        static const char *const in =
            "[render]\nscale = 2\n[mods.alpha]\nspeed = 3\n[mods.camera.free]\nspeed = 1\n";
        char err[512], buf[1024];
        char *got = recomp_cfg_patch_text(in, "t.toml", k, v, 4, err, sizeof err);
        CHECK(got && !strcmp(got,
              "[render]\nscale = 3\n[mods.alpha]\nspeed = 3\n[mods.camera.free]\nspeed = 1\n"
              "\n[mods]\nbeta = false\n"));
        CHECK(strstr(err, "t.toml: mods.alpha is the table [mods.alpha]") != NULL);
        /* the file is named once, then each key */
        CHECK(strstr(err, "; mods.camera cannot sit beside the table "
                          "[mods.camera.free]; write that table's keys flat under [mods]")
              != NULL);
        CHECK(strstr(err, "beta") == NULL && strstr(err, "render") == NULL);
        free(got);
        CHECK(strstr(err, "t.toml: mods.camera") == NULL);
        /* the file: written, and 2 says two keys were left out */
        remove("rcp_mix.toml");
        write_file("rcp_mix.toml", in);
        CHECK(recomp_cfg_patch("rcp_mix.toml", k, v, 4, err, sizeof err) == 2);
        CHECK(read_file("rcp_mix.toml", buf, sizeof buf) > 0);
        CHECK(strstr(buf, "scale = 3\n") != NULL && strstr(buf, "beta = false\n") != NULL);
        CHECK(strstr(err, "mods.alpha is the table") != NULL);
        /* only refused keys: nothing to write, still 1 */
        CHECK(recomp_cfg_patch("rcp_mix.toml", k + 1, v + 1, 1, err, sizeof err) == 1);
        /* An error buffer of no room or one byte (review N3): nothing is
         * written past it, and the count still says what was left out. */
        {
            char tiny[8];
            memset(tiny, 'x', sizeof tiny);
            got = recomp_cfg_patch_text(in, "t.toml", k, v, 4, tiny, 0);
            CHECK(got && strstr(got, "beta = false\n") != NULL);
            free(got);
            for (size_t j = 0; j < sizeof tiny; j++)
                CHECK(tiny[j] == 'x');
            got = recomp_cfg_patch_text(in, "t.toml", k, v, 4, tiny, 1);
            CHECK(got && tiny[0] == 0 && tiny[1] == 'x');
            free(got);
            got = recomp_cfg_patch_text(in, "t.toml", k, v, 4, NULL, 0);
            CHECK(got != NULL);
            free(got);
            write_file("rcp_mix.toml", in);
            memset(tiny, 'x', sizeof tiny);
            CHECK(recomp_cfg_patch("rcp_mix.toml", k, v, 4, tiny, 0) == 2);
            CHECK(tiny[0] == 'x');
            write_file("rcp_mix.toml", in);
            CHECK(recomp_cfg_patch("rcp_mix.toml", k, v, 4, tiny, 1) == 2);
            CHECK(tiny[0] == 0 && tiny[1] == 'x');
            write_file("rcp_mix.toml", in);
            CHECK(recomp_cfg_patch("rcp_mix.toml", k, v, 4, NULL, 0) == 2);
            CHECK(read_file("rcp_mix.toml", buf, sizeof buf) > 0);
            CHECK(strstr(buf, "beta = false\n") != NULL);
        }
        remove("rcp_mix.toml");
    }
}

static void test_array_comments(void)
{
    /* one binding per line, a note on each: the same count keeps both */
    static const char *const in =
        "[input.actions]\n"
        "\"menu.open\" = [\n"
        "  \"F10\",      # the keyboard\n"
        "  \"guide+a\",  # the pad, # in a comment\n"
        "]  # mine\n";
    const char *k[] = { "input.actions.menu.open" }, *v[] = { "[\"F11\", 'guide+b']" };
    EXPECT(in, k, v,
           "[input.actions]\n"
           "\"menu.open\" = [\n"
           "  \"F11\",      # the keyboard\n"
           "  'guide+b',  # the pad, # in a comment\n"
           "]  # mine\n");
    {
        /* a string holding '#' or ',' is one item, not a comment */
        const char *v2[] = { "[\"a\", \"b\"]" };
        EXPECT("[input.actions]\nx = [\"#,\", # c\n  \"]\"]\n", k, v2,
               "[input.actions]\nx = [\"#,\", # c\n  \"]\"]\n"
               "\"menu.open\" = [\"a\", \"b\"]\n");
        k[0] = "input.actions.x";
        EXPECT("[input.actions]\nx = [\"#,\", # c\n  \"]\"]\n", k, v2,
               "[input.actions]\nx = [\"a\", # c\n  \"b\"]\n");
        k[0] = "input.actions.menu.open";
    }
    {
        /* a new count: one line, the notes inside go (documented) */
        const char *v3[] = { "[\"F11\"]" };
        EXPECT(in, k, v3, "[input.actions]\n\"menu.open\" = [\"F11\"]  # mine\n");
    }
    {
        /* a note right above a removed key stays: it may be the table's */
        const char *kr[] = { "render.aniso" }, *vr[] = { NULL };
        EXPECT("[render]\nscale = 2\n# sharper\naniso = 8\n", kr, vr,
               "[render]\nscale = 2\n# sharper\n");
    }
}

static void test_crlf(void)
{
    const char *k[] = { "render.scale", "render.aniso", "overlay.stats" };
    const char *v[] = { "3", "8", "true" };
    EXPECT("# x\r\n[render]\r\nscale = 2\r\n", k, v,
           "# x\r\n[render]\r\nscale = 3\r\naniso = 8\r\n\r\n[overlay]\r\nstats = true\r\n");
}

static void test_refusals(void)
{
    const char *k[] = { "render.scale" }, *v[] = { "2" };
    /* a file that does not parse is left alone */
    EXPECT("[render\nscale = 2\n", k, v, NULL);
    EXPECT("[render]\nscale = 2\nscale = 3\n", k, v, NULL);
    {
        /* a value that is not one TOML value */
        const char *v2[] = { "2 3" };
        EXPECT(s_file, k, v2, NULL);
        const char *v3[] = { "\"open" };
        EXPECT(s_file, k, v3, NULL);
        const char *v4[] = { "1\nother = 2" };
        EXPECT(s_file, k, v4, NULL);
    }
    {
        /* a key given twice, an empty part, a key that is a table */
        const char *k2[] = { "render.scale", "render.scale" }, *v2[] = { "1", "2" };
        EXPECT(s_file, k2, v2, NULL);
        const char *k3[] = { "render..scale" };
        EXPECT(s_file, k3, v, NULL);
        const char *k4[] = { "render" };
        EXPECT(s_file, k4, v, NULL);
    }
}

static int read_file(const char *p, char *buf, size_t n)
{
    FILE *f = fopen(p, "rb");
    size_t got;
    if (!f)
        return -1;
    got = fread(buf, 1, n - 1, f);
    buf[got] = 0;
    fclose(f);
    return (int)got;
}

static void write_file(const char *p, const char *s)
{
    FILE *f = fopen(p, "wb");
    if (f) {
        fputs(s, f);
        fclose(f);
    }
}

static void test_file(void)
{
    char err[256], buf[1024];
    const char *k[] = { "render.scale", "mods.camera.free" }, *v[] = { "3", "false" };
    const char *kd[] = { "render.scale", "mods.camera.free" }, *vd[] = { NULL, NULL };
    FILE *f;

    remove("rcp_a.toml");
    remove("rcp_a.toml.tmp");
    /* a missing file is created with just the keys */
    CHECK(recomp_cfg_patch("rcp_a.toml", k, v, 2, err, sizeof err) == 0);
    CHECK(read_file("rcp_a.toml", buf, sizeof buf) > 0);
    CHECK(!strcmp(buf, "[render]\nscale = 3\n\n[mods]\n\"camera.free\" = false\n"));
    f = fopen("rcp_a.toml.tmp", "rb");
    CHECK(f == NULL);
    if (f)
        fclose(f);
    /* removing both leaves an empty file, not a missing one */
    CHECK(recomp_cfg_patch("rcp_a.toml", kd, vd, 2, err, sizeof err) == 0);
    CHECK(read_file("rcp_a.toml", buf, sizeof buf) == 0);
    /* removing from a missing file writes nothing */
    remove("rcp_a.toml");
    CHECK(recomp_cfg_patch("rcp_a.toml", kd, vd, 2, err, sizeof err) == 0);
    CHECK(read_file("rcp_a.toml", buf, sizeof buf) < 0);
    /* a broken file is refused and keeps its bytes */
    write_file("rcp_a.toml", "[render\n");
    CHECK(recomp_cfg_patch("rcp_a.toml", k, v, 2, err, sizeof err) == -1);
    CHECK(strstr(err, "rcp_a.toml:1:") != NULL);
    CHECK(read_file("rcp_a.toml", buf, sizeof buf) > 0 && !strcmp(buf, "[render\n"));
#ifndef _WIN32
    {
        /* a symlinked config is written at its target and stays a link */
        struct stat st;
        char lk[64];
        ssize_t ln;
        remove("rcp_link.toml");
        remove("rcp_target.toml");
        write_file("rcp_target.toml", "[render]\nscale = 2\n");
        CHECK(symlink("rcp_target.toml", "rcp_link.toml") == 0);
        CHECK(recomp_cfg_patch("rcp_link.toml", k, v, 1, err, sizeof err) == 0);
        CHECK(lstat("rcp_link.toml", &st) == 0 && S_ISLNK(st.st_mode));
        ln = readlink("rcp_link.toml", lk, sizeof lk - 1);
        CHECK(ln > 0 && (lk[ln] = 0, !strcmp(lk, "rcp_target.toml")));
        CHECK(read_file("rcp_target.toml", buf, sizeof buf) > 0 &&
              !strcmp(buf, "[render]\nscale = 3\n"));
        f = fopen("rcp_target.toml.tmp", "rb");
        CHECK(f == NULL);
        if (f)
            fclose(f);
        /* a dangling link is refused, and nothing appears at its target */
        remove("rcp_target.toml");
        CHECK(recomp_cfg_patch("rcp_link.toml", k, v, 1, err, sizeof err) == -1);
        CHECK(strstr(err, "cannot follow the link") != NULL);
        CHECK(lstat("rcp_link.toml", &st) == 0 && S_ISLNK(st.st_mode));
        CHECK(read_file("rcp_target.toml", buf, sizeof buf) < 0);
        remove("rcp_link.toml");
    }
#endif
    remove("rcp_a.toml");
}

static void test_quote(void)
{
    char b[64];
    CHECK(recomp_cfg_quote("a\"b\\c", b, sizeof b) && !strcmp(b, "\"a\\\"b\\\\c\""));
    CHECK(recomp_cfg_quote("\t", b, sizeof b) && !strcmp(b, "\"\\u0009\""));
    CHECK(!recomp_cfg_quote("too long", b, 4) && b[0] == 0);
}

int main(void)
{
    test_replace();
    test_add();
    test_remove();
    test_actions_mods();
    test_flat();
    test_array_comments();
    test_crlf();
    test_refusals();
    test_file();
    test_quote();
    printf("recomp_cfg_patch: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
