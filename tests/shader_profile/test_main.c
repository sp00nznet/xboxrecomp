/*
 * shader_profile - see CMakeLists.txt.
 *
 *   shader_profile_test CASE SHADERS_DIR
 *
 * SHADERS_DIR is the toolkit's assets/shaders (the bundled presets, read
 * only). Every file a case writes goes under the working directory, in a
 * folder with a space and a non-ASCII name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#define make_dir(p) _mkdir(p)
#else
#include <unistd.h>
#define make_dir(p) mkdir(p, 0755)
#endif

#include "enhance_cfg.h"
#include "nv2a_present_chain.h"
#include "recomp_cfg.h"
#include "recomp_env.h"
#include "recomp_exe_dir.h"
#include "shader_profile.h"

static int s_fail;

/* kernel_path.c's shader cache folder, as a case sets it ("" before
 * xbox_path_init: no folder). */
static char s_cache[512];
/* A pending prepare that only counts its runs. */
static int s_prepared;
static void count_prepare(void)
{
    s_prepared++;
}

int xbox_path_shader_cache(char *out, size_t n);
int xbox_path_shader_cache(char *out, size_t n)
{
    if (!s_cache[0] || strlen(s_cache) >= n)
        return 0;
    memcpy(out, s_cache, strlen(s_cache) + 1);
    return 1;
}

#define CHECK(c)                                                                     \
    do {                                                                             \
        if (!(c)) {                                                                  \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);             \
            s_fail = 1;                                                              \
        }                                                                            \
    } while (0)

/* A folder with a space and a non-ASCII name (UTF-8 "dé"). */
#define WORK "sp work"
#define DIR_U WORK "/d\xc3\xa9 dir"

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "FAIL cannot write %s\n", path);
        s_fail = 1;
        return;
    }
    fputs(text, f);
    fclose(f);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    if (fread(b, 1, (size_t)n, f) != (size_t)n)
        n = 0;
    b[n] = 0;
    fclose(f);
    return b;
}

static int has(const char *text, const char *line)
{
    return text && strstr(text, line) != NULL;
}

static int count_of(const char *text, const char *needle)
{
    int n = 0;
    for (const char *s = text; s && (s = strstr(s, needle)); s += strlen(needle))
        n++;
    return n;
}

static void dirs(void)
{
    make_dir(WORK);
    make_dir(DIR_U);
}

/* ---- names -------------------------------------------------------------- */

static void case_names(void)
{
    char s[64];
    const char *taken[] = { "soft-crt", "soft-crt-2" };

    CHECK(shader_profile_slug("Soft CRT", s, sizeof s) && !strcmp(s, "soft-crt"));
    CHECK(shader_profile_slug("  FSR + CRT (custom)  ", s, sizeof s)
          && !strcmp(s, "fsr-crt-custom"));
    /* UTF-8 bytes are one '-', never kept. */
    CHECK(shader_profile_slug("Caf\xc3\xa9 Lottes", s, sizeof s) && !strcmp(s, "caf-lottes"));
    CHECK(!shader_profile_slug("\xc3\xa9\xc3\xa9", s, sizeof s));
    CHECK(shader_profile_slug("--a--b--", s, sizeof s) && !strcmp(s, "a-b"));
    CHECK(shader_profile_slug_ok("soft-crt") && shader_profile_slug_ok("a_b"));
    CHECK(!shader_profile_slug_ok("Soft") && !shader_profile_slug_ok("-a")
          && !shader_profile_slug_ok("a b") && !shader_profile_slug_ok("")
          && !shader_profile_slug_ok("~unsaved"));
    CHECK(shader_profile_name_ok("Caf\xc3\xa9 Lottes"));
    CHECK(!shader_profile_name_ok("bad \xc3"));          /* cut UTF-8 */
    CHECK(!shader_profile_name_ok("tab\there"));
    CHECK(!shader_profile_name_ok("\xc3\xa9"));           /* no slug */
    CHECK(!shader_profile_name_ok("0123456789012345678901234567890123456789012345678"));
    shader_profile_slug_free("Soft CRT", taken, 2, s, sizeof s);
    CHECK(!strcmp(s, "soft-crt-3"));
    shader_profile_slug_free("Other", taken, 2, s, sizeof s);
    CHECK(!strcmp(s, "other"));
    shader_profile_slug_free("\xc3\xa9", taken, 2, s, sizeof s);
    CHECK(!strcmp(s, "profile"));
}

/* ---- parse -------------------------------------------------------------- */

static const char k_example[] =
    "# xboxrecomp shader profile\n"
    "version = 1\n"
    "name = \"Soft CRT\"\n"
    "stages = [\"fsr\", \"crt-lottes\"]\n"
    "\n"
    "[params.fsr]\n"
    "FSR_SHARPENING = 0.5\n"
    "\n"
    "[params.crt-lottes]\n"
    "hardScan = -10\n";

static void case_parse(void)
{
    static struct shader_profile p;
    char err[256];
    float v;
    int r;

    r = shader_profile_parse(k_example, "soft-crt.toml", "soft-crt", &p, err, sizeof err);
    CHECK(r == SHADER_PROFILE_OK);
    CHECK(!strcmp(p.name, "Soft CRT") && !strcmp(p.slug, "soft-crt") && p.version == 1);
    CHECK(p.nstages == 2 && !strcmp(p.stage[0], "fsr") && !strcmp(p.stage[1], "crt-lottes"));
    CHECK(p.nvalues == 2);
    CHECK(shader_profile_value_get(&p, "fsr", "FSR_SHARPENING", &v) && v == 0.5f);
    CHECK(shader_profile_value_get(&p, "crt-lottes", "hardScan", &v) && v == -10.0f);

    /* version: missing, wrong type, below 1, higher */
    CHECK(shader_profile_parse("stages = [\"fsr\"]\n", "a", NULL, &p, err, sizeof err)
          == SHADER_PROFILE_BROKEN && has(err, "no version"));
    CHECK(shader_profile_parse("version = \"1\"\nstages = [\"fsr\"]\n", "a", NULL, &p, err,
                               sizeof err) == SHADER_PROFILE_BROKEN);
    CHECK(shader_profile_parse("version = 0\nstages = [\"fsr\"]\n", "a", NULL, &p, err,
                               sizeof err) == SHADER_PROFILE_BROKEN);
    CHECK(shader_profile_parse("version = 2\nname = \"N\"\nstages = [\"fsr\"]\nnew = 1\n", "a",
                               NULL, &p, err, sizeof err) == SHADER_PROFILE_NEWER);
    CHECK(!strcmp(p.name, "N") && p.nstages == 1);
    /* a newer format with stages of another shape still lists */
    CHECK(shader_profile_parse("version = 3\nstages = \"fsr\"\n", "a", "x", &p, err,
                               sizeof err) == SHADER_PROFILE_NEWER);
    /* empty or missing stages; a stage that is not a string */
    CHECK(shader_profile_parse("version = 1\nstages = []\n", "a", NULL, &p, err, sizeof err)
          == SHADER_PROFILE_BROKEN);
    CHECK(shader_profile_parse("version = 1\n", "a", NULL, &p, err, sizeof err)
          == SHADER_PROFILE_BROKEN);
    CHECK(shader_profile_parse("version = 1\nstages = [\"fsr\", 3]\n", "a", NULL, &p, err,
                               sizeof err) == SHADER_PROFILE_BROKEN);
    CHECK(shader_profile_parse("version = 1\nname = 4\nstages = [\"fsr\"]\n", "a", NULL, &p,
                               err, sizeof err) == SHADER_PROFILE_BROKEN);
    /* does not parse */
    CHECK(shader_profile_parse("version = = 1\n", "a", NULL, &p, err, sizeof err)
          == SHADER_PROFILE_BROKEN && err[0]);
    /* 9 stages: the first 8 */
    r = shader_profile_parse("version = 1\nstages = [\"a\",\"b\",\"c\",\"d\",\"e\",\"f\","
                             "\"g\",\"h\",\"i\"]\n", "a", NULL, &p, err, sizeof err);
    CHECK(r == SHADER_PROFILE_OK && p.nstages == 8 && !strcmp(p.stage[7], "h"));
    /* unknown keys are reported, not fatal; a value that is not a number is
     * skipped */
    r = shader_profile_parse("version = 1\nstages = [\"fsr\"]\nfuture = true\n"
                             "[params.fsr]\nFSR_SHARPENING = \"high\"\nFSR_FILMGRAIN = 0.1\n",
                             "a", NULL, &p, err, sizeof err);
    CHECK(r == SHADER_PROFILE_OK && p.nvalues == 1);
    CHECK(shader_profile_value_get(&p, "fsr", "FSR_FILMGRAIN", &v));
    CHECK(!shader_profile_value_get(&p, "fsr", "FSR_SHARPENING", &v));
    /* no name: the slug */
    CHECK(shader_profile_parse("version = 1\nstages = [\"fsr\"]\n", "a", "my-fsr", &p, err,
                               sizeof err) == SHADER_PROFILE_OK && !strcmp(p.name, "my-fsr"));
    /* a long name is cut at a character boundary: 47 ASCII then a 2-byte é */
    {
        char t[256];
        snprintf(t, sizeof t, "version = 1\nname = \"%s\xc3\xa9x\"\nstages = [\"fsr\"]\n",
                 "01234567890123456789012345678901234567890123456");
        CHECK(shader_profile_parse(t, "a", "x", &p, err, sizeof err) == SHADER_PROFILE_OK);
        CHECK(strlen(p.name) == 47);
    }
}

