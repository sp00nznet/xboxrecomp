/*
 * enhance_cfg test: the TOML subset (recomp_cfg.c), its errors with line
 * numbers, the env > title file > user file > config.default.toml > default
 * layering with the enhance.toml migration and the header (enhance_cfg.c),
 * xbox_enhance_init (enhance.c) and recomp_exe_dir. Runs in its working
 * directory, where it writes small files under ecfg_*; config.default.toml
 * and the pack go beside the test's executable, where the layer looks for
 * them, and are removed afterwards. The textures.pack cases pin the
 * [ENHANCE] lines the golden check reads (xboxrecomp-cli golden.py,
 * ENHANCE_PACK_RE). The [input] cases drive enhance_input.c into src/input's
 * binding table and read it back.
 */
#include "enhance.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "host_pad.h"
#include "input_names.h"
#include "keyboard.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_tex_pack.h"
#include "pad_map.h"
#include "recomp_cfg.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <fcntl.h>
#ifdef _WIN32
#include <direct.h>
#include <io.h>
#define MKDIR(p) _mkdir(p)
#define RMDIR(p) _rmdir(p)
#define GETCWD(b, n) _getcwd(b, (int)(n))
#define DUP _dup
#define DUP2 _dup2
#define CLOSE _close
#define OPEN_TRUNC(p) _open(p, _O_WRONLY | _O_CREAT | _O_TRUNC, 0644)
#else
#include <sys/stat.h>
#include <unistd.h>
#define MKDIR(p) mkdir(p, 0755)
#define RMDIR(p) rmdir(p)
#define GETCWD(b, n) getcwd(b, n)
#define DUP dup
#define DUP2 dup2
#define CLOSE close
#define OPEN_TRUNC(p) open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644)
#endif

static int s_fail, s_checks;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fputs(text, f);
    fclose(f);
}


/* config.default.toml beside this executable, where the layer reads it. */
static const char *defaults_file(void)
{
    static char p[4096];
    snprintf(p, sizeof p, "%s/config.default.toml", recomp_exe_dir());
    return p;
}

static void remove_defaults(void)
{
    remove(defaults_file());
}

static int exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fclose(f);
    return 1;
}

/* A whole small file, or "" when absent. */
static const char *read_file(const char *path)
{
    static char buf[4096];
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    buf[0] = 0;
    if (f) {
        n = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
    }
    buf[n] = 0;
    return buf;
}

/* stderr to a file while a call runs, then its text. */
static int s_saved_fd = -1;
static char s_cap[4096];

static void capture_begin(void)
{
    int fd;
    fflush(stderr);
    s_saved_fd = DUP(2);
    fd = OPEN_TRUNC("ecfg_stderr.txt");
    if (s_saved_fd < 0 || fd < 0) { fprintf(stderr, "cannot capture stderr\n"); exit(2); }
    DUP2(fd, 2);
    CLOSE(fd);
}

static const char *capture_end(void)
{
    FILE *f;
    size_t n = 0;
    fflush(stderr);
    DUP2(s_saved_fd, 2);
    CLOSE(s_saved_fd);
    s_cap[0] = 0;
    f = fopen("ecfg_stderr.txt", "rb");
    if (f) {
        n = fread(s_cap, 1, sizeof s_cap - 1, f);
        fclose(f);
    }
    s_cap[n] = 0;
    {
        /* Windows' text-mode stderr writes "\r\n"; the lines compare as "\n". */
        char *r = s_cap, *w = s_cap;
        for (; *r; r++)
            if (*r != '\r')
                *w++ = *r;
        *w = 0;
    }
    return s_cap;
}

/* ── the subset ────────────────────────────────────────────────────── */

/* [[name]] arrays of tables: refused by the config loader, taken by
 * recomp_cfg_load_string_aot (embedded data: a HUD table, hud_anchor.h). */
static void test_aot(void)
{
    static const char text[] =
        "version = 1\n"
        "[canvas]\nw = 640\n"
        "[[site]]\nret_va = 0x2000\nelement = \"score\"\n"
        "[[group]]\nname = \"g\"\n"
        "[[site]]\nret_va = 0x2010\nnote.x = 3\n";
    static const char *const bad[] = {
        "[a]\nx = 1\n[[a]]\ny = 2\n",          /* a table, then an array */
        "[[a]]\nx = 1\n[a]\ny = 2\n",          /* the other way round */
        "[[a]]\nx = 1\n[a.b]\ny = 2\n",        /* a table inside one */
        "a = 1\n[[a]]\nx = 1\n",                /* a value */
        "b.c = 1\n[[b]]\nx = 1\n",              /* a dotted key's table */
        "[[a]\nx = 1\n",                         /* unclosed */
    };
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "aot", err, sizeof err);

    CHECK(c == NULL && strstr(err, "arrays of tables are not supported"));
    c = recomp_cfg_load_string_aot(text, "aot", err, sizeof err);
    CHECK(c != NULL);
    if (!c) {
        fprintf(stderr, "aot: %s\n", err);
        return;
    }
    CHECK(recomp_cfg_table_count(c, "site") == 2);
    CHECK(recomp_cfg_table_count(c, "group") == 1);
    CHECK(recomp_cfg_table_count(c, "canvas") == 0);
    CHECK(recomp_cfg_table_count(c, "none") == 0);
    CHECK(recomp_cfg_int(c, "site.0.ret_va", 0) == 0x2000);
    CHECK(!strcmp(recomp_cfg_string(c, "site.0.element", ""), "score"));
    CHECK(recomp_cfg_int(c, "site.1.ret_va", 0) == 0x2010);
    CHECK(recomp_cfg_int(c, "site.1.note.x", 0) == 3);
    CHECK(!strcmp(recomp_cfg_string(c, "group.0.name", ""), "g"));
    CHECK(recomp_cfg_int(c, "canvas.w", 0) == 640);
    CHECK(recomp_cfg_type_of(c, "site.0.note") == RCFG_NONE);
    recomp_cfg_free(c);
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        c = recomp_cfg_load_string_aot(bad[i], "aot", err, sizeof err);
        if (c)
            fprintf(stderr, "aot: bad text %zu accepted\n", i);
        CHECK(c == NULL);
        recomp_cfg_free(c);
    }
    CHECK(recomp_cfg_table_count(NULL, "site") == 0);
}

