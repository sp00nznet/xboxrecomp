/*
 * The parameter kinds (src/enhance/shader_param_kinds.c) and the declaring
 * file the chain records for each parameter (nv2a_present_chain.c).
 *
 *   shipped  every #pragma parameter of the shaders in assets/shaders,
 *            through the rules with the shipped sidecar
 *   rules    the rule order on made-up rows
 *   profile  a profile's [param_kinds] over the sidecar
 *   files    the file each row gets from a preset: #reference, #include,
 *            the first pass of two that declare a name
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifndef _WIN32
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>
#else
#include <direct.h>
#define mkdir(p, m) _mkdir(p)
#endif

#include "nv2a_present_chain.h"
#include "shader_param_kinds.h"

static int s_fail;
#define CHECK(c)                                                                 \
    do {                                                                         \
        if (!(c)) {                                                              \
            fprintf(stderr, "%s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #c); \
            s_fail = 1;                                                          \
        }                                                                        \
    } while (0)

static struct nv2a_present_param mk(const char *name, const char *desc, float init, float min,
                                    float max, float step, const char *file)
{
    struct nv2a_present_param p;
    memset(&p, 0, sizeof p);
    snprintf(p.name, sizeof p.name, "%s", name);
    snprintf(p.desc, sizeof p.desc, "%s", desc);
    p.initial = init;
    p.min = min;
    p.max = max;
    p.step = step;
    snprintf(p.file, sizeof p.file, "%s", file);
    return p;
}

static char *slurp(const char *path)
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
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) {
        free(b);
        b = NULL;
    }
    if (b)
        b[n] = 0;
    fclose(f);
    return b;
}

static void spit(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL);
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

/* ---- shipped ------------------------------------------------------------ */

/* Every parameter the bundled shaders declare and how its row shows. A new
 * shader or parameter fails here until it is listed: its kind is a choice
 * someone makes, not whatever the rules happen to give. label: the row's
 * label when it differs from the description; on: a toggle's on word. */