/* ---- save --------------------------------------------------------------- */

static void case_save(void)
{
    static struct shader_profile p, q;
    char err[256], path[512], *t;

    dirs();
    snprintf(path, sizeof path, "%s/profiles/soft-crt.toml", DIR_U);
    remove(path);
    CHECK(shader_profile_parse(k_example, "x", "soft-crt", &p, err, sizeof err) == 0);
    /* a new file: header, folder made */
    CHECK(shader_profile_save(path, &p, err, sizeof err) == 0);
    t = read_file(path);
    CHECK(has(t, "# xboxrecomp shader profile"));
    CHECK(has(t, "version = 1\n") && has(t, "name = \"Soft CRT\"\n")
          && has(t, "stages = [\"fsr\", \"crt-lottes\"]\n"));
    CHECK(has(t, "[params.fsr]\nFSR_SHARPENING = 0.5\n"));
    CHECK(has(t, "[params.crt-lottes]\nhardScan = -10\n"));
    free(t);
    CHECK(shader_profile_load(path, &q, err, sizeof err) == 0 && q.nvalues == 2
          && !strcmp(q.slug, "soft-crt"));

    /* a round trip keeps comments and unknown keys; reorder changes only
     * stages; a value removed leaves the file */
    write_file(path,
               "# my notes\nversion = 1\nname = \"Soft CRT\"  # the name\n"
               "stages = [\"fsr\", \"crt-lottes\"]\nmine = 7\n\n"
               "[params.fsr]\n# sharper\nFSR_SHARPENING = 0.5\n\n"
               "[params.crt-lottes]\nhardScan = -10\n");
    CHECK(shader_profile_load(path, &p, err, sizeof err) == 0);
    snprintf(p.stage[0], sizeof p.stage[0], "crt-lottes");
    snprintf(p.stage[1], sizeof p.stage[1], "fsr");
    CHECK(shader_profile_save(path, &p, err, sizeof err) == 0);
    t = read_file(path);
    CHECK(has(t, "# my notes\n") && has(t, "# the name") && has(t, "mine = 7\n")
          && has(t, "# sharper\n"));
    CHECK(has(t, "stages = [\"crt-lottes\", \"fsr\"]"));
    CHECK(has(t, "FSR_SHARPENING = 0.5\n") && has(t, "hardScan = -10\n"));
    free(t);
    shader_profile_value_remove(&p, "crt-lottes", "hardScan");
    shader_profile_value_set(&p, "fsr", "FSR_SHARPENING", 0.25f);
    CHECK(shader_profile_save(path, &p, err, sizeof err) == 0);
    t = read_file(path);
    CHECK(!has(t, "hardScan") && has(t, "FSR_SHARPENING = 0.25\n") && has(t, "mine = 7\n"));
    free(t);

    /* [param_kinds.X] as the last table: Save keeps it whole, and new
     * values land under their own [params] tables, not inside it. */
    write_file(path, "version = 1\nname = \"Soft CRT\"\nstages = [\"fsr\", \"crt-lottes\"]\n\n"
                     "[params.fsr]\nFSR_SHARPENING = 0.5\n\n"
                     "[param_kinds.MASK]\nkind = \"toggle\"\nhelp = \"Mine\"\n");
    CHECK(shader_profile_load(path, &p, err, sizeof err) == 0 && p.nvalues == 1);
    shader_profile_value_set(&p, "fsr", "FSR_GRAIN", 1.0f);
    shader_profile_value_set(&p, "crt-lottes", "hardScan", -8.0f);
    CHECK(shader_profile_save(path, &p, err, sizeof err) == 0);
    t = read_file(path);
    CHECK(has(t, "[param_kinds.MASK]\nkind = \"toggle\"\nhelp = \"Mine\"\n"));
    free(t);
    CHECK(shader_profile_load(path, &q, err, sizeof err) == 0 && q.nvalues == 3);
    {
        int seen = 0;
        for (int i = 0; i < q.nvalues; i++)
            seen += (!strcmp(q.value[i].key, "fsr") && !strcmp(q.value[i].name, "FSR_GRAIN")
                     && q.value[i].v == 1.0f)
                    + (!strcmp(q.value[i].key, "crt-lottes")
                       && !strcmp(q.value[i].name, "hardScan") && q.value[i].v == -8.0f);
        CHECK(seen == 2);
    }
    {
        recomp_cfg *c = recomp_cfg_load_file(path, err, sizeof err);
        CHECK(c && !strcmp(recomp_cfg_string(c, "param_kinds.MASK.kind", ""), "toggle"));
        CHECK(c && recomp_cfg_type_of(c, "param_kinds.MASK.FSR_GRAIN") == RCFG_NONE
              && recomp_cfg_type_of(c, "param_kinds.MASK.hardScan") == RCFG_NONE);
        recomp_cfg_free(c);
    }

    /* never written: a file that does not parse, a newer one */
    write_file(path, "version = = 1\n");
    CHECK(shader_profile_save(path, &p, err, sizeof err) == -1);
    t = read_file(path);
    CHECK(t && !strcmp(t, "version = = 1\n"));
    free(t);
    write_file(path, "version = 2\nstages = [\"fsr\"]\n");
    CHECK(shader_profile_save(path, &p, err, sizeof err) == -1 && has(err, "newer"));
    p.nstages = 0;
    CHECK(shader_profile_save(path, &p, err, sizeof err) == -1);

    /* rename and delete */
    {
        char dir[512], a[512], b[512];
        snprintf(dir, sizeof dir, "%s/profiles", DIR_U);
        shader_profile_path(dir, "one", a, sizeof a);
        shader_profile_path(dir, "two", b, sizeof b);
        remove(a);
        remove(b);
        write_file(a, k_example);
        CHECK(shader_profile_rename(dir, "one", "two", err, sizeof err) == 0);
        CHECK(shader_profile_load(b, &q, err, sizeof err) == 0);
        write_file(a, k_example);
        CHECK(shader_profile_rename(dir, "one", "two", err, sizeof err) == -1);  /* exists */
        CHECK(shader_profile_rename(dir, "one", "Bad Name", err, sizeof err) == -1);
        CHECK(shader_profile_delete(dir, "one", err, sizeof err) == 0);
        CHECK(shader_profile_delete(dir, "one", err, sizeof err) == -1);
        CHECK(shader_profile_delete(dir, "two", err, sizeof err) == 0);
    }
}