static void test_subset(void)
{
    static const char text[] =
        "\xEF\xBB\xBF# enhancements\n"
        "title = \"Example\"   # trailing comment\n"
        "\n"
        "[render]\n"
        "scale = 2\n"
        "big = 1_000_000\n"
        "neg = -42\n"
        "hex = 0xFF_FF\n"
        "oct = 0o17\n"
        "bin = 0b101\n"
        "sharp = 0.5\n"
        "exp = 1e3\n"
        "nexp = -2.5E-2\n"
        "huge = inf\n"
        "notnum = nan\n"
        "filter = 'C:\\path\\raw'\n"
        "esc = \"a\\tb\\n\\\"q\\\" \\u00e9\"\n"
        "\"quoted-key\" = true\n"
        "vsync = false\n"
        "\r\n"
        "[game]\n"
        "mode = \"fast\"\n"
        "\n"
        "[aspect.program]\n"
        "a1b2c3 = \"hud\"\n"
        "\n"
        "[display]\n"
        "aspect = \"16:9\"\n"
        "sizes = [1, 2, 3]\n"
        "mixed = [ \"x\", 2.5, true, ]  # trailing comma\n"
        "multi = [\n"
        "  \"one\",   # first\n"
        "  \"two\"\n"
        "]\n"
        "empty = []\n"
        "dotted.inner = 7\n";
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "subset", err, sizeof err);
    static const char *const modes[] = { "stock", "fast", "free", NULL };

    CHECK(c != NULL);
    if (!c) { fprintf(stderr, "%s\n", err); return; }
    CHECK(!strcmp(recomp_cfg_string(c, "title", ""), "Example"));
    CHECK(recomp_cfg_int(c, "render.scale", 0) == 2);
    CHECK(recomp_cfg_int(c, "render.big", 0) == 1000000);
    CHECK(recomp_cfg_int(c, "render.neg", 0) == -42);
    CHECK(recomp_cfg_int(c, "render.hex", 0) == 0xFFFF);
    CHECK(recomp_cfg_int(c, "render.oct", 0) == 15);
    CHECK(recomp_cfg_int(c, "render.bin", 0) == 5);
    CHECK(recomp_cfg_float(c, "render.sharp", 0) == 0.5);
    CHECK(recomp_cfg_float(c, "render.exp", 0) == 1000.0);
    CHECK(fabs(recomp_cfg_float(c, "render.nexp", 0) + 0.025) < 1e-12);
    CHECK(isinf(recomp_cfg_float(c, "render.huge", 0)));
    CHECK(isnan(recomp_cfg_float(c, "render.notnum", 0)));
    CHECK(recomp_cfg_float(c, "render.scale", 0) == 2.0);       /* int as float */
    CHECK(!strcmp(recomp_cfg_string(c, "render.filter", ""), "C:\\path\\raw"));
    CHECK(!strcmp(recomp_cfg_string(c, "render.esc", ""), "a\tb\n\"q\" \xC3\xA9"));
    CHECK(recomp_cfg_bool(c, "render.quoted-key", 0) == 1);
    CHECK(recomp_cfg_bool(c, "render.vsync", 1) == 0);
    CHECK(recomp_cfg_choice(c, "game.mode", modes, -1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "aspect.program.a1b2c3", ""), "hud"));
    CHECK(!strcmp(recomp_cfg_string(c, "display.aspect", ""), "16:9"));
    CHECK(recomp_cfg_int(c, "display.dotted.inner", 0) == 7);

    /* defaults: missing key, wrong type, NULL config */
    CHECK(recomp_cfg_int(c, "render.missing", 9) == 9);
    CHECK(recomp_cfg_int(c, "render.sharp", 9) == 9);
    CHECK(recomp_cfg_bool(c, "render.scale", 1) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "render.scale", "d"), "d"));
    CHECK(recomp_cfg_type_of(c, "render") == RCFG_NONE);
    CHECK(recomp_cfg_int(NULL, "render.scale", 3) == 3);
    CHECK(recomp_cfg_choice(c, "display.aspect", modes, 0) == 0);

    /* arrays */
    CHECK(recomp_cfg_type_of(c, "display.sizes") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.sizes") == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 2, 0) == 3);
    CHECK(recomp_cfg_array_int(c, "display.sizes", 3, -1) == -1);
    CHECK(recomp_cfg_array_len(c, "display.mixed") == 3);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.mixed", 0, ""), "x"));
    CHECK(recomp_cfg_array_float(c, "display.mixed", 1, 0) == 2.5);
    CHECK(recomp_cfg_array_bool(c, "display.mixed", 2, 0) == 1);
    CHECK(recomp_cfg_array_type(c, "display.mixed", 1) == RCFG_FLOAT);
    CHECK(recomp_cfg_array_len(c, "display.multi") == 2);
    CHECK(!strcmp(recomp_cfg_array_string(c, "display.multi", 1, ""), "two"));
    CHECK(recomp_cfg_type_of(c, "display.empty") == RCFG_ARRAY);
    CHECK(recomp_cfg_array_len(c, "display.empty") == 0);
    CHECK(recomp_cfg_array_len(c, "render.scale") == 0);

    /* iteration and lines */
    CHECK(!strcmp(recomp_cfg_key_at(c, 0), "title"));
    CHECK(recomp_cfg_line(c, "render.scale") == 5);
    CHECK(recomp_cfg_line(c, "game.mode") == 22);
    CHECK(recomp_cfg_line(c, "display.dotted.inner") == 36);
    CHECK(recomp_cfg_find(c, "nope") == -1);
    recomp_cfg_free(c);

    c = recomp_cfg_load_string("", "empty", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
    c = recomp_cfg_load_string("# only a comment", "comment", err, sizeof err);
    CHECK(c && recomp_cfg_count(c) == 0);
    recomp_cfg_free(c);
}

/* ── errors carry the line ─────────────────────────────────────────── */

static void expect_error(const char *text, const char *want, int line)
{
    char err[256], prefix[32];
    recomp_cfg *c = recomp_cfg_load_string(text, "e", err, sizeof err);
    s_checks++;
    snprintf(prefix, sizeof prefix, "e:%d: ", line);
    if (c || strncmp(err, prefix, strlen(prefix)) || !strstr(err, want)) {
        s_fail++;
        fprintf(stderr, "FAIL error for %s\n  want \"%s%s\"\n  got  \"%s\"\n",
                text, prefix, want, c ? "(parsed)" : err);
    }
    recomp_cfg_free(c);
}

static void test_errors(void)
{
    expect_error("a = 1\nb = \n", "expected a value", 2);
    expect_error("a = 1\na = 2\n", "duplicate key 'a' (first set on line 1)", 2);
    expect_error("[r]\nx = 1\n[r]\n", "table [r] defined twice", 3);
    expect_error("x = 1\n[x]\n", "'x' is a value", 2);
    expect_error("x = 1\nx.y = 2\n", "'x' is a value", 2);
    expect_error("[t.a]\nx = 1\n[t]\na = 2\n", "'t.a' is a table", 4);
    expect_error("x.y = 1\nx = 2\n", "'x' is a table", 2);
    expect_error("s = \"open\n", "unterminated string", 1);
    expect_error("\n\ns = \"\"\"multi\"\"\"\n", "multi-line strings", 3);
    expect_error("t = {a = 1}\n", "inline tables", 1);
    expect_error("[[arr]]\n", "arrays of tables", 1);
    expect_error("a = [[1]]\n", "nested arrays", 1);
    expect_error("d = 1979-05-27T07:32:00\n", "dates and times", 1);
    expect_error("f = linear\n", "strings need quotes", 1);
    expect_error("n = 012\n", "leading zero", 1);
    expect_error("n = 1__0\n", "misplaced '_'", 1);
    expect_error("n = 1.\n", "invalid number", 1);
    expect_error("n = 99999999999999999999\n", "out of range", 1);
    expect_error("a = 1 2\n", "expected end of line", 1);
    expect_error("a 1\n", "expected '='", 1);
    expect_error("= 1\n", "expected a key", 1);
    expect_error("[render\n", "expected ']'", 1);
    expect_error("a = [1,\n2\n", "unterminated array", 3);
    expect_error("a = [1 2]\n", "expected ',' or ']'", 1);
    expect_error("s = \"bad \\q\"\n", "bad escape", 1);
    expect_error("b = True\n", "strings need quotes", 1);

    /* strict pass (review of 409a8e8): each was accepted before */
    /* 1. '_' only between two digits */
    expect_error("n = 0x_FF\n", "misplaced '_'", 1);
    expect_error("n = 1_e5\n", "misplaced '_'", 1);
    expect_error("n = 1e_5\n", "misplaced '_'", 1);
    expect_error("n = 1_.5\n", "misplaced '_'", 1);
    expect_error("n = 1._5\n", "misplaced '_'", 1);
    expect_error("n = i_nf\n", "strings need quotes", 1);
    expect_error("n = 0_x10\n", "misplaced '_'", 1);
    expect_error("n = 0xFF_\n", "misplaced '_'", 1);
    expect_error("n = 0_1\n", "leading zero", 1);
    /* 2. one prefix, digits of its base only */
    expect_error("n = 0x0x1F\n", "invalid number", 1);
    expect_error("n = 0x\n", "invalid number", 1);
    expect_error("n = 0o8\n", "invalid number", 1);
    expect_error("n = 0b102\n", "invalid number", 1);
    expect_error("n = 0x-1\n", "invalid number", 1);
    expect_error("n = 1e\n", "invalid number", 1);
    expect_error("n = 1.5.2\n", "invalid number", 1);
    expect_error("n = 1e5e5\n", "invalid number", 1);
    expect_error("d = 1979-05-27\n", "dates and times", 1);
    /* 3. a table made by a dotted key cannot be reopened by a header, nor a
     *    header's table extended by a dotted key */
    expect_error("a.b = 1\n[a]\n", "defined by a dotted key", 2);
    expect_error("[t]\na.x = 1\n[t.a]\n", "defined by a dotted key", 3);
    expect_error("[a.b]\nx = 1\n[a]\nb.y = 2\n", "defined by a header", 4);
    /* 4. \e is TOML 1.1 only */
    expect_error("s = \"\\e[0m\"\n", "bad escape", 1);
    /* 6. DEL, control characters and invalid UTF-8 */
    expect_error("x = 1\ns = \"a\x7f\"\n", "control character in string", 2);
    expect_error("x = 1\n# bell \x07\n", "control character in comment", 2);
    expect_error("x = 1\n# del \x7f\n", "control character in comment", 2);
    expect_error("# cr \r alone\n", "control character in comment", 1);
    expect_error("x = 1\n\ns = \"\xff\"\n", "invalid UTF-8", 3);
    expect_error("s = \"\xc0\xaf\"\n", "invalid UTF-8", 1);          /* overlong '/' */
    expect_error("s = \"\xed\xa0\x80\"\n", "invalid UTF-8", 1);      /* surrogate */
    expect_error("s = \"\xf4\x90\x80\x80\"\n", "invalid UTF-8", 1); /* > U+10FFFF */
    expect_error("# \xe2\x82\n", "invalid UTF-8", 1);                  /* truncated */
    {
        char err[256];
        CHECK(recomp_cfg_load_file("ecfg_does_not_exist.toml", err, sizeof err) == NULL);
        CHECK(strstr(err, "cannot open") != NULL);
    }
}

/* A quoted key with '.' in it: only as the last part directly under a flat
 * table ([input.actions], whose keys are dotted action names), where it is
 * the same entry as the bare dotted key. */
static void test_quoted_dots(void)
{
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(
        "[input.actions]\n\"menu.open\" = [\"F10\"]\n'overlay.stats' = \"F11\"\n"
        "shot.window = \"F12\"\n", "q", err, sizeof err);
    CHECK(c != NULL);
    if (c) {
        CHECK(recomp_cfg_type_of(c, "input.actions.menu.open") == RCFG_ARRAY);
        CHECK(!strcmp(recomp_cfg_string(c, "input.actions.overlay.stats", ""), "F11"));
        CHECK(!strcmp(recomp_cfg_string(c, "input.actions.shot.window", ""), "F12"));
        recomp_cfg_free(c);
    }
    c = recomp_cfg_load_string("[input]\nactions.\"menu.open\" = \"F10\"\n", "q",
                               err, sizeof err);
    CHECK(c && !strcmp(recomp_cfg_string(c, "input.actions.menu.open", ""), "F10"));
    recomp_cfg_free(c);
    /* The quoted and the bare spelling are one key. */
    expect_error("[input.actions]\n\"menu.open\" = \"F10\"\nmenu.open = \"F9\"\n",
                 "duplicate key 'input.actions.menu.open'", 3);
    /* Under a flat table x.y beside x.y.z are two names (a mod's switch
     * and its setting); anywhere else they are a value and a table of it. */
    c = recomp_cfg_load_string("[mods]\n\"camera.free\" = false\ncamera.free.speed = 2\n",
                               "q", err, sizeof err);
    CHECK(c && recomp_cfg_bool(c, "mods.camera.free", 1) == 0 &&
          recomp_cfg_int(c, "mods.camera.free.speed", 0) == 2);
    recomp_cfg_free(c);
    expect_error("[render]\nscale = 1\nscale.x = 2\n", "'render.scale' is a value", 3);
    expect_error("[mods.alpha]\nspeed = 3\n[mods]\nalpha = true\n", "'mods.alpha' is a table", 4);
    /* Anywhere else, and not as a middle part. */
    expect_error("[render]\n\"a.b\" = 1\n", "'.' inside a quoted key", 2);
    expect_error("\"input.actions\".x = 1\n", "'.' inside a quoted key", 1);
    expect_error("[\"input.actions\"]\n", "'.' inside a quoted key", 1);
    expect_error("[input.actions]\n\"menu.open\".x = 1\n", "must be the last part", 2);
    expect_error("[input.actions.sub]\n\"a.b\" = 1\n", "'.' inside a quoted key", 2);
}

/* What the strict pass must still accept. */
static void test_strict_valid(void)
{
    static const char text[] =
        "hex = 0xdead_BEEF\n"
        "big = 1_000.000_5e1_0\n"
        "zero = 0\nzf = 0.0\nzexp = 0e0\nneg = -0.5\npos = +1.5\n"
        "a.b = 1\n"
        "a.c = 2\n"                      /* a dotted table extended by dotted keys */
        "[x.y.z]\nk = 1\n"
        "[x]\nv = 2\n"                  /* implicit (header) table, defined later */
        "[p]\nq.r = 1\n"
        "[p.q.s]\nt = 1\n"              /* sub-table of a dotted-defined table */
        "u = \"caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae\"  # \xc3\xa9 ok\n";
    char err[256];
    recomp_cfg *c = recomp_cfg_load_string(text, "valid", err, sizeof err);
    double d;
    CHECK(c != NULL);
    if (!c) { fprintf(stderr, "%s\n", err); return; }
    CHECK(recomp_cfg_int(c, "hex", 0) == 0xdeadbeefLL);
    CHECK(recomp_cfg_float(c, "big", 0) == 1000.0005e10);
    CHECK(recomp_cfg_int(c, "zero", 1) == 0);
    CHECK(recomp_cfg_float(c, "zexp", 1) == 0.0);
    CHECK(recomp_cfg_float(c, "neg", 0) == -0.5);
    CHECK(recomp_cfg_float(c, "pos", 0) == 1.5);
    CHECK(recomp_cfg_int(c, "a.c", 0) == 2);
    CHECK(recomp_cfg_int(c, "x.v", 0) == 2);
    CHECK(recomp_cfg_int(c, "p.q.s.t", 0) == 1);
    CHECK(!strcmp(recomp_cfg_string(c, "p.q.s.u", ""), "caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x8e\xae"));
    recomp_cfg_free(c);

    CHECK(recomp_cfg_parse_double("2.5", &d) && d == 2.5);
    CHECK(recomp_cfg_parse_double("-inf", &d) && isinf(d) && d < 0);
    CHECK(!recomp_cfg_parse_double("2,5", &d));
    CHECK(!recomp_cfg_parse_double("0x1p3", &d));
    CHECK(!recomp_cfg_parse_double(" 1", &d));
    CHECK(!recomp_cfg_parse_double("", &d));
}

/* 5. Floats do not depend on the locale (a comma-decimal locale used to
 *    make strtod stop at the '.'). Skipped when no such locale exists. */
static void test_locale(void)
{
    static const char *const names[] = { "de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8",
                                         "fr_FR.utf8", "German_Germany.1252", NULL };
    int i;
    const char *got = NULL;
    for (i = 0; names[i] && !got; i++)
        got = setlocale(LC_NUMERIC, names[i]);
    if (!got || strcmp(localeconv()->decimal_point, ",") != 0) {
        printf("enhance_cfg: no comma-decimal locale here; locale check skipped\n");
        setlocale(LC_NUMERIC, "C");
        return;
    }
    {
        char err[256];
        double d = 0;
        recomp_cfg *c = recomp_cfg_load_string("f = 0.25\ng = 1.5e2\n", "loc", err, sizeof err);
        CHECK(c && recomp_cfg_float(c, "f", 0) == 0.25);
        CHECK(c && recomp_cfg_float(c, "g", 0) == 150.0);
        recomp_cfg_free(c);
        CHECK(recomp_cfg_parse_double("0.75", &d) && d == 0.75);
        CHECK(!recomp_cfg_parse_double("0,75", &d));
        recomp_env_set(RENV_RENDER_SCALE, "1.5");
        CHECK(enhance_cfg_float("render.scale", 0) == 1.5);
        recomp_env_set(RENV_RENDER_SCALE, NULL);
        printf("enhance_cfg: locale check ran under %s\n", got);
    }
    setlocale(LC_NUMERIC, "C");
}

/* ── env > title file > user file > default ────────────────────────── */

static void test_layers(void)
{
    static const char *const modes[] = { "stock", "fast", "free", NULL };
    static const char *const aspect[] = { "4:3", "16:9", "16:10", "21:9", NULL };

    remove_defaults();
    MKDIR("ecfg_root");
    MKDIR("ecfg_root/config");
    MKDIR("ecfg_root/TITLE");
    write_file("ecfg_root/config/config.toml",
               "[render]\nscale = 3\nfilter = \"nearest\"\n"
               "[display]\naspect = \"16:10\"\n"
               "[game]\nmode = \"free\"\n"
               "[extra]\nunread = 1\n");
    write_file("ecfg_root/TITLE/config.toml",
               "[render]\nscale = 2\nsharp = 0.25\nvsync = true\nmodes = [1, 2]\n"
               "[game]\nmode = \"fast\"\n"
               "[display]\nbadtype = 5\n");

    recomp_env_set(RENV_CONFIG, NULL);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 2);
    /* title file over root file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    /* root file where the title file is silent */
    CHECK(!strcmp(enhance_cfg_string("render.filter", "linear"), "nearest"));
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 2);
    /* default when neither has it */
    CHECK(enhance_cfg_int("render.missing", 7) == 7);
    CHECK(enhance_cfg_source_of("render.missing") == ENHANCE_SRC_DEFAULT);
    CHECK(enhance_cfg_float("render.sharp", 1.0) == 0.25);
    CHECK(enhance_cfg_float("render.scale", 1.0) == 2.0);
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    /* wrong type in the file: the default, not a coercion */
    CHECK(!strcmp(enhance_cfg_string("display.badtype", "dflt"), "dflt"));
    {
        const recomp_cfg *c = enhance_cfg_lookup("render.modes");
        CHECK(c == enhance_cfg_file(0));
        CHECK(recomp_cfg_array_len(c, "render.modes") == 2);
        CHECK(enhance_cfg_lookup("nope") == NULL);
    }

    /* game.mode stands for a game key: unbound, the file answers; bound, env wins. */
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 1);
    CHECK(enhance_cfg_bind_env("game.mode", RENV_LOG_LEVEL));   /* any config id */
    recomp_env_set(RENV_LOG_LEVEL, "free");
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 2);
    CHECK(enhance_cfg_source_of("game.mode") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_LOG_LEVEL, "bogus");                    /* invalid: file */
    CHECK(enhance_cfg_choice("game.mode", modes, 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* env over both files */
    recomp_env_set(RENV_RENDER_SCALE, "4");
    CHECK(enhance_cfg_int("render.scale", 1) == 4);
    CHECK(enhance_cfg_float("render.scale", 1) == 4.0);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_RENDER_SCALE, "0x2");
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "two");                   /* not a number: file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, "");                      /* empty = unset */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, "21:9");
    CHECK(enhance_cfg_choice("display.aspect", aspect, 0) == 3);
    CHECK(!strcmp(enhance_cfg_string("display.aspect", ""), "21:9"));
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);

    /* bool spellings from env */
    CHECK(enhance_cfg_bind_env("render.vsync", RENV_LOG_LEVEL));
    recomp_env_set(RENV_LOG_LEVEL, "Off");
    CHECK(enhance_cfg_bool("render.vsync", 1) == 0);
    recomp_env_set(RENV_LOG_LEVEL, "yes");
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, "maybe");                    /* file: true */
    CHECK(enhance_cfg_bool("render.vsync", 0) == 1);
    recomp_env_set(RENV_LOG_LEVEL, NULL);

    /* only extra.unread was never read */
    CHECK(enhance_cfg_report_unused() == 1);

    /* real environment, read through recomp_env */
    set_env("RECOMP_RENDER_SCALE", "5");
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 5);
    set_env("RECOMP_RENDER_SCALE", NULL);
    recomp_env_reload();
    CHECK(enhance_cfg_int("render.scale", 1) == 2);

    /* RECOMP_CONFIG: one file, or none */
    recomp_env_set(RENV_CONFIG, "ecfg_root/config/config.toml");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    recomp_env_set(RENV_CONFIG, "none");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == 0);
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    CHECK(enhance_cfg_file(0) == NULL && enhance_cfg_file(1) == NULL);
    recomp_env_set(RENV_RENDER_SCALE, "2");                     /* env with no file */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_CONFIG, "ecfg_no_such_dir/missing.toml");     /* no dir: no header */
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    recomp_env_set(RENV_CONFIG, NULL);

    /* no title file: root only; a broken title file is skipped whole */
    CHECK(enhance_cfg_init("ecfg_root", "OTHER") == 1);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    write_file("ecfg_root/TITLE/config.toml", "[render]\nscale = 2\nscale = 3\n");
    CHECK(enhance_cfg_init("ecfg_root", "TITLE") == -1);
    CHECK(enhance_cfg_file(0) == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    enhance_cfg_shutdown();
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
}