static const struct {
    const char *file, *name;
    int kind, nnames;
    const char *label, *first;
} k_shipped[] = {
    {"crt-lottes-fast.slang", "MASK", SHADER_PARAM_CHOICE, 4, NULL, "None"},
    {"crt-lottes-fast.slang", "MASK_INTENSITY", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "SCANLINE_THINNESS", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "SCAN_BLUR", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "CURVATURE", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "TRINITRON_CURVE", SHADER_PARAM_TOGGLE, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "CORNER", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes-fast.slang", "CRT_GAMMA", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "hardScan", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "hardPix", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "warpX", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "warpY", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "maskDark", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "maskLight", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "scaleInLinearGamma", SHADER_PARAM_TOGGLE, 0, NULL, NULL},
    {"crt-lottes.slang", "shadowMask", SHADER_PARAM_CHOICE, 5, NULL, "None"},
    {"crt-lottes.slang", "brightBoost", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "hardBloomPix", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "hardBloomScan", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "bloomAmount", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-lottes.slang", "shape", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"fsr-pass1.slang", "FSR_SHARPENING", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"fsr-pass1.slang", "FSR_FILMGRAIN", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"fsr-pass1.slang", "FSR_GRAINCOLOR", SHADER_PARAM_TOGGLE, 2,
     "FSR LFGA Film Grain Color", "Gray"},
    {"fsr-pass1.slang", "FSR_GRAINPDF", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "MASK_INTENSITY", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "InputGamma", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "OutputGamma", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "BRIGHTBOOST", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "SCANLINES", SHADER_PARAM_SLIDER, 0, NULL, NULL},
    {"crt-hyllian-fast.slang", "SHARPER", SHADER_PARAM_TOGGLE, 0, NULL, NULL},
};
#define N_SHIPPED (int)(sizeof k_shipped / sizeof k_shipped[0])

/* The pass sources of the shipped presets (assets/shaders/<name>.slangp
 * and their #reference). */
static const char *const k_passes[] = {
    "crt/shaders/crt-lottes-fast.slang",
    "crt/shaders/crt-lottes.slang",
    "crt/shaders/hyllian/crt-hyllian-fast.slang",
    "edge-smoothing/fsr/shaders/fsr-pass0.slang",
    "edge-smoothing/fsr/shaders/fsr-pass1.slang",
};
static const char *const k_presets[] = {"crt-hyllian.slangp", "crt-lottes-fast.slangp",
                                        "crt-lottes.slangp", "fsr.slangp"};

/* #pragma parameter NAME "desc" initial min max step, as librashader reads
 * it, from one pass source. */
static int read_pragmas(const char *path, const char *file, struct nv2a_present_param *out,
                        int cap)
{
    char *text = slurp(path), *l, *e;
    int n = 0;
    CHECK(text != NULL);
    if (!text)
        return 0;
    for (l = text; *l; l = *e ? e + 1 : e) {
        char name[64], desc[96];
        float v[4];
        e = strchr(l, '\n');
        if (!e)
            e = l + strlen(l);
        if (strncmp(l, "#pragma parameter ", 18))
            continue;
        if (sscanf(l + 18, "%63s \"%95[^\"]\" %f %f %f %f", name, desc, &v[0], &v[1], &v[2],
                   &v[3]) == 6
            && n < cap)
            out[n++] = mk(name, desc, v[0], v[1], v[2], v[3], file);
    }
    free(text);
    return n;
}

static void case_shipped(void)
{
    static struct nv2a_present_param t[128];
    struct shader_param_kind_info info;
    const struct shader_param_kinds *side;
    int n = 0, seen[N_SHIPPED] = {0};
    char path[1024];

    snprintf(path, sizeof path, "%s/param-kinds.toml", XR_SHADERS);
    shader_param_kinds_builtin_reset(path);
    side = shader_param_kinds_builtin();
    CHECK(side->n == 2);
    for (size_t i = 0; i < sizeof k_passes / sizeof k_passes[0]; i++) {
        const char *b = strrchr(k_passes[i], '/') + 1;
        snprintf(path, sizeof path, "%s/%s", XR_SHADERS, k_passes[i]);
        n += read_pragmas(path, b, t + n, 128 - n);
    }
    CHECK(n == N_SHIPPED);
    for (int i = 0; i < n; i++) {
        int j;
        for (j = 0; j < N_SHIPPED; j++)
            if (!strcmp(k_shipped[j].file, t[i].file) && !strcmp(k_shipped[j].name, t[i].name))
                break;
        if (j == N_SHIPPED) {
            fprintf(stderr, "%s %s: not in k_shipped\n", t[i].file, t[i].name);
            s_fail = 1;
            continue;
        }
        seen[j] = 1;
        shader_param_kind_resolve(&t[i], NULL, side, &info);
        if (info.kind != k_shipped[j].kind || info.nnames != k_shipped[j].nnames) {
            fprintf(stderr, "%s %s: kind %d names %d\n", t[i].file, t[i].name, info.kind,
                    info.nnames);
            s_fail = 1;
        }
        CHECK(!strcmp(info.label, k_shipped[j].label ? k_shipped[j].label : t[i].desc));
        if (k_shipped[j].first)
            CHECK(!strcmp(info.name[0], k_shipped[j].first));
        /* The sidecar's help reaches the declared rows only. */
        CHECK(!!info.help[0] == (info.kind == SHADER_PARAM_CHOICE));
    }
    for (int j = 0; j < N_SHIPPED; j++)
        CHECK(seen[j]);

    /* The same name in another file is not the sidecar's MASK. */
    {
        struct nv2a_present_param p = mk("MASK", "Mask Type", 1, 0, 3, 1, "other.slang");
        shader_param_kind_resolve(&p, NULL, side, &info);
        CHECK(info.kind == SHADER_PARAM_SLIDER && !info.help[0]);
        p.file[0] = 0;
        shader_param_kind_resolve(&p, NULL, side, &info);
        CHECK(info.kind == SHADER_PARAM_SLIDER);
    }
    {
        struct nv2a_present_param p = mk("shadowMask", "shadowMask", 3, 0, 4, 1,
                                         "crt-lottes.slang");
        shader_param_kind_resolve(&p, NULL, side, &info);
        CHECK(!strcmp(info.name[4], "VGA"));
        CHECK(!strcmp(info.help, "The phosphor pattern drawn over the picture"));
        CHECK(shader_param_kind_index(&p, 3.0f) == 3);
        CHECK(shader_param_kind_value(&p, 2) == 2.0f);
    }

#ifndef _WIN32
    /* The presets listed above are all there is: a new one needs its
     * passes in k_passes. */
    {
        DIR *d = opendir(XR_SHADERS);
        struct dirent *de;
        int np = 0;
        CHECK(d != NULL);
        while (d && (de = readdir(d))) {
            size_t l = strlen(de->d_name);
            int known = 0;
            if (l < 7 || strcmp(de->d_name + l - 7, ".slangp"))
                continue;
            for (size_t i = 0; i < sizeof k_presets / sizeof k_presets[0]; i++)
                known |= !strcmp(de->d_name, k_presets[i]);
            if (!known)
                fprintf(stderr, "%s: a preset k_presets lacks\n", de->d_name);
            CHECK(known);
            np++;
        }
        if (d)
            closedir(d);
        CHECK(np == (int)(sizeof k_presets / sizeof k_presets[0]));
    }
#endif
    shader_param_kinds_builtin_reset(NULL);
}

/* ---- rules ---------------------------------------------------------------- */

static void case_rules(void)
{
    static struct shader_param_kinds side;
    struct shader_param_kind_info info;
    struct nv2a_present_param p;
    char err[256];

    /* 3. An inferred choice: one name per value. */
    p = mk("MODE", "Blend Mode: Off | Soft | Hard", 0, 0, 2, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_CHOICE && info.nnames == 3);
    CHECK(!strcmp(info.label, "Blend Mode") && !strcmp(info.name[2], "Hard"));
    CHECK(shader_param_kind_value(&p, 1) == 1.0f);
    CHECK(shader_param_kind_index(&p, 2.0f) == 2 && shader_param_kind_index(&p, 9.0f) == 2);
    /* A count that differs from the names: a slider, its label whole. */
    p = mk("MODE", "Blend Mode: Off | Soft", 0, 0, 2, 1, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER && !strcmp(info.label, "Blend Mode: Off | Soft"));
    /* A colon without a list stays. */
    p = mk("T", "Ratio: wide", 0, 0, 1, 1, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_TOGGLE && !strcmp(info.label, "Ratio: wide"));
    /* Not 0..1 by 1: a slider. */
    p = mk("F", "f", 0, 0, 1, 0.5f, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER);
    /* More than 8 values: a slider even with names. */
    p = mk("N", "n: a|b|c|d|e|f|g|h|i", 0, 0, 8, 1, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER);
    /* An offset range: the value is min + index. */
    p = mk("O", "o: x | y | z", 2, 1, 3, 1, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_CHOICE);
    CHECK(shader_param_kind_value(&p, 0) == 1.0f && shader_param_kind_index(&p, 3.0f) == 2);
    /* Left and Right wrap from both ends; a value off the grid steps from
     * its nearest index (1.5 rounds to 2, Off | Soft | Hard). */
    p = mk("MODE", "Blend Mode: Off | Soft | Hard", 0, 0, 2, 1, "");
    CHECK(shader_param_choice_next(&p, 3, 0.0f, -1) == 2.0f);
    CHECK(shader_param_choice_next(&p, 3, 2.0f, 1) == 0.0f);
    CHECK(shader_param_choice_next(&p, 3, 1.0f, 1) == 2.0f);
    CHECK(shader_param_kind_index(&p, 1.5f) == 2);
    CHECK(shader_param_choice_next(&p, 3, 1.5f, 1) == 0.0f);
    CHECK(shader_param_choice_next(&p, 3, 1.5f, -1) == 1.0f);
    /* A huge integer range is a slider, its count never overflowing. */
    p = mk("BIG", "big: a | b | c", 0, 0, 1e9f, 1, "");
    shader_param_kind_resolve(&p, NULL, NULL, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER);

    /* 1. Declared over the inferred, and a declaration that does not fit
     * falls through to the next rule. */
    CHECK(shader_param_kinds_parse("[[param]]\nfile = \"x.slang\"\nname = \"TOG\"\n"
                                   "kind = \"slider\"\nhelp = \"h\"\n"
                                   "[[param]]\nfile = \"x.slang\"\nname = \"BAD\"\n"
                                   "kind = \"choice\"\nchoices = [\"a\", \"b\", \"c\"]\n"
                                   "[[param]]\nfile = \"x.slang\"\nname = \"WORDS\"\n"
                                   "kind = \"toggle\"\nchoices = [\"Dim\", \"Bright\"]\n",
                                   "side", &side, err, sizeof err) == 0);
    CHECK(side.n == 3);
    p = mk("TOG", "tog", 0, 0, 1, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER && !strcmp(info.help, "h"));
    p = mk("BAD", "bad", 0, 0, 3, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER);
    p = mk("BAD", "bad", 0, 0, 1, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_TOGGLE);
    p = mk("WORDS", "words", 0, 0, 1, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_TOGGLE && info.nnames == 2);
    CHECK(!strcmp(info.name[1], "Bright"));
    CHECK(shader_param_toggle_value(&p, 1) == 1.0f && shader_param_toggle_value(&p, 0) == 0.0f);

    /* A declared toggle on 0..2 by 1: on stores max and reads from the
     * middle up, never a choice's index (min + 1). */
    CHECK(shader_param_kinds_parse("[[param]]\nfile = \"x.slang\"\nname = \"T3\"\n"
                                   "kind = \"toggle\"\n"
                                   "[[param]]\nfile = \"x.slang\"\nname = \"C4\"\n"
                                   "kind = \"choice\"\nchoices = [\"a\", \"b\", \"c\", \"d\"]\n",
                                   "side", &side, err, sizeof err) == 0);
    p = mk("T3", "t3", 0, 0, 2, 1, "x.slang");
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_TOGGLE);
    CHECK(shader_param_toggle_value(&p, 1) == 2.0f && shader_param_toggle_value(&p, 0) == 0.0f);
    CHECK(shader_param_toggle_on(&p, 2.0f) && shader_param_toggle_on(&p, 1.0f));
    CHECK(!shader_param_toggle_on(&p, 0.0f) && !shader_param_toggle_on(&p, 0.9f));
    CHECK(shader_param_toggle_value(&p, !shader_param_toggle_on(&p, 0.0f)) == 2.0f);

    /* A profile declaration that cannot fit the range (two names for four
     * values) leaves the sidecar's in force, before any inferred rule. */
    {
        static struct shader_param_kinds prof;
        CHECK(shader_param_kinds_parse_profile("[param_kinds.C4]\nkind = \"choice\"\n"
                                               "choices = [\"x\", \"y\"]\n",
                                               "p.toml", &prof, err, sizeof err) == 0);
        p = mk("C4", "c4: one | two | three | four", 0, 0, 3, 1, "x.slang");
        shader_param_kind_resolve(&p, &prof, &side, &info);
        CHECK(info.kind == SHADER_PARAM_CHOICE && info.nnames == 4);
        CHECK(!strcmp(info.name[0], "a") && !strcmp(info.name[3], "d"));
        /* Without the sidecar: the inferred choice from the label. */
        shader_param_kind_resolve(&p, &prof, NULL, &info);
        CHECK(info.kind == SHADER_PARAM_CHOICE && !strcmp(info.name[0], "one"));
    }

    /* A help over the cap is cut at a character, not inside one. */
    {
        char text[512], help[301];
        for (int k = 0; k < 150; k++)
            memcpy(help + 2 * k, "\xC3\xA9", 2);   /* é */
        help[300] = 0;
        snprintf(text, sizeof text,
                 "[[param]]\nfile = \"x.slang\"\nname = \"H\"\nhelp = \"x%s\"\n", help);
        CHECK(shader_param_kinds_parse(text, "side", &side, err, sizeof err) == 0);
        CHECK(side.n == 1 && strlen(side.d[0].help) < SHADER_PARAM_HELP_LEN);
        CHECK(strlen(side.d[0].help) == 99 && side.d[0].help[0] == 'x');
    }

    /* Broken entries are skipped, the rest kept. */
    CHECK(shader_param_kinds_parse("[[param]]\nname = \"NOFILE\"\nkind = \"toggle\"\n"
                                   "[[param]]\nfile = \"a/b.slang\"\nname = \"PATH\"\n"
                                   "[[param]]\nfile = \"x.slang\"\nname = \"OK\"\n"
                                   "kind = \"nonsense\"\nhelp = \"kept\"\n",
                                   "side", &side, err, sizeof err) == 0);
    CHECK(side.n == 1 && !strcmp(side.d[0].name, "OK") && side.d[0].kind < 0);
    CHECK(shader_param_kinds_parse("[[param\n", "side", &side, err, sizeof err) < 0);
}