/* ---- list --------------------------------------------------------------- */

static void case_list(void)
{
    static struct shader_profile_entry e[4];
    char dir[512], p[600];
    char stems[8][64];
    int n;

    dirs();
    snprintf(dir, sizeof dir, "%s/list", DIR_U);
    make_dir(dir);
    snprintf(p, sizeof p, "%s/zeta.toml", dir);
    write_file(p, "version = 1\nname = \"alpha\"\nstages = [\"fsr\"]\n");
    snprintf(p, sizeof p, "%s/beta.toml", dir);
    write_file(p, "version = = 1\n");
    snprintf(p, sizeof p, "%s/gamma.toml", dir);
    write_file(p, "version = 9\nname = \"Gamma\"\nstages = [\"fsr\"]\n");
    snprintf(p, sizeof p, "%s/Not A Slug.toml", dir);
    write_file(p, "version = 1\nstages = [\"fsr\"]\n");
    snprintf(p, sizeof p, "%s/delta.toml.tmp", dir);
    write_file(p, "version = 1\nstages = [\"fsr\"]\n");
    snprintf(p, sizeof p, "%s/epsilon.toml", dir);
    write_file(p, "version = 1\nname = \"Epsilon\"\nstages = [\"fsr\", \"crt-lottes\"]\n");
    snprintf(p, sizeof p, "%s/eta.toml", dir);
    write_file(p, "version = 1\nname = \"eta\"\nstages = [\"fsr\"]\n");

    n = shader_profile_list(dir, e, 4);
    /* alpha, beta (broken, named by its slug), Epsilon, eta; Gamma past cap */
    CHECK(n == 4);
    CHECK(!strcmp(e[0].name, "alpha") && !strcmp(e[0].slug, "zeta")
          && e[0].state == SHADER_PROFILE_OK);
    CHECK(!strcmp(e[1].slug, "beta") && e[1].state == SHADER_PROFILE_BROKEN && e[1].err[0]);
    CHECK(!strcmp(e[2].name, "Epsilon") && e[2].nstages == 2);
    CHECK(!strcmp(e[3].name, "eta"));
    n = shader_profile_list(dir, e, 64 > 4 ? 4 : 4);
    {
        static struct shader_profile_entry all[SHADER_PROFILE_LIST_MAX];
        n = shader_profile_list(dir, all, SHADER_PROFILE_LIST_MAX);
        CHECK(n == 5 && !strcmp(all[4].name, "Gamma") && all[4].state == SHADER_PROFILE_NEWER);
    }
    snprintf(p, sizeof p, "%s/none-here", DIR_U);
    CHECK(shader_profile_list(p, e, 4) == 0);

    /* the Add picker's user presets: sorted, bundled stems left out */
    snprintf(dir, sizeof dir, "%s/shaders", DIR_U);
    make_dir(dir);
    snprintf(p, sizeof p, "%s/zz.slangp", dir);
    write_file(p, "shaders = 0\n");
    snprintf(p, sizeof p, "%s/aa.slangp", dir);
    write_file(p, "shaders = 0\n");
    snprintf(p, sizeof p, "%s/fsr.slangp", dir);
    write_file(p, "shaders = 0\n");
    n = shader_profile_list_presets(dir, stems, 8);
    CHECK(n == 2 && !strcmp(stems[0], "aa") && !strcmp(stems[1], "zz"));
}

/* ---- stages ------------------------------------------------------------- */

static void case_stages(const char *shaders)
{
    char root[512], exe[1024], out[1024], err[256], k[64], p[1100];

    dirs();
    /* An exe dir with shaders/ (the bundled presets' place). */
    snprintf(exe, sizeof exe, "%s/exe", DIR_U);
    make_dir(exe);
    snprintf(p, sizeof p, "%s/shaders", exe);
    make_dir(p);
    snprintf(p, sizeof p, "%s/shaders/fsr.slangp", exe);
    write_file(p, "shaders = 1\n");
    snprintf(p, sizeof p, "%s/beside.slangp", exe);
    write_file(p, "shaders = 1\n");
    snprintf(root, sizeof root, "%s/root", DIR_U);
    make_dir(root);
    snprintf(p, sizeof p, "%s/shaders", root);
    make_dir(p);
    snprintf(p, sizeof p, "%s/shaders/mine.slangp", root);
    write_file(p, "shaders = 1\n");
    snprintf(p, sizeof p, "%s/shaders/beside.slangp", root);
    write_file(p, "shaders = 1\n");
    snprintf(p, sizeof p, "%s/shaders/crt-lottes.slangp", root);
    write_file(p, "shaders = 1\n");
    (void)shaders;

    CHECK(shader_profile_bundled("fsr") == 3 && shader_profile_bundled("mine") < 0);
    CHECK(shader_profile_stage_path("fsr", root, exe, out, sizeof out, err, sizeof err) == 1);
    snprintf(p, sizeof p, "%s/shaders/fsr.slangp", exe);
    CHECK(!strcmp(out, p));
    /* a bundled name missing beside the exe is an error, never a user file */
    CHECK(shader_profile_stage_path("crt-lottes", root, exe, out, sizeof out, err, sizeof err)
          == -1);
    CHECK(shader_profile_stage_path("user:mine", root, exe, out, sizeof out, err, sizeof err)
          == 1);
    snprintf(p, sizeof p, "%s/shaders/mine.slangp", root);
    CHECK(!strcmp(out, p));
    CHECK(shader_profile_stage_path("user:crt-lottes", root, exe, out, sizeof out, err,
                                    sizeof err) == -1 && has(err, "clashes"));
    CHECK(shader_profile_stage_path("user:none", root, exe, out, sizeof out, err, sizeof err)
          == -1);
    CHECK(shader_profile_stage_path("user:a/b", root, exe, out, sizeof out, err, sizeof err)
          == -1);
    /* relative: <root>/shaders/ before the exe dir */
    CHECK(shader_profile_stage_path("beside.slangp", root, exe, out, sizeof out, err,
                                    sizeof err) == 1);
    snprintf(p, sizeof p, "%s/shaders/beside.slangp", root);
    CHECK(!strcmp(out, p));
    CHECK(shader_profile_stage_path("crt-lottes.slangp", root, exe, out, sizeof out, err,
                                    sizeof err) == -1 && has(err, "clashes"));
    snprintf(p, sizeof p, "%s/shaders/beside.slangp", root);
    remove(p);
    CHECK(shader_profile_stage_path("beside.slangp", root, exe, out, sizeof out, err,
                                    sizeof err) == 1);
    snprintf(p, sizeof p, "%s/beside.slangp", exe);
    CHECK(!strcmp(out, p));
    /* absolute */
    {
        char abs[1200];
#ifdef _WIN32
        _fullpath(abs, p, sizeof abs);
#else
        char cwd[1024];
        CHECK(getcwd(cwd, sizeof cwd) != NULL);
        snprintf(abs, sizeof abs, "%s/%s", cwd, p);
#endif
        CHECK(shader_profile_stage_path(abs, root, exe, out, sizeof out, err, sizeof err) == 1);
    }
    CHECK(shader_profile_stage_path("", root, exe, out, sizeof out, err, sizeof err) == -1);
    /* root "." in the exe's own directory: <root>/shaders/ is the bundled
     * folder, and a bundled preset there is itself, no clash. */
    CHECK(shader_profile_stage_path("user:fsr", exe, exe, out, sizeof out, err, sizeof err)
          == 1);
    CHECK(shader_profile_stage_path("fsr.slangp", exe, exe, out, sizeof out, err, sizeof err)
          == 1);

    shader_profile_stage_key("user:mine", k, sizeof k);
    CHECK(!strcmp(k, "mine"));
    shader_profile_stage_key("fsr", k, sizeof k);
    CHECK(!strcmp(k, "fsr"));
    shader_profile_stage_key("x/My Preset.slangp", k, sizeof k);
    CHECK(k[0] && !strchr(k, '/'));
}