/* ── config.default.toml, config.toml, migration and the header ────── */

/* Is `path` an existing directory? */
static int is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static void config_reset_env(void)
{
    recomp_env_set(RENV_CONFIG, NULL);
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_PRESENT_FILTER, NULL);
}

static void test_config_files(void)
{
    static const char defaults[] =
        "[render]\nscale = 2\n[present]\nfilter = \"integer\"\n[extra]\nunread = 1\n";
    static const char *const filters[] = { "nearest", "linear", "integer", NULL };
    char want[4400];
    const char *out;
    const recomp_cfg *d;

    config_reset_env();
    remove_defaults();
    /* A rerun starts from no user directory. */
    remove("ecfg_lay/config/config.toml");
    RMDIR("ecfg_lay/config");

    /* The defaults file alone: no user directory, so nothing is created
     * and the save path is where the file would be. */
    MKDIR("ecfg_lay");
    write_file(defaults_file(), defaults);
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 1);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] config: %s (3 keys)\n", defaults_file());
    CHECK(strstr(out, want) != NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_DEFAULTS);
    CHECK(enhance_cfg_file(2) != NULL && enhance_cfg_file(2) == enhance_cfg_defaults());
    CHECK(enhance_cfg_file(0) == NULL && enhance_cfg_file(1) == NULL);
    CHECK(!is_dir("ecfg_lay/config") && !exists("ecfg_lay/config.toml"));
    CHECK(enhance_cfg_user_path() && !strcmp(enhance_cfg_user_path(), "ecfg_lay/config/config.toml"));
    /* What a writer compares against. */
    d = enhance_cfg_defaults();
    CHECK(recomp_cfg_int(d, "render.scale", 0) == 2);
    CHECK(!strcmp(recomp_cfg_string(d, "present.filter", ""), "integer"));
    CHECK(recomp_cfg_find(d, "render.missing") < 0);

    /* config/ exists: the user file is created as the header, 0 keys. */
    MKDIR("ecfg_lay/config");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 2);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] config: %s (3 keys), ecfg_lay/config/config.toml (0 keys)\n",
             defaults_file());
    CHECK(strstr(out, want) != NULL);
    CHECK(!strncmp(read_file("ecfg_lay/config/config.toml"), "# Your settings: only the keys", 30));
    CHECK(strstr(read_file("ecfg_lay/config/config.toml"), "config.default.toml") != NULL);
    CHECK(enhance_cfg_file(1) && recomp_cfg_count(enhance_cfg_file(1)) == 0);
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_lay/config/config.toml"));

    /* The user file over the defaults; a defaults key it does not name
     * still answers. One key, so "1 key". */
    write_file("ecfg_lay/config/config.toml", "[render]\nscale = 3\n");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 2);
    out = capture_end();
    CHECK(strstr(out, "ecfg_lay/config/config.toml (1 key)\n") != NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    /* Unused: the defaults' extra.unread and present.filter, not the
     * render.scale the user file overrides. */
    capture_begin();
    CHECK(enhance_cfg_report_unused() == 2);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] unused key extra.unread, %s:6\n", defaults_file());
    CHECK(strstr(out, want) != NULL);
    CHECK(strstr(out, "unused key render.scale") == NULL);
    CHECK(enhance_cfg_choice("present.filter", filters, 0) == 2);
    CHECK(enhance_cfg_source_of("present.filter") == ENHANCE_SRC_DEFAULTS);

    /* The writer's comparison: the user file holds the defaults' value for
     * one key; it is read as any other (no equality judged on read). */
    write_file("ecfg_lay/config/config.toml", "[render]\nscale = 2\n");
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 2);
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    write_file("ecfg_lay/config/config.toml", "[render]\nscale = 3\n");

    /* Title over user over defaults; the save path is the title file. */
    MKDIR("ecfg_lay/T");
    write_file("ecfg_lay/T/config.toml", "[render]\nscale = 4\n");
    CHECK(enhance_cfg_init("ecfg_lay", "T") == 3);
    CHECK(enhance_cfg_int("render.scale", 1) == 4);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_FILE);
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_lay/T/config.toml"));
    CHECK(enhance_cfg_lookup("render.scale") == enhance_cfg_file(0));
    /* An absent title file in an existing title directory gets the header
     * (the file Save targets), and the user file answers. */
    remove("ecfg_lay/T/config.toml");
    CHECK(enhance_cfg_init("ecfg_lay", "T") == 3);
    CHECK(!strncmp(read_file("ecfg_lay/T/config.toml"), "# T settings: only the keys", 27));
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_lay/T/config.toml"));
    /* A title with no directory: the user file is the save path. */
    CHECK(enhance_cfg_init("ecfg_lay", "NODIR") == 2);
    CHECK(!is_dir("ecfg_lay/NODIR"));
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_lay/config/config.toml"));

    /* The environment over every file, with no deprecation line. */
    recomp_env_set(RENV_RENDER_SCALE, "1");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", "T") == 3);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] render.scale = 1 from RECOMP_RENDER_SCALE\n") != NULL);
    CHECK(strstr(out, "deprecated") == NULL);
    CHECK(enhance_cfg_int("render.scale", 4) == 1);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_ENV);
    recomp_env_set(RENV_RENDER_SCALE, NULL);

    /* RECOMP_CONFIG=<path>: that file replaces the title and user files,
     * the defaults stay under it. */
    write_file("ecfg_lay/mine.toml", "[render]\nscale = 4\n");
    recomp_env_set(RENV_CONFIG, "ecfg_lay/mine.toml");
    CHECK(enhance_cfg_init("ecfg_lay", "T") == 2);
    CHECK(enhance_cfg_file(0) == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 4);
    CHECK(enhance_cfg_choice("present.filter", filters, 0) == 2);
    CHECK(enhance_cfg_source_of("present.filter") == ENHANCE_SRC_DEFAULTS);
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_lay/mine.toml"));
    /* ... and is created as the header when absent in an existing
     * directory, as a launcher's <data>/config/config.toml is. */
    MKDIR("ecfg_fresh");
    MKDIR("ecfg_fresh/config");
    remove("ecfg_fresh/config/config.toml");
    remove("ecfg_fresh/config/enhance.toml");
    recomp_env_set(RENV_CONFIG, "ecfg_fresh/config/config.toml");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 2);
    out = capture_end();
    CHECK(strstr(out, "ecfg_fresh/config/config.toml (0 keys)\n") != NULL);
    CHECK(exists("ecfg_fresh/config/config.toml"));
    CHECK(!strcmp(enhance_cfg_user_path(), "ecfg_fresh/config/config.toml"));

    /* none: no file at all, whatever exists, and nothing written. */
    remove("ecfg_fresh/config/config.toml");
    write_file("ecfg_fresh/config/enhance.toml", "[render]\nscale = 2\n");
    recomp_env_set(RENV_CONFIG, "none");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_fresh", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] config: none (RECOMP_CONFIG)\n") != NULL);
    CHECK(strstr(out, "renamed") == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_DEFAULT);
    CHECK(enhance_cfg_file(0) == NULL && enhance_cfg_file(1) == NULL && enhance_cfg_file(2) == NULL);
    CHECK(enhance_cfg_user_path() == NULL && enhance_cfg_defaults() == NULL);
    CHECK(!exists("ecfg_fresh/config/config.toml") && exists("ecfg_fresh/config/enhance.toml"));

    /* The legacy name, through the table: one deprecation line, then the
     * same as RECOMP_CONFIG=none. */
    set_env("RECOMP_CONFIG", NULL);
    set_env("RECOMP_ENHANCE_CONFIG", "none");
    capture_begin();
    recomp_env_reload();
    recomp_env_flush_notes();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENV] RECOMP_ENHANCE_CONFIG is deprecated; use RECOMP_CONFIG\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] config: none (RECOMP_CONFIG)\n") != NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    set_env("RECOMP_ENHANCE_CONFIG", NULL);
    recomp_env_reload();
    recomp_env_flush_notes();
    config_reset_env();

    /* A broken defaults file: said with its line, skipped whole; the user
     * file still applies. */
    write_file(defaults_file(), "[render]\nscale = 2\nscale = 3\n");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_lay", NULL) == -1);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] config error: %s:3: duplicate key", defaults_file());
    CHECK(strstr(out, want) != NULL && strstr(out, "(file ignored)") != NULL);
    CHECK(enhance_cfg_defaults() == NULL);
    CHECK(strstr(out, "[ENHANCE] config: ecfg_lay/config/config.toml (1 key)\n") != NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    remove_defaults();

    /* No file anywhere: "none" alone. */
    MKDIR("ecfg_empty");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_empty", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] config: none\n") != NULL);