/* ---- profile ------------------------------------------------------------- */

static void case_profile(void)
{
    static struct shader_param_kinds prof, side;
    struct shader_param_kind_info info;
    struct nv2a_present_param p;
    char err[256], path[1024];

    snprintf(path, sizeof path, "%s/param-kinds.toml", XR_SHADERS);
    shader_param_kinds_builtin_reset(path);
    CHECK(shader_param_kinds_profile_key("param_kinds.MASK.kind"));
    CHECK(!shader_param_kinds_profile_key("params.crt.MASK"));
    CHECK(shader_param_kinds_parse_profile("version = 1\nname = \"Mine\"\n"
                                           "[param_kinds.MASK]\nkind = \"slider\"\n"
                                           "[param_kinds.warpX]\nhelp = \"Bend\"\n"
                                           "[param_kinds.FSR_GRAINCOLOR]\nkind = \"choice\"\n"
                                           "choices = [\"Mono\", \"Colour\"]\n"
                                           "help = \"Grain tint\"\n",
                                           "p.toml", &prof, err, sizeof err) == 0);
    CHECK(prof.n == 3);
    memcpy(&side, shader_param_kinds_builtin(), sizeof side);

    /* The profile's slider beats the sidecar's choice; the sidecar's help
     * still shows (the profile gave none). */
    p = mk("MASK", "Mask Type", 1, 0, 3, 1, "crt-lottes-fast.slang");
    shader_param_kind_resolve(&p, &prof, &side, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER);
    CHECK(!strcmp(info.help, "The phosphor pattern drawn over the picture"));
    /* Help alone, the kind inferred. */
    p = mk("warpX", "warpX", 0.031f, 0, 0.125f, 0.01f, "crt-lottes.slang");
    shader_param_kind_resolve(&p, &prof, &side, &info);
    CHECK(info.kind == SHADER_PARAM_SLIDER && !strcmp(info.help, "Bend"));
    /* A two-value choice with its own names; the label still cut. */
    p = mk("FSR_GRAINCOLOR", "FSR LFGA Film Grain Color: Gray | RGB", 1, 0, 1, 1,
           "fsr-pass1.slang");
    shader_param_kind_resolve(&p, &prof, &side, &info);
    CHECK(info.kind == SHADER_PARAM_CHOICE && info.nnames == 2);
    CHECK(!strcmp(info.name[1], "Colour") && !strcmp(info.help, "Grain tint"));
    CHECK(!strcmp(info.label, "FSR LFGA Film Grain Color"));
    /* Without the profile: the inferred toggle. */
    shader_param_kind_resolve(&p, NULL, &side, &info);
    CHECK(info.kind == SHADER_PARAM_TOGGLE && !strcmp(info.name[1], "RGB"));
    shader_param_kinds_builtin_reset(NULL);
}