/* ---- shared values ------------------------------------------------------ */

static void case_rehome(void)
{
    static struct shader_profile p;
    const char *a[] = { "MASK", "A1" }, *b[] = { "MASK", "B1" };
    struct shader_profile_stage_params sp[2] = { { 2, a }, { 2, b } };
    float v;

    memset(&p, 0, sizeof p);
    p.nstages = 2;
    snprintf(p.stage[0], sizeof p.stage[0], "crt-lottes");
    snprintf(p.stage[1], sizeof p.stage[1], "crt-hyllian");
    CHECK(shader_profile_owner(&p, sp, "MASK") == 0);
    CHECK(shader_profile_owner(&p, sp, "B1") == 1);
    CHECK(shader_profile_owner(&p, sp, "nope") == -1);

    /* a value under a non-owner's table is read and moved to the owner */
    shader_profile_value_set(&p, "crt-hyllian", "MASK", 0.7f);
    shader_profile_value_set(&p, "crt-hyllian", "B1", 2);
    shader_profile_value_set(&p, "gone", "X", 1);
    shader_profile_rehome(&p, sp, "t");
    CHECK(shader_profile_value_get(&p, "crt-lottes", "MASK", &v) && v == 0.7f);
    CHECK(!shader_profile_value_get(&p, "crt-hyllian", "MASK", &v));
    CHECK(shader_profile_value_get(&p, "crt-hyllian", "B1", &v) && v == 2);
    CHECK(shader_profile_value_get(&p, "gone", "X", &v));   /* nobody declares it: kept */
    CHECK(p.nvalues == 3);

    /* two copies: the owner's wins, the other goes */
    shader_profile_value_set(&p, "crt-hyllian", "MASK", 0.1f);
    shader_profile_rehome(&p, sp, "t");
    CHECK(shader_profile_value_get(&p, "crt-lottes", "MASK", &v) && v == 0.7f);
    CHECK(!shader_profile_value_get(&p, "crt-hyllian", "MASK", &v));

    /* reorder: the owner changes, the value follows */
    snprintf(p.stage[0], sizeof p.stage[0], "crt-hyllian");
    snprintf(p.stage[1], sizeof p.stage[1], "crt-lottes");
    sp[0].name = b;
    sp[1].name = a;
    shader_profile_rehome(&p, sp, "t");
    CHECK(shader_profile_value_get(&p, "crt-hyllian", "MASK", &v) && v == 0.7f);
    CHECK(!shader_profile_value_get(&p, "crt-lottes", "MASK", &v));
    CHECK(p.nvalues == 3);

    shader_profile_value_remove(&p, "crt-hyllian", NULL);
    CHECK(p.nvalues == 1);
}

/* ---- compose: synthetic presets ----------------------------------------- */

static void mk(const char *name, const char *text)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", DIR_U, name);
    write_file(p, text);
}

static char *compose(const char *const *names, int n, struct shader_profile_compose *info,
                     char *err, size_t errlen)
{
    char paths[SHADER_PROFILE_STAGES_MAX + 2][512];
    const char *pp[SHADER_PROFILE_STAGES_MAX + 2];
    for (int i = 0; i < n; i++) {
        snprintf(paths[i], sizeof paths[i], "%s/%s", DIR_U, names[i]);
        pp[i] = paths[i];
    }
    return shader_profile_compose_text(pp, names, n, "test", info, err, errlen);
}