#ifdef __APPLE__
    /* An app bundle: Contents/MacOS/<exe> reads Contents/Resources/. Only
     * when that directory is ours to make and remove. */
    {
        char res[4200], file[4300];
        snprintf(res, sizeof res, "%s/../Resources", recomp_exe_dir());
        if (!is_dir(res) && MKDIR(res) == 0) {
            snprintf(file, sizeof file, "%s/config.default.toml", res);
            write_file(file, "[render]\nscale = 3\n");
            CHECK(enhance_cfg_init("ecfg_empty", NULL) == 1);
            CHECK(enhance_cfg_int("render.scale", 1) == 3);
            CHECK(enhance_cfg_source_of("render.scale") == ENHANCE_SRC_DEFAULTS);
            /* Beside the executable wins. */
            write_file(defaults_file(), "[render]\nscale = 2\n");
            CHECK(enhance_cfg_init("ecfg_empty", NULL) == 1);
            CHECK(enhance_cfg_int("render.scale", 1) == 2);
            remove_defaults();
            remove(file);
            RMDIR(res);
        } else {
            printf("enhance_cfg: %s exists; Resources check skipped\n", res);
        }
    }
#endif
    enhance_cfg_shutdown();
}

/* ── the one-time enhance.toml rename ──────────────────────────────── */

static void test_migration(void)
{
    static const char old[] = "# mine\n[render]\nscale = 2\n";
    const char *out;

    config_reset_env();
    remove_defaults();

    /* Under RECOMP_CONFIG=<dir>/config.toml, as every launcher sets it. */
    MKDIR("ecfg_mig");
    MKDIR("ecfg_mig/config");
    remove("ecfg_mig/config/config.toml");
    write_file("ecfg_mig/config/enhance.toml", old);
    recomp_env_set(RENV_CONFIG, "ecfg_mig/config/config.toml");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_mig", NULL) == 1);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] config: renamed ecfg_mig/config/enhance.toml to config.toml"
                      " (one-time migration)\n") != NULL);
    CHECK(!exists("ecfg_mig/config/enhance.toml"));
    CHECK(!strcmp(read_file("ecfg_mig/config/config.toml"), old));      /* same bytes */
    CHECK(enhance_cfg_int("render.scale", 1) == 2);
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_mig", NULL) == 1);
    out = capture_end();
    CHECK(strstr(out, "renamed") == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 2);

    /* At the default locations, the title's and the user's. */
    MKDIR("ecfg_mig2");
    MKDIR("ecfg_mig2/config");
    MKDIR("ecfg_mig2/T");
    remove("ecfg_mig2/config/config.toml");
    remove("ecfg_mig2/T/config.toml");
    write_file("ecfg_mig2/config/enhance.toml", "[render]\nscale = 3\n");
    write_file("ecfg_mig2/T/enhance.toml", "[present]\nfullscreen = true\n");
    recomp_env_set(RENV_CONFIG, NULL);
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_mig2", "T") == 2);
    out = capture_end();
    CHECK(strstr(out, "renamed ecfg_mig2/config/enhance.toml to config.toml") != NULL);
    CHECK(strstr(out, "renamed ecfg_mig2/T/enhance.toml to config.toml") != NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3 && enhance_cfg_bool("present.fullscreen", 0) == 1);

    /* Not candidates: another basename under RECOMP_CONFIG, the old root
     * location, anything under none. */
    MKDIR("ecfg_mig3");
    remove("ecfg_mig3/ecfg_init.toml");
    write_file("ecfg_mig3/enhance.toml", old);
    recomp_env_set(RENV_CONFIG, "ecfg_mig3/ecfg_init.toml");
    capture_begin();
    enhance_cfg_init("ecfg_mig3", NULL);
    out = capture_end();
    CHECK(strstr(out, "renamed") == NULL);
    CHECK(!strcmp(read_file("ecfg_mig3/enhance.toml"), old));
    CHECK(enhance_cfg_int("render.scale", 1) == 1);
    recomp_env_set(RENV_CONFIG, NULL);
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_mig3", NULL) == 0);       /* no config/: nothing */
    out = capture_end();
    CHECK(strstr(out, "renamed") == NULL);
    CHECK(exists("ecfg_mig3/enhance.toml") && !exists("ecfg_mig3/config.toml"));
    CHECK(!is_dir("ecfg_mig3/config"));
    CHECK(enhance_cfg_int("render.scale", 1) == 1);

    /* Both files present: config.toml wins, enhance.toml is not touched. */
    MKDIR("ecfg_mig4");
    MKDIR("ecfg_mig4/config");
    write_file("ecfg_mig4/config/config.toml", "[render]\nscale = 3\n");
    write_file("ecfg_mig4/config/enhance.toml", old);
    recomp_env_set(RENV_CONFIG, "ecfg_mig4/config/config.toml");
    capture_begin();
    CHECK(enhance_cfg_init("ecfg_mig4", NULL) == 1);
    out = capture_end();
    CHECK(strstr(out, "renamed") == NULL);
    CHECK(enhance_cfg_int("render.scale", 1) == 3);
    CHECK(!strcmp(read_file("ecfg_mig4/config/config.toml"), "[render]\nscale = 3\n"));
    CHECK(!strcmp(read_file("ecfg_mig4/config/enhance.toml"), old));

#ifndef _WIN32
    if (geteuid() != 0) {
        int n = 0;
        const char *p;

        /* An existing config.toml the player cannot read is not absent:
         * the header never truncates it and no rename replaces it. */
        write_file("ecfg_mig4/config/config.toml", "[render]\nscale = 4\n");
        chmod("ecfg_mig4/config/config.toml", 0);
        recomp_env_set(RENV_CONFIG, NULL);
        capture_begin();
        enhance_cfg_init("ecfg_mig4", NULL);
        out = capture_end();
        chmod("ecfg_mig4/config/config.toml", 0644);
        CHECK(strstr(out, "renamed") == NULL);
        CHECK(strstr(out, "[ENHANCE] config error:") != NULL);
        CHECK(!strcmp(read_file("ecfg_mig4/config/config.toml"), "[render]\nscale = 4\n"));
        CHECK(!strcmp(read_file("ecfg_mig4/config/enhance.toml"), old));

        /* A rename that fails (a read-only directory): one line, the old
         * file left as is, no header in its place. */
        MKDIR("ecfg_mig5");
        MKDIR("ecfg_mig5/config");
        chmod("ecfg_mig5/config", 0755);
        remove("ecfg_mig5/config/config.toml");
        write_file("ecfg_mig5/config/enhance.toml", old);
        chmod("ecfg_mig5/config", 0555);
        capture_begin();
        enhance_cfg_init("ecfg_mig5", NULL);
        out = capture_end();
        chmod("ecfg_mig5/config", 0755);
        for (p = out; (p = strstr(p, "cannot rename")) != NULL; p++)
            n++;
        CHECK(n == 1);
        CHECK(strstr(out, "ecfg_mig5/config/enhance.toml to config.toml (") != NULL);
        CHECK(strstr(out, "; left as is\n") != NULL);
        CHECK(strstr(out, "cannot create") == NULL);
        CHECK(!exists("ecfg_mig5/config/config.toml"));
        CHECK(!strcmp(read_file("ecfg_mig5/config/enhance.toml"), old));
    }
#endif

    /* The header never truncates: an existing user file keeps its bytes
     * through repeated runs. */
    write_file("ecfg_mig4/config/config.toml", "# kept\n[render]\nscale = 2\n");
    recomp_env_set(RENV_CONFIG, NULL);
    capture_begin();
    enhance_cfg_init("ecfg_mig4", NULL);
    enhance_cfg_init("ecfg_mig4", NULL);
    out = capture_end();
    CHECK(strstr(out, "cannot create") == NULL);
    CHECK(!strcmp(read_file("ecfg_mig4/config/config.toml"), "# kept\n[render]\nscale = 2\n"));
    CHECK(enhance_cfg_int("render.scale", 1) == 2);

    config_reset_env();
    enhance_cfg_shutdown();
}

/* ── xbox_enhance_init: the toolkit's keys into nv2a_host_opts ─────── */

/* The kernel's pacing switch (kernel_pacing.c), which this test does not
 * link: record what the layer hands it. */
static int s_pacing_mode = -1;
void xbox_PacingSetMode(int mode);
void xbox_PacingSetMode(int mode) { s_pacing_mode = mode; }
/* The kernel's RAM cache switch (kernel_ramfs.c), likewise. */
static int s_ram_cache = -1;
void xbox_RamCacheEnable(int on);
void xbox_RamCacheEnable(int on) { s_ram_cache = on; }
/* kernel_path.c: no save root in this test, so a chain profile cannot
 * compose (shader_profile_req.c). */
int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n) { (void)out; (void)n; return 0; }
/* mod.c and enhance_input.c's links into the runtime; no mods run here
 * (tests/enhance_mod has them). */
ptrdiff_t xbox_GetMemoryOffset(void);
ptrdiff_t xbox_GetMemoryOffset(void) { return 0; }
/* mod.c's v6 guest calls run guest code (recomp_guest_call.c, tested in
 * mod_guest_call); this test has none to run. */
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out);
int recomp_guest_call(const char *who, uint32_t va, uint32_t eax, uint32_t ecx, uint32_t edx,
                      const uint32_t *stack, uint32_t n_stack, uint32_t *eax_out)
{
    (void)who; (void)va; (void)eax; (void)ecx; (void)edx; (void)stack; (void)n_stack;
    (void)eax_out;
    return 0;
}
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align);
uint32_t recomp_guest_alloc(uint32_t size, uint32_t align)
{
    (void)size; (void)align;
    return 0;
}
/* The batch info's present offset (nv2a_pb_exec.c); never asked here. */
const struct nv2a_pb_present *nv2a_pb_present_state(void);
const struct nv2a_pb_present *nv2a_pb_present_state(void)
{
    static struct nv2a_pb_present p;
    return &p;
}
/* The overlay (nv2a_overlay.c, tests/overlay has it): record what
 * overlay.stats asks of it. */