/* ---- files ----------------------------------------------------------------- */

static const struct nv2a_present_param *row(const struct nv2a_present_param *t, int n,
                                            const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(t[i].name, name))
            return &t[i];
    return NULL;
}

static void case_files(void)
{
    static struct nv2a_present_param t[NV2A_PRESENT_PARAMS_MAX];
    const struct nv2a_present_param *r;
    char path[1024], dir[1024];
    int n;

    /* The stand-in lists warpX and maskDark for any preset. */
    nv2a_present_chain_set_lib(FAKE_LRS_PARAMS);
    CHECK(nv2a_present_chain_lib() != NULL);

    /* A shipped preset, through its #reference into crt/. */
    snprintf(path, sizeof path, "%s/crt-lottes.slangp", XR_SHADERS);
    n = nv2a_present_chain_describe(path, t, NV2A_PRESENT_PARAMS_MAX);
    CHECK(n == 2);
    CHECK((r = row(t, n, "warpX")) && !strcmp(r->file, "crt-lottes.slang"));
    CHECK((r = row(t, n, "maskDark")) && !strcmp(r->file, "crt-lottes.slang"));
    /* A preset whose passes declare neither: no file. */
    snprintf(path, sizeof path, "%s/fsr.slangp", XR_SHADERS);
    n = nv2a_present_chain_describe(path, t, NV2A_PRESENT_PARAMS_MAX);
    CHECK(n == 2 && !t[0].file[0] && !t[1].file[0]);

    /* Made up: pass 0 declares maskDark in an #include, pass 1 declares
     * both; maskDark is pass 0's, warpX pass 1's. The top preset reaches
     * them through a #reference one directory down, which overrides
     * shader1 with a path relative to itself. */
    snprintf(dir, sizeof dir, "spk_files");
    mkdir(dir, 0755);
    mkdir("spk_files/sub", 0755);
    mkdir("spk_files/sub/inc", 0755);
    spit("spk_files/top.slangp", "#reference \"sub/inner.slangp\"\nwarpX = 0.02\n");
    spit("spk_files/sub/inner.slangp",
         "shaders = 2\nshader0 = \"p0.slang\" # first\nshader1 = wrong.slang\n"
         "shader1 = p1.slang\n");
    spit("spk_files/sub/p0.slang", "#version 450\n#include \"inc/common.h\"\n");
    spit("spk_files/sub/inc/common.h",
         "  #pragma parameter maskDark \"maskDark\" 0.5 0.0 2.0 0.1\n");
    spit("spk_files/sub/p1.slang", "#pragma parameter maskDark \"maskDark\" 0.5 0.0 2.0 0.1\n"
                                   "#pragma parameter warpX \"warpX\" 0.03 0.0 0.1 0.01\n"
                                   "#pragma parameter warpXY \"warpXY\" 0 0 1 1\n");
    n = nv2a_present_chain_describe("spk_files/top.slangp", t, NV2A_PRESENT_PARAMS_MAX);
    CHECK(n == 2);
    CHECK((r = row(t, n, "maskDark")) && !strcmp(r->file, "p0.slang"));
    CHECK((r = row(t, n, "warpX")) && !strcmp(r->file, "p1.slang"));

    /* Guarded headers that include each other (and a backslash path, as a
     * Windows preset writes them) are each read once: the call ends at
     * once instead of reading the pair again at every level. */
    spit("spk_files/sub/p0.slang", "#version 450\n#include \"inc\\a.inc\"\n");
    spit("spk_files/sub/inc/a.inc",
         "#ifndef A\n#define A\n#include \"b.inc\"\n#include \"a.inc\"\n#include \"b.inc\"\n"
         "#include \"a.inc\"\n#endif\n"
         "#pragma parameter maskDark \"maskDark\" 0.5 0.0 2.0 0.1\n");
    spit("spk_files/sub/inc/b.inc",
         "#ifndef B\n#define B\n#include \"a.inc\"\n#include \"b.inc\"\n#include \"a.inc\"\n"
         "#include \"b.inc\"\n#endif\n");
    {
        clock_t c0 = clock();
        for (int k = 0; k < 20; k++)
            n = nv2a_present_chain_describe("spk_files/top.slangp", t, NV2A_PRESENT_PARAMS_MAX);
        CHECK((double)(clock() - c0) / CLOCKS_PER_SEC < 2.0);
    }
    CHECK(n == 2);
    CHECK((r = row(t, n, "maskDark")) && !strcmp(r->file, "p0.slang"));
    CHECK((r = row(t, n, "warpX")) && !strcmp(r->file, "p1.slang"));
    remove("spk_files/sub/inc/a.inc");
    remove("spk_files/sub/inc/b.inc");

    remove("spk_files/top.slangp");
    remove("spk_files/sub/inner.slangp");
    remove("spk_files/sub/p0.slang");
    remove("spk_files/sub/p1.slang");
    remove("spk_files/sub/inc/common.h");
#ifndef _WIN32
    rmdir("spk_files/sub/inc");
    rmdir("spk_files/sub");
    rmdir("spk_files");
#endif
}

int main(int argc, char **argv)
{
    const char *c = argc > 1 ? argv[1] : "";

    if (!strcmp(c, "shipped"))
        case_shipped();
    else if (!strcmp(c, "rules"))
        case_rules();
    else if (!strcmp(c, "profile"))
        case_profile();
    else if (!strcmp(c, "files"))
        case_files();
    else {
        fprintf(stderr, "unknown case \"%s\"\n", c);
        return 2;
    }
    if (!s_fail)
        printf("shader_param_kinds %s: ok\n", c);
    return s_fail;
}