static void case_compose(void)
{
    struct shader_profile_compose info;
    char err[256], *t;

    dirs();
    mk("a.slang", "#pragma parameter A1 \"A\" 0.5 0.0 1.0 0.1\n"
                  "layout(std140) uniform UBO { vec4 OriginalSize; } global;\n");
    mk("a.slangp", "shaders = 1\nshader0 = a.slang\nalias0 = AOUT\n"
                   "parameters = \"A1\"\nA1 = 0.25\n");
    mk("b.slang", "uniform sampler2D Original;\nvoid main() { c = texture(Original, v); }\n");
    mk("b.slangp", "shaders = \"1\"\nshader0 = \"b.slang\"\nscale_type0 = source\n"
                   "scale0 = 2.0\nA1 = 0.75\nB1 = 3\n");
    mk("c.slang", "// PassOutput0 in a comment is fine\nvoid main() {}\n");
    mk("c.slangp", "shaders = 1\nshader0 = c.slang\nfilter_linear0 = true\n");
    mk("p.slang", "#include \"p.h\"\n");
    mk("p.h", "/* reads */ vec4 x = texture(PassOutput0, v);\n");
    mk("p.slangp", "shaders = 1\nshader0 = p.slang\n");
    mk("fb.slang", "vec4 x = texture(PassFeedback0, v);\n");
    mk("fb.slangp", "shaders = 1\nshader0 = fb.slang\n");
    mk("t1.slangp", "shaders = 1\nshader0 = c.slang\ntextures = \"LUT\"\nLUT = lut.png\n"
                    "LUT_linear = true\n");
    mk("t2.slangp", "shaders = 1\nshader0 = c.slang\ntextures = \"LUT\"\nLUT = lut.png\n");
    mk("c1.slangp", "#reference \"c2.slangp\"\n");
    mk("c2.slangp", "#reference \"c1.slangp\"\n");
    mk("w.slangp", "#reference \"a.slangp\"\nA1 = \"0.9\"\n");
    mk("big.slangp", "shaders = 9\nshader0 = c.slang\nshader1 = c.slang\nshader2 = c.slang\n"
                     "shader3 = c.slang\nshader4 = c.slang\nshader5 = c.slang\n"
                     "shader6 = c.slang\nshader7 = c.slang\nshader8 = c.slang\n");
    mk("missing.slangp", "shaders = 1\nshader0 = nope.slang\n");
    /* A parameter named like a pass key; a stage's own feedback_pass; a
     * sampler declared, never read. */
    mk("sp.slangp", "shaders = 1\nshader0 = c.slang\nparameters = \"scale1\"\nscale1 = 0.3\n");
    mk("fp.slangp", "shaders = 1\nshader0 = c.slang\nfeedback_pass = 0\n");
    mk("dc.slang", "layout(set = 0, binding = 2) uniform sampler2D PassOutput0;\n"
                   "layout(push_constant) uniform Push { vec4 PassOutputSize0; } params;\n");
    mk("dc.slangp", "shaders = 1\nshader0 = dc.slang\n");
    /* #include: a file including itself, and a diamond twelve deep (each
     * level includes the next twice) with a read at its bottom. */
    mk("self.slang", "#include \"self.slang\"\nvoid main() {}\n");
    mk("self.slangp", "shaders = 1\nshader0 = self.slang\n");
    for (int i = 0; i < 12; i++) {
        char nm[32], tx[96];
        snprintf(nm, sizeof nm, "i%d.h", i);
        snprintf(tx, sizeof tx, "#include \"i%d.h\"\n#include \"i%d.h\"\n", i + 1, i + 1);
        mk(nm, tx);
    }
    mk("i12.h", "vec4 x = texture(PassOutput1, v);\n");
    mk("dia.slang", "#include \"i0.h\"\n");
    mk("dia.slangp", "shaders = 1\nshader0 = dia.slang\n");
    /* #reference 17 deep */
    for (int i = 0; i < 17; i++) {
        char nm[32], tx[64];
        snprintf(nm, sizeof nm, "d%d.slangp", i);
        snprintf(tx, sizeof tx, "#reference \"d%d.slangp\"\n", i + 1);
        mk(nm, tx);
    }
    mk("d17.slangp", "shaders = 1\nshader0 = c.slang\n");

    {
        const char *n[] = { "a.slangp", "b.slangp", "c.slangp" };
        t = compose(n, 3, &info, err, sizeof err);
        CHECK(t != NULL);
        CHECK(has(t, "shaders = 3\n"));
        CHECK(has(t, "# stage 1: a.slangp\n") && has(t, "# stage 3: c.slangp\n"));
        /* absolute paths through the folder with a space and a non-ASCII name */
        CHECK(has(t, "/sp work/d\xc3\xa9 dir/a.slang\"\n"));
        CHECK(count_of(t, "shader0 = \"/") + count_of(t, "shader0 = \"") == 2);
        CHECK(has(t, "alias0 = \"AOUT\"\n"));
        /* a non-final stage keeps its standalone viewport output */
        CHECK(has(t, "scale_type0 = \"viewport\"\n"));
        CHECK(has(t, "scale_type1 = \"source\"\n") && has(t, "scale1 = \"2.0\"\n"));
        CHECK(!has(t, "scale_type2"));
        CHECK(has(t, "filter_linear2 = \"true\"\n"));
        /* the earlier stage's value wins; B1 (no list) kept */
        CHECK(has(t, "parameters = \"A1\"\n") && has(t, "A1 = \"0.25\"\n")
              && !has(t, "A1 = \"0.75\"") && has(t, "B1 = \"3\"\n"));
        CHECK(info.passes == 3 && info.nstages == 3);
        CHECK(info.stage[0].flags == 0);
        /* b reads Original; a only declares OriginalSize */
        CHECK(info.stage[1].flags == (SHADER_PROFILE_SCALED | SHADER_PROFILE_ORIGINAL));
        CHECK(info.stage[2].flags == SHADER_PROFILE_SCALED);
        CHECK(info.stage[2].first_pass == 2);
        free(t);
    }
    {
        /* a wrapper's own line overrides its reference's */
        const char *n[] = { "w.slangp" };
        t = compose(n, 1, &info, err, sizeof err);
        CHECK(t && has(t, "A1 = \"0.9\"\n") && !has(t, "0.25"));
        free(t);
    }
    {
        const char *n[] = { "c.slangp", "p.slangp" };
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "PassOutput0"));
        n[0] = "p.slangp";
        n[1] = "c.slangp";
        t = compose(n, 2, &info, err, sizeof err);   /* a first stage may */
        CHECK(t != NULL);
        free(t);
        n[0] = "c.slangp";
        n[1] = "fb.slangp";
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "PassFeedback0"));
    }
    {
        const char *n[] = { "c.slangp", "sp.slangp" };
        t = compose(n, 2, &info, err, sizeof err);
        /* a value, not stage 2's pass 1 */
        CHECK(t && has(t, "scale1 = \"0.3\"\n") && !has(t, "scale2"));
        free(t);
        n[1] = "fp.slangp";
        t = compose(n, 2, &info, err, sizeof err);
        CHECK(t && !has(t, "feedback_pass"));
        CHECK(info.stage[0].flags == 0 && (info.stage[1].flags & SHADER_PROFILE_NO_FEEDBACK));
        free(t);
        n[1] = "dc.slangp";
        t = compose(n, 2, &info, err, sizeof err);
        CHECK(t != NULL);
        free(t);
        n[1] = "self.slangp";
        t = compose(n, 2, &info, err, sizeof err);
        CHECK(t != NULL);
        free(t);
        n[1] = "dia.slangp";
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "PassOutput1"));
    }
    {
        const char *n[] = { "a.slangp", "a.slangp" };
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "alias"));
        n[0] = "t1.slangp";
        n[1] = "t2.slangp";
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "texture"));
        n[1] = "c.slangp";
        t = compose(n, 2, &info, err, sizeof err);
        CHECK(t && has(t, "textures = \"LUT\"\n") && has(t, "/lut.png\"\n")
              && has(t, "LUT_linear = \"true\"\n"));
        free(t);
    }
    {
        const char *n[] = { "c1.slangp" };
        CHECK(!compose(n, 1, &info, err, sizeof err) && has(err, "cycle"));
        n[0] = "d0.slangp";
        CHECK(!compose(n, 1, &info, err, sizeof err) && has(err, "deeper"));
        n[0] = "d2.slangp";   /* 15 deep */
        t = compose(n, 1, &info, err, sizeof err);
        CHECK(t != NULL);
        free(t);
        n[0] = "missing.slangp";
        CHECK(!compose(n, 1, &info, err, sizeof err) && has(err, "no such file"));
        n[0] = "none.slangp";
        CHECK(!compose(n, 1, &info, err, sizeof err) && has(err, "cannot read"));
    }
    {
        const char *n[] = { "big.slangp", "big.slangp" };
        CHECK(!compose(n, 2, &info, err, sizeof err) && has(err, "limit is 16"));
    }
    {
        /* the write: through a temporary, folder made */
        char out[512], *f;
        const char *pp[1];
        char p0[512];
        snprintf(p0, sizeof p0, "%s/c.slangp", DIR_U);
        pp[0] = p0;
        shader_profile_cache_path(DIR_U "/cache", NULL, out, sizeof out);
        CHECK(has(out, "/cache/slangp/~unsaved.slangp"));
        shader_profile_cache_path(DIR_U "/cache", "soft-crt", out, sizeof out);
        CHECK(shader_profile_compose(pp, NULL, 1, "soft-crt", out, &info, err, sizeof err) == 0);
        f = read_file(out);
        CHECK(has(f, "shader profile soft-crt") && has(f, "shaders = 1\n"));
        free(f);
    }
}