static uint32_t s_ovl_layers;
static int s_ovl_stats = -1;
int nv2a_overlay_arm(uint32_t layers, uint32_t out_h)
{
    (void)out_h;
    s_ovl_layers = layers;
    return 0;
}
void nv2a_overlay_set_stats(int on) { s_ovl_stats = on; }
float nv2a_overlay_set_ui_scale(float mul) { return mul; }
int nv2a_overlay_stats(void) { return s_ovl_stats > 0; }
/* The menu (tests/enhance_menu has it): enhance.c starts it. */
void enhance_menu_start(void);
void enhance_menu_start(void) {}
void nv2a_overlay_toast(const char *fmt, ...) { (void)fmt; }
DWORD xbox_InputLastState(DWORD port, XBOX_INPUT_STATE *st)
{
    (void)port;
    memset(st, 0, sizeof *st);
    return ERROR_DEVICE_NOT_CONNECTED;
}
/* input_core.c's host-action seam (enhance_actions.c): no pad here. */
void xbox_InputSetPadFilter(xbox_input_pad_filter_fn fn)
{
    (void)fn;
}
/* The same for the port-0 record (mod.c installs it). */
void xbox_InputSetPadSeen(xbox_input_seen_fn fn)
{
    (void)fn;
}
uint64_t xbox_InputNowMs(void)
{
    return 0;
}
DWORD xbox_input_backend_read_host(DWORD slot, struct xbox_host_pad *host)
{
    (void)slot;
    memset(host, 0, sizeof *host);
    return ERROR_DEVICE_NOT_CONNECTED;
}
/* input_core.c's pad setup, kept so [input.pad] round-trips here. */
static xbox_pad_setup s_pad_setup;
static int s_pad_setup_set;
void xbox_InputSetPadSetup(const xbox_pad_setup *s)
{
    s_pad_setup_set = s != NULL;
    if (s)
        s_pad_setup = *s;
}
void xbox_InputPadSetup(xbox_pad_setup *out)
{
    if (s_pad_setup_set)
        *out = s_pad_setup;
    else
        xbox_PadSetupIdentity(out);
}
static int s_glyphs = XBOX_PAD_FAMILY_AUTO;
void xbox_InputSetGlyphFamily(int family) { s_glyphs = family; }

static void enhance_reset_env(void)
{
    recomp_env_set(RENV_CONFIG, "none");
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_RENDER_ANISO, NULL);
    recomp_env_set(RENV_RENDER_SNAP_2D, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    recomp_env_set(RENV_PRESENT_FILTER, NULL);
    recomp_env_set(RENV_PRESENT_FULLSCREEN, NULL);
    recomp_env_set(RENV_PRESENT_PACING, NULL);
    recomp_env_set(RENV_PRESENT_SURFACE, NULL);
    recomp_env_set(RENV_HOST_GAMESCOPE, NULL);
    recomp_env_set(RENV_TEXTURES_FONT, NULL);
    recomp_env_set(RENV_TEXTURES_TEXT, NULL);
    recomp_env_set(RENV_OVERLAY_STATS, NULL);
}

static void test_enhance_init(void)
{
    const struct nv2a_host_opts *o = nv2a_host_opts();

    /* No file, no env: stock. */
    enhance_reset_env();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    CHECK(o->render_scale == 1 && o->present_filter == NV2A_PRESENT_NEAREST && o->fullscreen == 0);
    CHECK(nv2a_host_render_scale() == 1);

    /* The file answers. */
    write_file("ecfg_init.toml",
               "[render]\nscale = 2\n[present]\nfilter = \"integer\"\nfullscreen = true\n");
    recomp_env_set(RENV_CONFIG, "ecfg_init.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0);
    CHECK(o->render_scale == 2 && o->present_filter == NV2A_PRESENT_INTEGER && o->fullscreen == 1);

    /* Env over file, every key; bool spellings. */
    recomp_env_set(RENV_RENDER_SCALE, "3");
    recomp_env_set(RENV_PRESENT_FILTER, "linear");
    recomp_env_set(RENV_PRESENT_FULLSCREEN, "off");
    CHECK(xbox_enhance_init(".", NULL) == 0);
    CHECK(o->render_scale == 3 && o->present_filter == NV2A_PRESENT_LINEAR && o->fullscreen == 0);
    recomp_env_set(RENV_PRESENT_FULLSCREEN, "Yes");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->fullscreen == 1);

    /* An invalid filter is reported and the file answers (integer); with
     * no file, nearest. */
    recomp_env_set(RENV_PRESENT_FILTER, "bicubic");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_INTEGER);
    recomp_env_set(RENV_CONFIG, "none");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_NEAREST);
    write_file("ecfg_bad.toml", "[present]\nfilter = \"sharp\"\n");
    recomp_env_set(RENV_PRESENT_FILTER, NULL);
    recomp_env_set(RENV_CONFIG, "ecfg_bad.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->present_filter == NV2A_PRESENT_NEAREST);

    /* Scale out of range clamps to 1..4. */
    recomp_env_set(RENV_CONFIG, "none");
    recomp_env_set(RENV_RENDER_SCALE, "9");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 4);
    recomp_env_set(RENV_RENDER_SCALE, "0");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);
    recomp_env_set(RENV_RENDER_SCALE, "-2");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);

    /* render.aniso: unset and 1 are stock (0, no log), 2..16 pass, out of
     * range clamps to 0..16 with a line saying so. */
    {
        const char *out;

        recomp_env_set(RENV_RENDER_SCALE, NULL);
        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aniso == 0);
        recomp_env_set(RENV_RENDER_ANISO, "1");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aniso == 0);
        out = capture_end();
        CHECK(strstr(out, "out of range") == NULL);
        recomp_env_set(RENV_RENDER_ANISO, "8");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aniso == 8);
        capture_begin();
        recomp_env_set(RENV_RENDER_ANISO, "32");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aniso == 16);
        out = capture_end();
        CHECK(strstr(out, "render.aniso=32 out of range 0..16; using 16") != NULL);
        capture_begin();
        recomp_env_set(RENV_RENDER_ANISO, "-1");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aniso == 0);
        out = capture_end();
        CHECK(strstr(out, "render.aniso=-1 out of range 0..16; using 0") != NULL);
        recomp_env_set(RENV_RENDER_ANISO, NULL);
    }

    /* present.profile: logged on an [ENHANCE] line only when chosen (the
     * golden check's stock value is its absence), even when the profile
     * then fails: here there is no profiles folder under RECOMP_CONFIG=none. */
    {
        const char *out;

        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0);
        out = capture_end();
        CHECK(strstr(out, "present.profile=") == NULL);
        recomp_env_set(RENV_PRESENT_PROFILE, "soft-crt");
        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0);
        out = capture_end();
        CHECK(strstr(out, "[ENHANCE] present.profile=soft-crt\n") != NULL);
        CHECK(strstr(out, "[SHADER] profile soft-crt: no profiles under RECOMP_CONFIG=none")
              != NULL);
        recomp_env_set(RENV_PRESENT_PROFILE, NULL);
    }

    /* render.snap_2d: off unless asked (the stock line does not name it),
     * on from the environment, and named on the line then. */
    {
        const char *out;

        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->snap_2d == 0);
        out = capture_end();
        CHECK(strstr(out, "snap_2d") == NULL);
        capture_begin();
        recomp_env_set(RENV_RENDER_SNAP_2D, "1");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->snap_2d == 1);
        out = capture_end();
        CHECK(strstr(out, " render.snap_2d=1") != NULL);
        recomp_env_set(RENV_RENDER_SNAP_2D, "0");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->snap_2d == 0);
        recomp_env_set(RENV_RENDER_SNAP_2D, NULL);
    }

    /* A non-4:3 aspect is reported, nothing else changes. */
    recomp_env_set(RENV_RENDER_SCALE, NULL);
    recomp_env_set(RENV_DISPLAY_ASPECT, "16:9");
    CHECK(xbox_enhance_init(".", NULL) == 0 && o->render_scale == 1);
    CHECK(o->aspect_w == 16 && o->aspect_h == 9);
    /* the whole value is the ratio: trailing junk is reported, 4:3 used */
    {
        const char *out;
        capture_begin();
        recomp_env_set(RENV_DISPLAY_ASPECT, "16:9x");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->aspect_w == 0 && o->aspect_h == 0);
        out = capture_end();
        CHECK(strstr(out, "display.aspect=16:9x is not a W:H ratio") != NULL);
    }

    /* display.overscan: 0 by default with the summary line as before, the
     * file and the variable, 0..64 with a clamp line, a non-number reported
     * with 0 used, refused under hor+ but not when the aspect fell back. */
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    {
        const char *out;
        capture_begin();
        recomp_env_set(RENV_CONFIG, "none");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 0);
        out = capture_end();
        CHECK(strstr(out, " (display.aspect=4:3)\n") != NULL);
        CHECK(strstr(out, "overscan") == NULL);
        write_file("ecfg_overscan.toml", "[display]\noverscan = 9\n");
        recomp_env_set(RENV_CONFIG, "ecfg_overscan.toml");
        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 9);
        out = capture_end();
        CHECK(strstr(out, " (display.aspect=4:3 display.overscan=9)\n") != NULL);
        recomp_env_set(RENV_DISPLAY_OVERSCAN, "12");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 12);
        capture_begin();
        recomp_env_set(RENV_DISPLAY_OVERSCAN, "65");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 64);
        out = capture_end();
        CHECK(strstr(out, "display.overscan=65 out of range 0..64; using 64") != NULL);
        CHECK(strstr(out, " (display.aspect=4:3 display.overscan=64)\n") != NULL);
        capture_begin();
        recomp_env_set(RENV_DISPLAY_OVERSCAN, "-1");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 0);
        out = capture_end();
        CHECK(strstr(out, "display.overscan=-1 out of range 0..64; using 0") != NULL);
        CHECK(strstr(out, " (display.aspect=4:3)\n") != NULL);
        capture_begin();
        recomp_env_set(RENV_DISPLAY_OVERSCAN, "wide");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 9);   /* the file answers */
        out = capture_end();
        CHECK(strstr(out, "RECOMP_DISPLAY_OVERSCAN") != NULL);
        recomp_env_set(RENV_DISPLAY_OVERSCAN, "9");
        capture_begin();
        recomp_env_set(RENV_DISPLAY_ASPECT, "16:9");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 0 && o->aspect_w == 16);
        out = capture_end();
        CHECK(strstr(out, "display.overscan=9 ignored with display.aspect=16:9:") != NULL);
        CHECK(strstr(out, " (display.aspect=16:9)\n") != NULL);
        capture_begin();
        recomp_env_set(RENV_DISPLAY_ASPECT, "16:9x");
        CHECK(xbox_enhance_init(".", NULL) == 0 && o->overscan == 9 && o->aspect_w == 0);
        out = capture_end();
        CHECK(strstr(out, "ignored with") == NULL);
        recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
        recomp_env_set(RENV_DISPLAY_OVERSCAN, NULL);
        recomp_env_set(RENV_CONFIG, NULL);
    }

    /* There is no HUD option: the HUD keeps its 4:3 layout. A config.toml
     * from a build that had display.hud (the user's Deck: hud = "edges")
     * loads, its other keys apply, and the stale key gets the generic
     * unused-key line like any other unknown key. */
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    {
        const char *out;
        struct nv2a_host_opts ref;
        write_file("ecfg_hud0.toml", "[display]\naspect = \"16:10\"\nmovie = \"fit\"\n");
        write_file("ecfg_hud.toml",
                   "[display]\naspect = \"16:10\"\nhud = \"edges\"\nmovie = \"fit\"\n");
        recomp_env_set(RENV_CONFIG, "ecfg_hud0.toml");
        CHECK(xbox_enhance_init(".", NULL) == 0);
        ref = *o;
        recomp_env_set(RENV_CONFIG, "ecfg_hud.toml");
        capture_begin();
        CHECK(xbox_enhance_init(".", NULL) == 0);
        CHECK(enhance_cfg_report_unused() == 1);
        out = capture_end();
        CHECK(memcmp(o, &ref, sizeof ref) == 0);
        CHECK(o->aspect_w == 16 && o->aspect_h == 10 && o->movie_mode == NV2A_MOVIE_FIT);
        CHECK(strstr(out, "[ENHANCE] unused key display.hud, ecfg_hud.toml:3\n") != NULL);
        CHECK(strstr(out, "display.aspect=16:10 display.movie=fit\n") != NULL);
        recomp_env_set(RENV_CONFIG, NULL);
    }

    /* present.pacing: sleep by default (no file, no variable), spin from
     * the file, env over file, and a bad value reported with the file
     * answering: the file says spin, so the fallback is visible. */
    recomp_env_set(RENV_DISPLAY_ASPECT, NULL);
    recomp_env_set(RENV_CONFIG, "none");
    s_pacing_mode = -1;
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    write_file("ecfg_pacing.toml", "[present]\npacing = \"spin\"\n");
    recomp_env_set(RENV_CONFIG, "ecfg_pacing.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 0);
    recomp_env_set(RENV_PRESENT_PACING, "sleep");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    CHECK(enhance_cfg_report_unused() == 0);    /* overridden, not unused */
    recomp_env_set(RENV_PRESENT_PACING, "nap");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 0);
    recomp_env_set(RENV_PRESENT_PACING, NULL);
    recomp_env_set(RENV_CONFIG, "none");
    recomp_env_set(RENV_PRESENT_PACING, "spin");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 0);
    /* A bad value with no file falls to the code default, sleep. */
    recomp_env_set(RENV_PRESENT_PACING, "nap");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_pacing_mode == 1);
    recomp_env_set(RENV_PRESENT_PACING, NULL);

    /* storage.ram_cache: on unless told otherwise, off from the
     * file, and the variable over the file both ways. */
    recomp_env_set(RENV_CONFIG, "none");
    s_ram_cache = -1;
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ram_cache == 1);
    write_file("ecfg_ram.toml", "[storage]\nram_cache = false\n");
    recomp_env_set(RENV_CONFIG, "ecfg_ram.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ram_cache == 0);
    recomp_env_set(RENV_STORAGE_RAM_CACHE, "1");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ram_cache == 1);
    write_file("ecfg_ram.toml", "[storage]\nram_cache = true\n");
    recomp_env_set(RENV_STORAGE_RAM_CACHE, "0");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ram_cache == 0);
    CHECK(enhance_cfg_report_unused() == 0);
    recomp_env_set(RENV_STORAGE_RAM_CACHE, NULL);
    recomp_env_set(RENV_CONFIG, "none");

    /* overlay.stats: stock arms neither the actions hook nor the overlay
     * (no atlas line in a golden). A feature key arms the stats and toast
     * layers, hidden, so the action and toasts can show later; on also
     * shows the strip. */
    s_ovl_layers = 0;
    s_ovl_stats = -1;
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ovl_stats == -1 && s_ovl_layers == 0);
    write_file("ecfg_menu.toml", "[menu]\nenabled = true\n");
    recomp_env_set(RENV_CONFIG, "ecfg_menu.toml");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ovl_stats == -1);
    CHECK(s_ovl_layers == ((1u << NV2A_OVL_STATS) | (1u << NV2A_OVL_TOAST)));
    recomp_env_set(RENV_CONFIG, "none");
    s_ovl_layers = 0;
    recomp_env_set(RENV_OVERLAY_STATS, "1");
    CHECK(xbox_enhance_init(".", NULL) == 0 && s_ovl_stats == 1);
    CHECK(s_ovl_layers == ((1u << NV2A_OVL_STATS) | (1u << NV2A_OVL_TOAST)));
    recomp_env_set(RENV_OVERLAY_STATS, NULL);

    /* A file that does not parse: -1, stock. */
    write_file("ecfg_broken.toml", "[render]\nscale = \n");
    recomp_env_set(RENV_CONFIG, "ecfg_broken.toml");
    CHECK(xbox_enhance_init(".", NULL) == -1 && o->render_scale == 1);

    enhance_reset_env();
    recomp_env_set(RENV_CONFIG, NULL);
    enhance_cfg_shutdown();
}

/* ── present.surface: the key and the launcher's gamescope hint ────── */

/* One init with key `key` and hint `hint` (NULL = unset): the resolved
 * surface, and the [ENHANCE] line's present.surface= fragment. */
static void check_surface(const char *key, const char *hint, int want,
                          const char *frag, int line)
{
    const struct nv2a_host_opts *o = nv2a_host_opts();
    char needle[96];
    const char *out;
    int ok;

    recomp_env_set(RENV_PRESENT_SURFACE, key);
    recomp_env_set(RENV_HOST_GAMESCOPE, hint);
    capture_begin();
    ok = xbox_enhance_init(".", NULL) == 0;
    out = capture_end();
    snprintf(needle, sizeof needle, " present.surface=%s (display.aspect=", frag);
    s_checks++;
    if (!ok || o->present_surface != want || !strstr(out, needle)) {
        s_fail++;
        fprintf(stderr, "FAIL %s:%d: surface key=%s hint=%s: got %d, want %d (%s)\n%s",
                __FILE__, line, key ? key : "unset", hint ? hint : "unset",
                o->present_surface, want, frag, out);
    }
}

static void test_present_surface(void)
{
    enum { W = NV2A_PRESENT_SURFACE_WINDOW, I = NV2A_PRESENT_SURFACE_INTERNAL };
    const char *out;

    enhance_reset_env();
    /* Unset and auto: the hint decides; only a true hint means internal. */
    check_surface(NULL, NULL, W, "window(auto)", __LINE__);
    check_surface(NULL, "0", W, "window(auto)", __LINE__);
    check_surface(NULL, "1", I, "internal(auto: gamescope)", __LINE__);
    check_surface("auto", NULL, W, "window(auto)", __LINE__);
    check_surface("auto", "0", W, "window(auto)", __LINE__);
    check_surface("auto", "1", I, "internal(auto: gamescope)", __LINE__);
    /* An explicit value ignores the hint. */
    check_surface("window", NULL, W, "window", __LINE__);
    check_surface("window", "0", W, "window", __LINE__);
    check_surface("window", "1", W, "window", __LINE__);
    check_surface("internal", NULL, I, "internal", __LINE__);
    check_surface("internal", "0", I, "internal", __LINE__);
    check_surface("internal", "1", I, "internal", __LINE__);
    /* An invalid value is reported (once) and read as auto. */
    recomp_env_set(RENV_PRESENT_SURFACE, "compositor");
    recomp_env_set(RENV_HOST_GAMESCOPE, NULL);
    capture_begin();
    CHECK(xbox_enhance_init(".", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] RECOMP_PRESENT_SURFACE=compositor is not one of"
                      " auto|window|internal; ignored\n") != NULL);
    check_surface("compositor", NULL, W, "window(auto)", __LINE__);
    check_surface("compositor", "0", W, "window(auto)", __LINE__);
    check_surface("compositor", "1", I, "internal(auto: gamescope)", __LINE__);

    /* The file's key, and the environment over it. */
    write_file("ecfg_surface.toml", "[present]\nsurface = \"window\"\n");
    recomp_env_set(RENV_CONFIG, "ecfg_surface.toml");
    check_surface(NULL, "1", W, "window", __LINE__);
    check_surface("internal", NULL, I, "internal", __LINE__);
    CHECK(enhance_cfg_report_unused() == 0);    /* overridden, not unused */

    enhance_reset_env();
    recomp_env_set(RENV_CONFIG, NULL);
    enhance_cfg_shutdown();
}

/* ── textures.pack: path resolution and the lines golden reads ────── */

/* A one-entry pack: a 1x1 texel at scale 2. */
static void write_pack(const char *path)
{
    unsigned char b[68];
    FILE *f;
    memset(b, 0, sizeof b);
    memcpy(b, NV2A_TEX_PACK_MAGIC, 4);
    b[4] = 1;                           /* version */
    b[8] = 1;                           /* count */
    b[12] = 2;                          /* scale */
    b[16] = 32;                         /* index offset */
    b[32] = 0x5A;                       /* hash */
    b[40] = 1; b[44] = 1; b[48] = 1;    /* 1x1, one level */
    b[56] = 64;                         /* texel offset */
    b[64] = 0xFF; b[67] = 0xFF;
    f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "cannot write %s\n", path); exit(2); }
    fwrite(b, 1, sizeof b, f);
    fclose(f);
}

static void test_tex_pack_key(void)
{
    char abs[4200], dir[4200], want[4400];
    const char *out;

    /* The pack sits beside the executable, as a bundle's enhance/hud.pack
     * does; the user root (ecfg_root) is not where it is looked for. */
    enhance_reset_env();
    MKDIR("ecfg_root");
    snprintf(dir, sizeof dir, "%s/ecfg_enhance", recomp_exe_dir());
    MKDIR(dir);
    snprintf(abs, sizeof abs, "%s/t.pack", dir);
    write_pack(abs);

    /* Unset and "none": the none line, no pack. */
    recomp_env_set(RENV_TEXTURES_PACK, NULL);
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] textures.pack=none\n") != NULL);
    CHECK(nv2a_tex_pack_count() == 0);
    recomp_env_set(RENV_TEXTURES_PACK, "none");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] textures.pack=none\n") != NULL);
    CHECK(nv2a_tex_pack_count() == 0);

    /* Relative: against the executable's directory. The line names the
     * key's value. */
    recomp_env_set(RENV_TEXTURES_PACK, "ecfg_enhance/t.pack");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] textures.pack=ecfg_enhance/t.pack (1 entries: 1 art, 0 font, 0 text;"
                      " scale 2, text 2; 0 bc3) textures.font=on textures.text=on\n") != NULL);
    CHECK(nv2a_tex_pack_count() == 1 && nv2a_tex_pack_scale() == 2);
    CHECK(nv2a_host_opts()->pack_off == 0);

    /* The kinds' keys: "off" reaches pack_off and the line; stock is on. */
    recomp_env_set(RENV_TEXTURES_TEXT, "off");
    recomp_env_set(RENV_TEXTURES_FONT, "off");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "textures.font=off textures.text=off\n") != NULL);
    CHECK(nv2a_host_opts()->pack_off == (NV2A_PACK_OFF_FONT | NV2A_PACK_OFF_TEXT));
    recomp_env_set(RENV_TEXTURES_FONT, NULL);
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "textures.font=on textures.text=off\n") != NULL);
    CHECK(nv2a_host_opts()->pack_off == NV2A_PACK_OFF_TEXT);
    recomp_env_set(RENV_TEXTURES_TEXT, NULL);

    /* A later "none" drops it. */
    recomp_env_set(RENV_TEXTURES_PACK, "none");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    capture_end();
    CHECK(nv2a_tex_pack_count() == 0);

    /* Absolute: taken as is, never joined to a directory. */
    recomp_env_set(RENV_TEXTURES_PACK, abs);
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_no_such_root", NULL) == 0);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] textures.pack=%s (1 entries: 1 art, 0 font, 0 text;"
             " scale 2, text 2; 0 bc3) textures.font=on textures.text=on\n", abs);
    CHECK(strstr(out, want) != NULL);
    CHECK(nv2a_tex_pack_count() == 1);

    /* Refused: the reason and the resolved path, "; no pack", stock. The
     * golden check skips this line, so a refused pack reads as none. */
    recomp_env_set(RENV_TEXTURES_PACK, "ecfg_enhance/missing.pack");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    snprintf(want, sizeof want, "[ENHANCE] textures.pack=ecfg_enhance/missing.pack: cannot open"
             " (%s/missing.pack); no pack\n", dir);
    CHECK(strstr(out, want) != NULL);
    CHECK(nv2a_tex_pack_count() == 0);

    /* The key from config.default.toml, as --hud-pack writes it: the pack
     * resolves against the same directory the file is read from. */
    recomp_env_set(RENV_TEXTURES_PACK, NULL);
    recomp_env_set(RENV_CONFIG, "ecfg_root/ecfg_pack_user.toml");
    write_file("ecfg_root/ecfg_pack_user.toml", "");
    write_file(defaults_file(), "[textures]\npack = \"ecfg_enhance/t.pack\"\n");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] textures.pack=ecfg_enhance/t.pack (1 entries") != NULL);
    CHECK(nv2a_tex_pack_count() == 1);
    CHECK(enhance_cfg_source_of("textures.pack") == ENHANCE_SRC_DEFAULTS);
    /* "none" in the user file turns a shipped pack off. */
    write_file("ecfg_root/ecfg_pack_user.toml", "[textures]\npack = \"none\"\n");
    capture_begin();
    CHECK(xbox_enhance_init("ecfg_root", NULL) == 0);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] textures.pack=none\n") != NULL);
    CHECK(nv2a_tex_pack_count() == 0);
    remove_defaults();
    remove(abs);

    enhance_reset_env();
    recomp_env_set(RENV_TEXTURES_PACK, NULL);
    recomp_env_set(RENV_CONFIG, NULL);
    enhance_cfg_shutdown();
    nv2a_tex_pack_unload();
}