/* ---- compose: the bundled presets --------------------------------------- */

static void bundled(const char *shaders, const char *name, char *out, size_t cap)
{
    snprintf(out, cap, "%s/%s.slangp", shaders, name);
}

static char *compose_bundled(const char *shaders, const char *const *names, int n,
                             struct shader_profile_compose *info, char *err, size_t errlen)
{
    char paths[4][1024];
    const char *pp[4];
    for (int i = 0; i < n; i++) {
        bundled(shaders, names[i], paths[i], sizeof paths[i]);
        pp[i] = paths[i];
    }
    return shader_profile_compose_text(pp, names, n, "t", info, err, errlen);
}

static void case_bundled(const char *shaders)
{
    struct shader_profile_compose info;
    char err[256], *t;

    /* each alone: its pass settings copied through */
    for (int i = 0; i < 4; i++) {
        static const char *const names[] = { "crt-lottes", "crt-lottes-fast", "crt-hyllian",
                                             "fsr" };
        const char *n[] = { names[i] };
        t = compose_bundled(shaders, n, 1, &info, err, sizeof err);
        CHECK(t != NULL);
        if (!t) {
            fprintf(stderr, "  %s: %s\n", names[i], err);
            continue;
        }
        CHECK(has(t, "scale_type0 = \"viewport\"\n"));
        CHECK(info.passes == (i == 3 ? 2 : 1));
        free(t);
    }
    {
        const char *n[] = { "fsr", "crt-lottes" };
        t = compose_bundled(shaders, n, 2, &info, err, sizeof err);
        CHECK(t != NULL);
        CHECK(has(t, "shaders = 3\n"));
        CHECK(has(t, "/edge-smoothing/fsr/shaders/fsr-pass0.slang\"\n")
              && has(t, "/edge-smoothing/fsr/shaders/fsr-pass1.slang\"\n")
              && has(t, "shader2 = \"") && has(t, "/crt/shaders/crt-lottes.slang\"\n"));
        CHECK(!has(t, "/./") && !has(t, "/../"));
        CHECK(has(t, "scale_type1 = \"source\"\n") && has(t, "scale_type2 = \"viewport\"\n"));
        CHECK(has(t, "parameters = \"FSR_SHARPENING;FSR_FILMGRAIN;FSR_GRAINCOLOR;FSR_GRAINPDF\""));
        CHECK(has(t, "FSR_SHARPENING = \"0.3\"\n"));
        CHECK(has(t, "warpX = \"0.01\"\n") && has(t, "warpY = \"0.02\"\n"));
        CHECK(info.stage[1].first_pass == 2 && info.stage[1].flags == SHADER_PROFILE_SCALED);
        free(t);
    }
    {
        /* fsr twice: one list, one value each */
        const char *n[] = { "fsr", "fsr" };
        t = compose_bundled(shaders, n, 2, &info, err, sizeof err);
        CHECK(t && has(t, "shaders = 4\n"));
        CHECK(count_of(t, "FSR_SHARPENING") == 2);   /* the list and one value */
        CHECK(count_of(t, "parameters = ") == 1);
        free(t);
    }
    {
        /* crt-hyllian reads OriginalSize: flagged after fsr */
        const char *n[] = { "fsr", "crt-hyllian" };
        t = compose_bundled(shaders, n, 2, &info, err, sizeof err);
        CHECK(t && info.stage[1].flags == (SHADER_PROFILE_SCALED | SHADER_PROFILE_ORIGINAL));
        free(t);
        n[0] = "crt-hyllian";
        n[1] = "fsr";   /* fsr declares OriginalSize, never reads it */
        t = compose_bundled(shaders, n, 2, &info, err, sizeof err);
        CHECK(t && info.stage[1].flags == SHADER_PROFILE_SCALED);
        free(t);
    }
}

/* ---- paths -------------------------------------------------------------- */

static void case_paths(void)
{
    char o[512];
    /* a C:\-style string on any host */
    shader_profile_path_join("C:\\Games\\BLiNX 2\\shaders", "crt\\..\\crt\\.\\x.slang", 1, o,
                             sizeof o);
    CHECK(!strcmp(o, "C:/Games/BLiNX 2/shaders/crt/x.slang"));
    shader_profile_path_join("C:\\a", "D:\\b\\c.slang", 1, o, sizeof o);
    CHECK(!strcmp(o, "D:/b/c.slang"));
    shader_profile_path_join("/a/b", "../../../c", 0, o, sizeof o);
    CHECK(!strcmp(o, "/c"));
    shader_profile_path_join("/a//b/", "./c", 0, o, sizeof o);
    CHECK(!strcmp(o, "/a/b/c"));
    shader_profile_path_join("a", "../../c", 0, o, sizeof o);
    CHECK(!strcmp(o, "../c"));
    shader_profile_path_join("\\\\server\\share", "x", 1, o, sizeof o);
    CHECK(!strcmp(o, "//server/share/x"));
}

/* ---- the profiles folder ------------------------------------------------ */

static void case_user_dir(void)
{
    char d[512];

    dirs();
    recomp_env_set(RENV_CONFIG, NULL);
    CHECK(!shader_profile_dir(d, sizeof d));   /* before init */
    CHECK(enhance_cfg_init(DIR_U, NULL) >= 0);
    CHECK(shader_profile_dir(d, sizeof d) && !strcmp(d, DIR_U "/config/shader-profiles"));
    /* a per-title file does not move it */
    {
        char t[512];
        snprintf(t, sizeof t, "%s/TITLE", DIR_U);
        make_dir(t);
        snprintf(t, sizeof t, "%s/TITLE/config.toml", DIR_U);
        write_file(t, "render.scale = 2\n");
        CHECK(enhance_cfg_init(DIR_U, "TITLE") >= 0);
        CHECK(enhance_cfg_user_path() && strstr(enhance_cfg_user_path(), "TITLE"));
        CHECK(shader_profile_dir(d, sizeof d) && !strcmp(d, DIR_U "/config/shader-profiles"));
    }
    recomp_env_set(RENV_CONFIG, DIR_U "/elsewhere/my.toml");
    enhance_cfg_init(DIR_U, NULL);
    CHECK(shader_profile_dir(d, sizeof d) && !strcmp(d, DIR_U "/elsewhere/shader-profiles"));
    recomp_env_set(RENV_CONFIG, "none");
    enhance_cfg_init(DIR_U, NULL);
    CHECK(!shader_profile_dir(d, sizeof d));
    recomp_env_set(RENV_CONFIG, NULL);
    enhance_cfg_shutdown();
    CHECK(!shader_profile_dir(d, sizeof d));
}

/* ---- the start's choice (D7) -------------------------------------------- */

#define ST DIR_U "/start"