/* ── [input] (enhance_input.c) ─────────────────────────────────────── */

/* Control ctl's inputs as "Name,Name" (mouse names included). */
static const char *bound(int ctl)
{
    static char buf[256];
    int codes[XBOX_BIND_MAX], mouse[XBOX_BIND_MAX];
    int n = xbox_InputBindingsOf(ctl, codes, mouse, XBOX_BIND_MAX);
    size_t len = 0;
    buf[0] = 0;
    for (int i = 0; i < n && i < XBOX_BIND_MAX; i++) {
        const char *name = xbox_InputCodeName(codes[i], mouse[i]);
        len += (size_t)snprintf(buf + len, sizeof buf - len, "%s%s", i ? "," : "",
                                name ? name : "?");
    }
    return buf;
}

/* Load the defaults file and the RECOMP_CONFIG user file, then the [input]
 * keys; the captured stderr comes back. */
static const char *input_load(const char *defaults, const char *user)
{
    if (defaults)
        write_file(defaults_file(), defaults);
    else
        remove_defaults();
    write_file("ecfg_in/user.toml", user ? user : "");
    recomp_env_set(RENV_CONFIG, "ecfg_in/user.toml");
    capture_begin();
    enhance_cfg_init("ecfg_in", NULL);
    enhance_input_init();
    return capture_end();
}

static void test_input(void)
{
    static const char defaults[] =
        "[input]\n"
        "keyboard = true\n"
        "mouse = true\n"
        "[input.bindings]\n"
        "a = \"Space\"\n"
        "start = [\"Enter\", \"Escape\"]\n"
        "b = \"mouse1\"\n";
    char want[4400];
    const char *out;
    xbox_mouse_opts mo;

    MKDIR("ecfg_in");
    recomp_env_set(RENV_KEYBOARD, NULL);
    recomp_env_set(RENV_MOUSE, NULL);

    /* The defaults file alone. */
    out = input_load(defaults, NULL);
    CHECK(!strcmp(bound(INPUT_A), "space"));
    CHECK(!strcmp(bound(INPUT_START), "enter,escape"));
    CHECK(!strcmp(bound(INPUT_B), "mouse1"));
    CHECK(xbox_InputKeyboardOn() && xbox_InputMouseOn());
    CHECK(enhance_cfg_source_of("input.keyboard") == ENHANCE_SRC_DEFAULTS);
    snprintf(want, sizeof want, "[INPUT] config: keyboard=on mouse=on look=right_stick"
             " sens=1.0 bindings=3 (%s)\n", defaults_file());
    CHECK(strstr(out, want) != NULL);
    CHECK(strstr(out, "config error") == NULL);
    /* The knobs' defaults (design D3). */
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.capture == XBOX_MOUSE_CAPTURE_FOCUS && mo.look == XBOX_MOUSE_LOOK_RIGHT);
    CHECK(mo.sensitivity == 1.0 && mo.min_deflection == 0.45);
    CHECK(mo.release_key == 0x78 && mo.wheel_pulse_ms == 100);
    /* Every binding key was read, so none is reported unused. */
    capture_begin();
    enhance_cfg_report_unused();
    out = capture_end();
    CHECK(strstr(out, "input.bindings") == NULL);

    /* The player's a replaces the defaults' whole. */
    out = input_load(defaults, "[input.bindings]\na = \"J\"\n");
    CHECK(!strcmp(bound(INPUT_A), "J"));
    CHECK(!strcmp(bound(INPUT_START), "enter,escape"));
    CHECK(strstr(out, " bindings=2 (") != NULL);
    CHECK(strstr(out, " +1 (ecfg_in/user.toml)\n") != NULL);

    /* A key the player binds elsewhere leaves the inherited control. */
    out = input_load(defaults, "[input.bindings]\nback = \"Escape\"\n");
    CHECK(!strcmp(bound(INPUT_BACK), "escape"));
    CHECK(!strcmp(bound(INPUT_START), "enter"));
    CHECK(!strcmp(bound(INPUT_A), "space"));

    /* [] unbinds; a control no file names keeps the compiled-in default. */
    out = input_load(defaults, "[input.bindings]\na = []\n");
    CHECK(!strcmp(bound(INPUT_A), ""));
    {
        char compiled[256];
        xbox_InputBindingsReset();
        snprintf(compiled, sizeof compiled, "%s", bound(INPUT_Y));
        input_load(defaults, NULL);
        CHECK(!strcmp(bound(INPUT_Y), compiled) && compiled[0]);
    }

    /* A control left on the compiled-in default still loses a key the
     * player binds elsewhere (no defaults file, so A is the compiled one):
     * the player's b takes A's first compiled key, A keeps the rest. */
    {
        char compiled[256], key[64], user[128];
        const char *comma;
        xbox_InputBindingsReset();
        snprintf(compiled, sizeof compiled, "%s", bound(INPUT_A));
        CHECK(compiled[0] != 0);
        comma = strchr(compiled, ',');
        snprintf(key, sizeof key, "%.*s",
                 (int)(comma ? (size_t)(comma - compiled) : strlen(compiled)), compiled);
        snprintf(user, sizeof user, "[input.bindings]\nb = \"%s\"\n", key);
        input_load(NULL, user);
        CHECK(!strcmp(bound(INPUT_B), key));
        CHECK(!strcmp(bound(INPUT_A), comma ? comma + 1 : ""));
    }

    /* The release key and the walk key bound to a control get a warning. */
    out = input_load(defaults, "[input]\nwalk_key = \"LAlt\"\n"
                     "[input.bindings]\ny = \"F9\"\n"
                     "x = \"LAlt\"\n");
    CHECK(strstr(out, "[INPUT] config warning: input.mouse_release_key 'F9' is also"
                 " bound to input.bindings.y\n") != NULL);
    CHECK(strstr(out, "[INPUT] config warning: input.walk_key 'lalt' is also"
                 " bound to input.bindings.x\n") != NULL);
    /* The code default is no walk key (the legacy keyboard): LAlt is free. */
    out = input_load(defaults, "[input.bindings]\nx = \"LAlt\"\n");
    CHECK(strstr(out, "input.walk_key") == NULL);
    out = input_load(defaults, "[input]\nwalk_key = \"F9\"\n");
    CHECK(strstr(out, "input.walk_key and input.mouse_release_key are the same key")
          != NULL);
    out = input_load(defaults, NULL);
    CHECK(strstr(out, "config warning") == NULL);

    /* "none" and "" are no key, without an error: the release key can be
     * turned off (its default is F9), and so can the walk key. */
    out = input_load(defaults, "[input]\nmouse_release_key = \"None\"\nwalk_key = \"\"\n");
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.release_key == 0);
    CHECK(strstr(out, "config error") == NULL);
    out = input_load(defaults, NULL);
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.release_key == 0x78);

    /* A script with both devices off would never play: say so. */
    recomp_env_set(RENV_KBM_SCRIPT, "probe.kbm");
    out = input_load(NULL, NULL);
    CHECK(strstr(out, "[INPUT] kbm_script ignored: keyboard and mouse off (probe.kbm)\n")
          != NULL);
    out = input_load(defaults, NULL);
    CHECK(strstr(out, "kbm_script ignored") == NULL);
    recomp_env_set(RENV_KBM_SCRIPT, NULL);

    /* The three capture choices. */
    out = input_load(defaults, "[input]\nmouse_capture = \"click\"\n");
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.capture == XBOX_MOUSE_CAPTURE_CLICK);
    out = input_load(defaults, "[input]\nmouse_capture = \"never\"\n");
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.capture == XBOX_MOUSE_CAPTURE_NEVER);
    out = input_load(defaults, "[input]\nmouse_capture = \"focus\"\nmouse_look = \"none\"\n"
                     "mouse_sensitivity = 2\nmouse_invert_y = true\n");
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.capture == XBOX_MOUSE_CAPTURE_FOCUS && mo.look == XBOX_MOUSE_LOOK_NONE);
    CHECK(mo.sensitivity == 2.0 && mo.invert_y && !mo.invert_x);
    CHECK(strstr(out, "look=none sens=2.0") != NULL);

    /* One input under two controls in one file: the second is reported
     * with the first's line, and the first kept. */
    out = input_load(defaults, "[input.bindings]\nx = \"J\"\ny = \"J\"\n");
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:3: 'J' already bound to"
                 " input.bindings.x on line 2; ignored here\n") != NULL);
    CHECK(!strcmp(bound(INPUT_X), "J"));
    CHECK(!strcmp(bound(INPUT_Y), ""));

    /* Each mistake its line, and the rest of the file applied. */
    out = input_load(defaults,
                     "[input]\n"
                     "mouse_sensitivity = 0\n"                  /* 2 */
                     "mouse_look = \"camera\"\n"                /* 3 */
                     "walk_scale = 0.3\n"                       /* 4 */
                     "stick_ramp_ms = \"fast\"\n"               /* 5 */
                     "[input.bindings]\n"
                     "jmp = \"J\"\n"                            /* 7 */
                     "x = [\"Spacebar\", \"K\"]\n"              /* 8 */
                     "a = 5\n"                                  /* 9 */
                     "white = \"F10\"\n"                        /* 10 */
                     "y = \"Q\"\n");                            /* 11 */
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:2: input.mouse_sensitivity 0"
                 " out of range 0.1 .. 10; using 1\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:3: input.mouse_look 'camera'"
                 " is not one of right_stick, left_stick, none; using right_stick\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:4: input.walk_scale 0.3"
                 " out of range 0.41 .. 1; using 0.41\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:5: input.stick_ramp_ms should be"
                 " an integer (got string); using the default\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:7: unknown control 'jmp' under"
                 " [input.bindings] (controls: dpad_up dpad_down dpad_left dpad_right start back"
                 " left_thumb right_thumb a b x y black white left_trigger right_trigger"
                 " left_stick_up left_stick_down left_stick_left left_stick_right"
                 " right_stick_up right_stick_down right_stick_left right_stick_right);"
                 " ignored\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:8: unknown input 'Spacebar' in"
                 " input.bindings.x (see docs/input.md); skipped\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:9: input.bindings.a is a string or"
                 " a list of strings (got integer); ignored\n") != NULL);
    CHECK(strstr(out, "[INPUT] config error: ecfg_in/user.toml:10: input.bindings.white 'F10' is"
                 " menu.open's default ([input.actions]); bound anyway\n") != NULL);
    CHECK(strstr(out, "[ENHANCE] ecfg_in") == NULL);     /* [INPUT] lines only */
    CHECK(!strcmp(bound(INPUT_X), "K"));
    CHECK(!strcmp(bound(INPUT_A), "space"));            /* the defaults' */
    CHECK(!strcmp(bound(INPUT_WHITE), "F10"));
    CHECK(!strcmp(bound(INPUT_Y), "Q"));
    xbox_InputMouseGetOpts(&mo);
    CHECK(mo.sensitivity == 1.0 && mo.look == XBOX_MOUSE_LOOK_RIGHT);

    /* The environment over the file, left to the env path. */
    recomp_env_set(RENV_KEYBOARD, "0");
    out = input_load(defaults, NULL);
    CHECK(!xbox_InputKeyboardOn() && xbox_InputMouseOn());
    CHECK(enhance_cfg_source_of("input.keyboard") == ENHANCE_SRC_ENV);
    CHECK(strstr(out, "[INPUT] config: keyboard=off mouse=on") != NULL);
    recomp_env_set(RENV_KEYBOARD, NULL);
    /* The player's file turns the mouse off. */
    out = input_load(defaults, "[input]\nmouse = false\n");
    CHECK(xbox_InputKeyboardOn() && !xbox_InputMouseOn());

    /* none: no file, both off, the compiled-in table. */
    recomp_env_set(RENV_CONFIG, "none");
    capture_begin();
    enhance_cfg_init("ecfg_in", NULL);
    enhance_input_init();
    out = capture_end();
    CHECK(strstr(out, "[INPUT] config: keyboard=off mouse=off (RECOMP_CONFIG=none)\n") != NULL);
    CHECK(!xbox_InputKeyboardOn() && !xbox_InputMouseOn());
    {
        char now[256];
        snprintf(now, sizeof now, "%s", bound(INPUT_START));
        xbox_InputBindingsReset();
        CHECK(!strcmp(now, bound(INPUT_START)));
    }

    remove_defaults();
    remove("ecfg_in/user.toml");
    recomp_env_set(RENV_CONFIG, NULL);
    enhance_cfg_shutdown();
    xbox_InputKeyboardEnable(-1);
    xbox_InputMouseEnable(-1);
}