/* Every config file of the start case: the title file, the user file and
 * the defaults beside the executable, each NULL for none. */
static void start_files(const char *title, const char *user, const char *defaults)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/config.default.toml", recomp_exe_dir());
    if (defaults)
        write_file(p, defaults);
    else
        remove(p);
    if (user)
        write_file(ST "/config/config.toml", user);
    else
        remove(ST "/config/config.toml");
    if (title)
        write_file(ST "/TITLE/config.toml", title);
    else
        remove(ST "/TITLE/config.toml");
}

/* Start as xbox_enhance_init would: the layers, then the choice. */
static int start(char *sh, size_t cap)
{
    nv2a_present_chain_params_reset_for_test();
    nv2a_present_chain_request("none");
    CHECK(enhance_cfg_init(ST, "TITLE") >= 0);
    return shader_profile_start(ST, sh, cap);
}

static int requested(const char *slug)
{
    char s[64];
    int on = nv2a_present_chain_requested_profile(s, sizeof s);
    return slug ? on && !strcmp(s, slug) : !on;
}

static int preset_ends(const char *tail)
{
    char p[1024];
    size_t n = strlen(tail), l;
    if (nv2a_present_chain_preset(p, sizeof p, NULL) <= 0)
        return 0;
    l = strlen(p);
    return l >= n && !strcmp(p + l - n, tail);
}

static int profile_error(void)
{
    char e[NV2A_PRESENT_PROFILE_ERR_MAX];
    return nv2a_present_chain_profile_error_get(e, sizeof e) && e[0];
}