/* ── recomp_exe_dir ────────────────────────────────────────────────── */

static void test_exe_dir(void)
{
    const char *d = recomp_exe_dir();
    struct stat st;
    CHECK(d && *d);
    CHECK(stat(d, &st) == 0 && (st.st_mode & S_IFDIR));
    CHECK(recomp_exe_dir() == d);          /* cached */
    {
        /* This test's own binary sits in that directory. */
        char p[4096];
        FILE *f;
#ifdef _WIN32
        snprintf(p, sizeof p, "%s/enhance_cfg_test.exe", d);
#else
        snprintf(p, sizeof p, "%s/enhance_cfg_test", d);
#endif
        f = fopen(p, "rb");
        CHECK(f != NULL);
        if (f)
            fclose(f);
    }
}

/* ── [input.pad] (enhance_input.c, design D3 of gamepad-input-ux) ──── */

static xbox_pad_setup pad_now(void)
{
    xbox_pad_setup s;
    xbox_InputPadSetup(&s);
    return s;
}

static void test_input_pad(void)
{
    static const char defaults[] =
        "[input.pad]\n"
        "x = [\"west\", \"paddle3\"]\n"
        "dpad_up = [\"dpad_up\", \"paddle1\"]\n";
    xbox_pad_setup s, id;
    const char *out;

    MKDIR("ecfg_in");
    xbox_PadSetupIdentity(&id);

    /* No [input.pad]: the identity, and no pad line. */
    out = input_load(NULL, NULL);
    s = pad_now();
    CHECK(xbox_PadSetupIsIdentity(&s));
    CHECK(strstr(out, "[INPUT] pad map") == NULL);
    CHECK(s_glyphs == XBOX_PAD_FAMILY_AUTO);

    /* The defaults' second cells go to the alternate map. */
    out = input_load(defaults, NULL);
    s = pad_now();
    CHECK(s.map.source[INPUT_X] == INPUT_SRC_WEST && s.alt.source[INPUT_X] == INPUT_SRC_PADDLE3);
    CHECK(s.map.source[INPUT_DPAD_UP] == INPUT_SRC_DPAD_UP
          && s.alt.source[INPUT_DPAD_UP] == INPUT_SRC_PADDLE1);
    CHECK(s.map.source[INPUT_A] == INPUT_SRC_SOUTH && s.alt.source[INPUT_A] == INPUT_SRC_NONE);
    CHECK(strstr(out, "[INPUT] pad map: 2 (") != NULL);
    CHECK(strstr(out, "config error") == NULL);
    capture_begin();
    enhance_cfg_report_unused();
    out = capture_end();
    CHECK(strstr(out, "input.pad") == NULL);

    /* The player's file: a control wins whole, and a source it binds leaves
     * the controls below (paddle3 off x, left_shoulder off white). */
    out = input_load(defaults, "[input.pad]\ny = \"paddle3\"\nb = \"left_shoulder\"\n");
    s = pad_now();
    CHECK(s.map.source[INPUT_Y] == INPUT_SRC_PADDLE3 && s.alt.source[INPUT_Y] == INPUT_SRC_NONE);
    CHECK(s.map.source[INPUT_X] == INPUT_SRC_WEST && s.alt.source[INPUT_X] == INPUT_SRC_NONE);
    CHECK(s.map.source[INPUT_B] == INPUT_SRC_LEFT_SHOULDER);
    CHECK(s.map.source[INPUT_WHITE] == INPUT_SRC_NONE);
    CHECK(s.map.source[INPUT_BLACK] == INPUT_SRC_RIGHT_SHOULDER);
    CHECK(strstr(out, " +2 (ecfg_in/user.toml)") != NULL);

    /* A source removed from cell 0 moves cell 1 up. */
    out = input_load(defaults, "[input.pad]\na = \"west\"\n");
    s = pad_now();
    CHECK(s.map.source[INPUT_A] == INPUT_SRC_WEST);
    CHECK(s.map.source[INPUT_X] == INPUT_SRC_PADDLE3 && s.alt.source[INPUT_X] == INPUT_SRC_NONE);

    /* [] and "none" unbind; sticks swap; options. */
    out = input_load(NULL, "[input]\npad_deadzone_left = 20\npad_threshold = 30\n"
                     "pad_invert_ry = true\nglyphs = \"steam\"\n"
                     "[input.pad]\nback = []\nstart = \"none\"\n"
                     "left_stick = \"right\"\nright_stick = \"none\"\n");
    s = pad_now();
    CHECK(s.map.source[INPUT_BACK] == INPUT_SRC_NONE && s.map.source[INPUT_START] == INPUT_SRC_NONE);
    CHECK(s.map.left_stick == INPUT_STICK_RIGHT && s.map.right_stick == INPUT_STICK_NONE);
    CHECK(s.map.deadzone_left == 20 && s.map.deadzone_right == 0);
    CHECK(s.map.button_threshold == 30 && s.alt.button_threshold == 30);
    CHECK(!s.invert_ly && s.invert_ry);
    CHECK(s_glyphs == XBOX_PAD_FAMILY_STEAM);
    CHECK(strstr(out, "deadzone=20/0%") != NULL && strstr(out, "invert=ry") != NULL);
    CHECK(strstr(out, "config error") == NULL);

    /* The env deadzone keeps its integer meaning; the file key wins, said once. */
    recomp_env_set(RENV_PAD_DEADZONE, "7849");
    out = input_load(NULL, "[input]\npad_deadzone_right = 10\n");
    CHECK(strstr(out, "input.pad_deadzone_right wins over RECOMP_PAD_DEADZONE=7849 on the"
                      " right stick") != NULL);
    out = input_load(NULL, NULL);
    CHECK(strstr(out, "wins over RECOMP_PAD_DEADZONE") == NULL);
    recomp_env_set(RENV_PAD_DEADZONE, NULL);

    /* Mistakes: old spellings, keyboard-only controls, a source twice in one
     * file, too many cells, a bad stick, out of range; guide warns. */
    out = input_load(NULL, "[input]\npad_deadzone_left = 99\nglyphs = \"sega\"\n"
                     "[input.pad]\nltrigger = \"left_trigger\"\na = \"cross\"\n"
                     "left_stick_up = \"dpad_up\"\nb = \"south\"\ny = [\"west\", \"north\", \"east\"]\n"
                     "x = \"south\"\nleft_stick = \"both\"\nback = \"guide\"\n");
    s = pad_now();
    CHECK(strstr(out, "unknown control 'ltrigger' under [input.pad]") != NULL);
    CHECK(strstr(out, "unknown control 'left_stick_up' under [input.pad]") != NULL);
    CHECK(strstr(out, "unknown pad source 'cross'") != NULL);
    CHECK(strstr(out, "'south' already mapped to input.pad.b") != NULL);
    CHECK(strstr(out, "more than 2 sources; 'east' ignored") != NULL);
    CHECK(strstr(out, "left_stick 'both' is not one of left, right, none") != NULL);
    CHECK(strstr(out, "input.pad_deadzone_left 99 out of range 0 .. 95") != NULL);
    CHECK(strstr(out, "glyphs 'sega' is not one of") != NULL);
    CHECK(strstr(out, "'guide': most hosts keep Guide") != NULL);
    CHECK(s.map.source[INPUT_B] == INPUT_SRC_SOUTH && s.map.source[INPUT_X] == INPUT_SRC_NONE);
    CHECK(s.map.source[INPUT_Y] == INPUT_SRC_WEST && s.alt.source[INPUT_Y] == INPUT_SRC_NORTH);
    CHECK(s.map.source[INPUT_A] == INPUT_SRC_NONE);   /* named, nothing valid */
    CHECK(s.map.source[INPUT_BACK] == INPUT_SRC_GUIDE);
    CHECK(s.map.left_stick == INPUT_STICK_LEFT && s.map.deadzone_left == 0);
    CHECK(s_glyphs == XBOX_PAD_FAMILY_AUTO);

    /* RECOMP_CONFIG=none: the identity. */
    recomp_env_set(RENV_CONFIG, "none");
    remove_defaults();
    capture_begin();
    enhance_cfg_init("ecfg_in", NULL);
    enhance_input_init();
    out = capture_end();
    s = pad_now();
    CHECK(xbox_PadSetupIsIdentity(&s));
    recomp_env_set(RENV_CONFIG, NULL);
    remove("ecfg_in/user.toml");
    enhance_cfg_shutdown();
    (void)id;
}

/* A stray key names the mod it belongs to (mod-settings B.11): the
 * longest id that prefixes it, under mods., input.controls. or
 * input.actions. or at the mod's own prefix, at the same boundary as
 * mark_read_prefix. */
static void test_owners(void)
{
    const char *keys[8], *out;

    config_reset_env();
    remove_defaults();
    MKDIR("ecfg_own");
    MKDIR("ecfg_own/config");
    write_file("ecfg_own/config/config.toml",
               "[camera]\nfree = true\nfreecam = 1\n[mods.lod]\ndistnce = 2\n"
               "[input.actions]\n\"cam.toggle\" = [\"F6\"]\n");
    CHECK(enhance_cfg_init("ecfg_own", NULL) == 1);
    enhance_cfg_owner_add(NULL);
    enhance_cfg_owner_add("cam");
    enhance_cfg_owner_add("camera.free");
    enhance_cfg_owner_add("lod");
    CHECK(!strcmp(enhance_cfg_owner_of("camera.free_speed"), "camera.free"));
    CHECK(!strcmp(enhance_cfg_owner_of("mods.lod.distnce"), "lod"));
    CHECK(!strcmp(enhance_cfg_owner_of("input.actions.cam.toggle"), "cam"));
    CHECK(!strcmp(enhance_cfg_owner_of("input.controls.lod.near"), "lod"));
    CHECK(enhance_cfg_owner_of("camera.freecam") == NULL);   /* not at a boundary */
    CHECK(enhance_cfg_owner_of("camera") == NULL);
    CHECK(enhance_cfg_owner_of("lodge.x") == NULL);

    CHECK(enhance_cfg_bool("camera.free", 0) == 1);
    CHECK(enhance_cfg_unused_of("lod", keys, 8) == 1 && !strcmp(keys[0], "mods.lod.distnce"));
    CHECK(enhance_cfg_unused_of("cam", keys, 8) == 1 &&
          !strcmp(keys[0], "input.actions.cam.toggle"));
    CHECK(enhance_cfg_unused_of("camera.free", keys, 8) == 0);
    CHECK(enhance_cfg_unused_of(NULL, keys, 8) == 0);
    capture_begin();
    CHECK(enhance_cfg_report_unused() == 3);
    out = capture_end();
    CHECK(strstr(out, "[ENHANCE] unused key mods.lod.distnce, ecfg_own/config/config.toml:5 (mod lod)\n")
          != NULL);
    CHECK(strstr(out, "[ENHANCE] unused key camera.freecam, ecfg_own/config/config.toml:3\n")
          != NULL);
    enhance_cfg_owner_add(NULL);
    CHECK(enhance_cfg_owner_of("mods.lod.distnce") == NULL);
    enhance_cfg_shutdown();
}

int main(void)
{
    test_subset();
    test_aot();
    test_errors();
    test_quoted_dots();
    test_strict_valid();
    test_locale();
    test_layers();
    test_config_files();
    test_migration();
    test_enhance_init();
    test_present_surface();
    test_tex_pack_key();
    test_input();
    test_input_pad();
    test_exe_dir();
    test_owners();
    printf("enhance_cfg: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail ? 1 : 0;
}