static void case_start(void)
{
    char sh[512], e[256];
    float v = 0;

    dirs();
    make_dir(ST);
    make_dir(ST "/config");
    make_dir(ST "/config/shader-profiles");
    make_dir(ST "/TITLE");
    make_dir(ST "/shaders");
    write_file(ST "/shaders/a.slang", "#version 450\n");
    write_file(ST "/shaders/b.slang", "#version 450\n");
    write_file(ST "/shaders/a.slangp", "shaders = 1\nshader0 = a.slang\n");
    write_file(ST "/shaders/b.slangp", "shaders = 1\nshader0 = b.slang\n");
    write_file(ST "/config/shader-profiles/one.toml",
               "version = 1\nname = \"One\"\nstages = [\"user:a\"]\n[params.a]\nY = 0.25\n");
    write_file(ST "/config/shader-profiles/two.toml",
               "version = 1\nname = \"Two\"\nstages = [\"user:a\", \"user:b\"]\n"
               "[params.b]\nX = 0.5\n");
    write_file(ST "/config/shader-profiles/broken.toml", "version = \n");
    write_file(ST "/config/shader-profiles/gap.toml",
               "version = 1\nname = \"Gap\"\nstages = [\"user:a\", \"user:gone\"]\n");
    recomp_env_set(RENV_CONFIG, NULL);
    recomp_env_set(RENV_PRESENT_SHADER, NULL);
    recomp_env_set(RENV_PRESENT_PROFILE, NULL);
    s_cache[0] = 0;

    /* stock: nothing set, and the broken file is never read */
    start_files(NULL, NULL, NULL);
    CHECK(start(sh, sizeof sh) == 0 && requested(NULL) && !nv2a_present_chain_wanted());
    CHECK(!profile_error());
    /* present.shader only: as before profiles */
    start_files(NULL, "present.shader = \"crt-lottes\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "crt-lottes") && requested(NULL));
    /* a profile only: one stage runs its preset, no compose; the request
     * (whose stage descriptions load librashader) waits for the first
     * present as a chain's does, so nothing is in the store yet */
    start_files(NULL, "present.profile = \"one\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested("one") && nv2a_present_chain_wanted());
    CHECK(nv2a_present_chain_preset(e, sizeof e, NULL) == 0);
    CHECK(!nv2a_present_chain_profile_value_get("one", "Y", &v));
    CHECK(!shader_profile_start_loaded(e, sizeof e));
    nv2a_present_chain_gen();
    CHECK(requested("one") && preset_ends("/shaders/a.slangp"));
    CHECK(nv2a_present_chain_profile_value_get("one", "Y", &v) && v == 0.25f);
    /* the menu learns once that the start's values are in the store */
    CHECK(shader_profile_start_loaded(e, sizeof e) && !strcmp(e, "one"));
    CHECK(!shader_profile_start_loaded(e, sizeof e));
    /* both in one layer: the profile */
    start_files(NULL, "present.profile = \"one\"\npresent.shader = \"fsr\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested("one"));
    /* env over config, both ways */
    recomp_env_set(RENV_PRESENT_SHADER, "fsr");
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "fsr") && requested(NULL));
    recomp_env_set(RENV_PRESENT_SHADER, "none");
    CHECK(start(sh, sizeof sh) == 0 && requested(NULL) && !nv2a_present_chain_wanted());
    recomp_env_set(RENV_PRESENT_SHADER, NULL);
    start_files(NULL, "present.shader = \"fsr\"\n", NULL);
    recomp_env_set(RENV_PRESENT_PROFILE, "one");
    CHECK(start(sh, sizeof sh) == 2 && requested("one"));
    /* RECOMP_PRESENT_PROFILE=none leaves the choice to the files */
    recomp_env_set(RENV_PRESENT_PROFILE, "none");
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "fsr"));
    recomp_env_set(RENV_PRESENT_PROFILE, NULL);
    /* missing and broken profiles: stock with the error, never the shader */
    start_files(NULL, "present.profile = \"gone\"\npresent.shader = \"fsr\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested(NULL) && !nv2a_present_chain_wanted());
    CHECK(profile_error());
    start_files(NULL, "present.profile = \"broken\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested(NULL) && profile_error());
    start_files(NULL, "present.profile = \"Not A Slug\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested(NULL) && profile_error());
    /* a value that is not a string counts as unset */
    start_files(NULL, "present.profile = 3\npresent.shader = \"fsr\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "fsr"));
    /* the highest layer decides alone */
    start_files("present.shader = \"fsr\"\n", "present.profile = \"one\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "fsr") && requested(NULL));
    start_files(NULL, NULL, "present.profile = \"one\"\n");
    CHECK(start(sh, sizeof sh) == 2 && requested("one"));
    start_files(NULL, "present.shader = \"fsr\"\n", "present.profile = \"one\"\n");
    CHECK(start(sh, sizeof sh) == 1 && !strcmp(sh, "fsr"));
    /* "none" in a layer stops a lower layer's choice */
    start_files("present.profile = \"none\"\n", "present.shader = \"fsr\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 0 && requested(NULL) && !profile_error());
    start_files(NULL, "present.shader = \"none\"\n", "present.profile = \"one\"\n");
    CHECK(start(sh, sizeof sh) == 0 && requested(NULL));
    /* RECOMP_CONFIG=none: no files and no profiles */
    start_files(NULL, NULL, NULL);
    recomp_env_set(RENV_CONFIG, "none");
    recomp_env_set(RENV_PRESENT_PROFILE, "one");
    CHECK(start(sh, sizeof sh) == 2 && requested(NULL) && profile_error());
    recomp_env_set(RENV_PRESENT_PROFILE, NULL);
    recomp_env_set(RENV_CONFIG, NULL);

    /* A chain waits for the shader cache: requested, wanted, no preset
     * until the first generation read composes it. */
    start_files(NULL, "present.profile = \"two\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested("two") && nv2a_present_chain_wanted());
    CHECK(nv2a_present_chain_preset(e, sizeof e, NULL) == 0);
    snprintf(s_cache, sizeof s_cache, "%s/cache", ST);
    nv2a_present_chain_gen();
    CHECK(requested("two") && preset_ends("/cache/slangp/two.slangp") && !profile_error());
    CHECK(nv2a_present_chain_profile_value_get("two", "X", &v) && v == 0.5f);
    /* run once: a later read does not compose again */
    remove(ST "/cache/slangp/two.slangp");
    nv2a_present_chain_gen();
    CHECK(nv2a_present_chain_preset(e, sizeof e, NULL) < 0);
    /* No cache folder yet at the first presents (the boot presents before
     * the path module is up): the start stays pending, without an error,
     * and composes at the first present that finds the folder. */
    s_cache[0] = 0;
    shader_profile_start_loaded(e, sizeof e);
    CHECK(start(sh, sizeof sh) == 2 && requested("two"));
    {
        unsigned g = nv2a_present_chain_request_gen();
        nv2a_present_chain_gen();
        nv2a_present_chain_gen();
        CHECK(requested("two") && nv2a_present_chain_wanted() && !profile_error());
        CHECK(nv2a_present_chain_preset(e, sizeof e, NULL) == 0);
        CHECK(nv2a_present_chain_request_gen() == g);
        /* the backend finds no preset yet: not a failure while it waits,
         * or it would stop reading the generation that runs the wait */
        nv2a_present_chain_failed(g);
        CHECK(nv2a_present_chain_wanted() && !profile_error());
    }
    snprintf(s_cache, sizeof s_cache, "%s/cache", ST);
    nv2a_present_chain_gen();
    CHECK(requested("two") && preset_ends("/cache/slangp/two.slangp") && !profile_error());
    CHECK(shader_profile_start_loaded(e, sizeof e) && !strcmp(e, "two"));
    /* a real failure once the wait is over still counts: the backend's
     * failed() at the composed chain's generation clears wanted and sets
     * the error */
    s_cache[0] = 0;
    CHECK(start(sh, sizeof sh) == 2);
    nv2a_present_chain_gen();
    nv2a_present_chain_gen();
    CHECK(nv2a_present_chain_wanted() && !profile_error());
    snprintf(s_cache, sizeof s_cache, "%s/cache", ST);
    {
        unsigned g = nv2a_present_chain_gen();
        CHECK(requested("two") && preset_ends("/cache/slangp/two.slangp"));
        nv2a_present_chain_failed(g);
        CHECK(!nv2a_present_chain_wanted() && profile_error());
    }
    shader_profile_start_loaded(e, sizeof e);
    /* a missing stage fails at the first present, not when the folder
     * appears: the stages do not need it */
    s_cache[0] = 0;
    start_files(NULL, "present.profile = \"gap\"\n", NULL);
    CHECK(start(sh, sizeof sh) == 2 && requested("gap") && nv2a_present_chain_wanted());
    nv2a_present_chain_gen();
    CHECK(requested(NULL) && !nv2a_present_chain_wanted() && profile_error());
    start_files(NULL, "present.profile = \"two\"\n", NULL);
    /* a menu request while it waits drops the wait */
    s_cache[0] = 0;
    CHECK(start(sh, sizeof sh) == 2);
    nv2a_present_chain_gen();
    nv2a_present_chain_request("fsr");
    snprintf(s_cache, sizeof s_cache, "%s/cache", ST);
    nv2a_present_chain_gen();
    CHECK(requested(NULL) && !profile_error());
    CHECK(!shader_profile_start_loaded(e, sizeof e));
    /* a rearm only holds at its own generation, and runs once a read */
    {
        unsigned g;
        nv2a_present_chain_request_pending("two", count_prepare);
        g = nv2a_present_chain_request_gen();
        CHECK(!nv2a_present_chain_request_rearm(g - 1, count_prepare));
        CHECK(nv2a_present_chain_request_rearm(g, count_prepare));
        s_prepared = 0;
        nv2a_present_chain_gen();
        nv2a_present_chain_gen();
        CHECK(s_prepared == 1);
        nv2a_present_chain_request("none");
        CHECK(!nv2a_present_chain_request_rearm(g, count_prepare));
    }
    s_cache[0] = 0;
    /* another request first drops the pending compose */
    CHECK(start(sh, sizeof sh) == 2);
    nv2a_present_chain_request("fsr");
    snprintf(s_cache, sizeof s_cache, "%s/cache", ST);
    nv2a_present_chain_gen();
    CHECK(requested(NULL) && !profile_error());
    CHECK(!shader_profile_start_loaded(e, sizeof e));
    s_cache[0] = 0;

    /* The deferred half's request applies only at the generation it was
     * made for: a menu request that lands while it composes wins, its
     * stock fallback included. */
    {
        unsigned g;
        nv2a_present_chain_request("fsr");
        g = nv2a_present_chain_request_gen();
        CHECK(!nv2a_present_chain_request_profile_at(g - 1, "two", ST "/shaders/a.slangp",
                                                     NULL, 0, NULL));
        CHECK(!nv2a_present_chain_request_profile_at(g - 1, "two", NULL, NULL, 0, "late"));
        CHECK(requested(NULL) && nv2a_present_chain_wanted() && !profile_error());
        CHECK(nv2a_present_chain_request_profile_at(g, "two", NULL, NULL, 0, "why"));
        CHECK(requested(NULL) && !nv2a_present_chain_wanted() && profile_error());
        g = nv2a_present_chain_request_gen();
        CHECK(nv2a_present_chain_request_profile_at(g, "two", ST "/shaders/a.slangp", NULL, 0,
                                                    NULL));
        CHECK(requested("two") && preset_ends("/shaders/a.slangp") && !profile_error());
        /* a real profile request is never rearmed: its build failure
         * must not be hidden as a wait */
        g = nv2a_present_chain_request_gen();
        CHECK(!nv2a_present_chain_request_rearm(g, count_prepare));
        nv2a_present_chain_failed(g);
        CHECK(!nv2a_present_chain_wanted() && profile_error());
    }

    start_files(NULL, NULL, NULL);
    enhance_cfg_shutdown();
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";
    const char *shaders = argc > 2 ? argv[2] : "assets/shaders";

    if (!strcmp(c, "names"))
        case_names();
    else if (!strcmp(c, "parse"))
        case_parse();
    else if (!strcmp(c, "save"))
        case_save();
    else if (!strcmp(c, "list"))
        case_list();
    else if (!strcmp(c, "stages"))
        case_stages(shaders);
    else if (!strcmp(c, "rehome"))
        case_rehome();
    else if (!strcmp(c, "compose"))
        case_compose();
    else if (!strcmp(c, "bundled"))
        case_bundled(shaders);
    else if (!strcmp(c, "paths"))
        case_paths();
    else if (!strcmp(c, "user_dir"))
        case_user_dir();
    else if (!strcmp(c, "start"))
        case_start();
    else {
        fprintf(stderr, "unknown case %s\n", c);
        return 2;
    }
    if (!s_fail)
        printf("ok %s\n", c);
    return s_fail;
}
