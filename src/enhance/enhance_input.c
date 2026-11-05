/*
 * enhance_input.c - the [input] keys: the port-0 keyboard and mouse.
 *
 * [input] holds the switches and knobs, [input.bindings] maps each of the
 * Duke's controls to key and mouse names (src/input/input_names.c). The
 * layering is enhance_cfg's, with one difference for the bindings: a
 * control is taken whole from the highest file that names it, and a key the
 * higher file names elsewhere is taken out of the controls it inherited,
 * so "Space is B" in the player's file means Space is B and nothing else.
 * Controls no file names keep the toolkit's compiled-in table, less the
 * keys a file binds elsewhere.
 *
 * [input.pad] remaps the host pad onto the Duke the same way: each control
 * takes up to two host sources (upstream's InputPadMap names, design D3 of
 * gamepad-input-ux), the highest file that names a control decides it, and
 * a source a higher file binds is taken off the controls below. With no
 * [input.pad] the pad is the identity, the fixed table the runtime always
 * had.
 *
 * Errors are [INPUT] lines, never fatal, and leave the rest applied: a
 * player's typo in one binding must not cost them the keyboard. The
 * [ENHANCE] lines gain nothing (the golden harness parses those). Docs:
 * docs/runtime/config.md, "Input".
 */
#include "enhance_input.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "host_pad.h"
#include "input_names.h"
#include "keyboard.h"
#include "mouse.h"
#include "pad_map.h"
#include "recomp_env.h"

#define PREFIX     "input.bindings."
#define NFILES     3            /* enhance_cfg_file: 0 title, 1 user, 2 defaults */
#define SLOT_DEF   2

/* The layer's code defaults. The stick knobs default to the legacy
 * keyboard's (no ramp, no walk key, full deflection), so a title that builds
 * the layer but ships no [input] keys keeps the stock keyboard; a game's
 * config.default.toml sets the ramp and walk it wants (design D3). */
#define DEF_RAMP_MS     0
#define DEF_WALK_KEY    NULL
#define DEF_WALK_SCALE  1.0
#define DEF_SENS        1.0
#define DEF_MIN_DEFL    0.45
#define DEF_PULSE_MS    100

static const char *const s_look[]    = { "right_stick", "left_stick", "none", NULL };
static const char *const s_capture[] = { "focus", "click", "never", NULL };
/* input.glyphs: "auto", then the families in xbox_InputPadFamily order. */
static const char *const s_glyphs[] = { "auto", "xbox", "playstation", "nintendo", "steam",
                                        "generic", "gamecube", NULL };

static void say(const recomp_cfg *c, const char *key, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "[INPUT] config error: %s:%d: ", recomp_cfg_name(c),
            recomp_cfg_line(c, key));
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static const char *type_name(recomp_cfg_type t)
{
    switch (t) {
    case RCFG_STRING: return "string";
    case RCFG_INT:    return "integer";
    case RCFG_FLOAT:  return "float";
    case RCFG_BOOL:   return "boolean";
    case RCFG_ARRAY:  return "array";
    default:          return "nothing";
    }
}

/* F9, F10 and F11 have host uses (design D11): bindable, with a warning. */
static const char *reserved_use(int code, int is_mouse)
{
    if (is_mouse)
        return NULL;
    switch (code) {
    case 0x78: return "frees the cursor (input.mouse_release_key)";
    case 0x79: return "menu.open's default ([input.actions])";
    case 0x7A: return "reserved for the fullscreen hotkey";
    }
    return NULL;
}

/* ---- the knobs ----------------------------------------------------------
 * Read here rather than through enhance_cfg_float and friends so a wrong
 * type is an [INPUT] line like every other mistake in the table. Only
 * input.keyboard and input.mouse have an environment variable. */

static const recomp_cfg *knob(const char *key, recomp_cfg_type want, int number)
{
    const recomp_cfg *c = enhance_cfg_lookup(key);
    recomp_cfg_type t;
    if (!c)
        return NULL;
    t = recomp_cfg_type_of(c, key);
    if (t == want || (number && t == RCFG_INT))
        return c;
    say(c, key, "%s should be %s (got %s); using the default", key,
        want == RCFG_BOOL ? "true or false" : want == RCFG_STRING ? "a string"
        : want == RCFG_INT ? "an integer" : "a number", type_name(t));
    return NULL;
}

static int knob_bool(const char *key, int def)
{
    const recomp_cfg *c;
    if (enhance_cfg_source_of(key) == ENHANCE_SRC_ENV)
        return enhance_cfg_bool(key, def);
    c = knob(key, RCFG_BOOL, 0);
    return c ? recomp_cfg_bool(c, key, def) : def;
}

static double knob_float(const char *key, double lo, double hi, double def,
                         int clamp)
{
    const recomp_cfg *c = knob(key, RCFG_FLOAT, 1);
    double v;
    if (!c)
        return def;
    v = recomp_cfg_float(c, key, def);
    if (v >= lo && v <= hi)
        return v;
    if (clamp) {
        double to = v < lo ? lo : hi;
        say(c, key, "%s %g out of range %g .. %g; using %g", key, v, lo, hi, to);
        return to;
    }
    say(c, key, "%s %g out of range %g .. %g; using %g", key, v, lo, hi, def);
    return def;
}

static int knob_int(const char *key, int lo, int hi, int def)
{
    const recomp_cfg *c = knob(key, RCFG_INT, 0);
    long long v;
    if (!c)
        return def;
    v = recomp_cfg_int(c, key, def);
    if (v >= lo && v <= hi)
        return (int)v;
    say(c, key, "%s %lld out of range %d .. %d; using %d", key, v, lo, hi, def);
    return def;
}

static int knob_choice(const char *key, const char *const *choices, int def)
{
    const recomp_cfg *c = knob(key, RCFG_STRING, 0);
    const char *v;
    char list[128];
    size_t n = 0;
    if (!c)
        return def;
    v = recomp_cfg_string(c, key, "");
    for (int i = 0; choices[i]; i++)
        if (!strcmp(v, choices[i]))
            return i;
    list[0] = 0;
    for (int i = 0; choices[i] && n < sizeof list; i++)
        n += (size_t)snprintf(list + n, sizeof list - n, "%s%s", i ? ", " : "", choices[i]);
    say(c, key, "%s '%s' is not one of %s; using %s", key, v, list, choices[def]);
    return def;
}

/* "" or "none", in any case. */
static int no_key(const char *v)
{
    static const char none[] = "none";
    int i;

    if (!v || !*v)
        return 1;
    for (i = 0; v[i] && none[i]; i++)
        if (tolower((unsigned char)v[i]) != none[i])
            return 0;
    return !v[i] && !none[i];
}

/* A key name (not a mouse input); `def` is the default's name. */
/* A NULL def is "no key" (code 0), and so are "" and "none" in a file, so
 * a player can turn the walk key or the release key off. */
static int knob_key(const char *key, const char *def)
{
    const recomp_cfg *c = knob(key, RCFG_STRING, 0);
    const char *v, *use, *defname = def ? def : "none";
    int code = 0, def_code = 0, is_mouse;

    xbox_InputNameToCode(def, &def_code, &is_mouse);
    if (!c)
        return def_code;
    v = recomp_cfg_string(c, key, def);
    if (no_key(v))
        return 0;
    if (!xbox_InputNameToCode(v, &code, &is_mouse)) {
        say(c, key, "unknown input '%s' in %s (see docs/input.md); using %s", v, key, defname);
        return def_code;
    }
    if (is_mouse) {
        say(c, key, "%s '%s' is not a key; using %s", key, v, defname);
        return def_code;
    }
    /* F9 as the release key is the default, not a clash. */
    use = reserved_use(code, 0);
    if (use && !(code == 0x78 && !strcmp(key, "input.mouse_release_key")))
        say(c, key, "%s '%s' is %s; bound anyway", key, v, use);
    return code;
}

/* ---- the bindings ------------------------------------------------------- */

typedef struct {
    unsigned char named;        /* this file names the control */
    unsigned char n;
    unsigned char code[XBOX_BIND_MAX];
    unsigned char mouse[XBOX_BIND_MAX];
    int           line[XBOX_BIND_MAX];
} file_set;

static file_set s_set[NFILES][INPUT_CONTROL_COUNT];

static void control_list(char *out, size_t size)
{
    size_t n = 0;
    out[0] = 0;
    for (int i = 0; i < INPUT_CONTROL_COUNT && n < size; i++)
        n += (size_t)snprintf(out + n, size - n, "%s%s", i ? " " : "",
                              xbox_InputMap_ControlName(i));
}

/* Which control of file f already has (code, mouse), or -1. */
static int owner_in_file(int f, int code, int mouse, int *line)
{
    for (int c = 0; c < INPUT_CONTROL_COUNT; c++)
        for (int k = 0; k < s_set[f][c].n; k++)
            if (s_set[f][c].code[k] == code && s_set[f][c].mouse[k] == mouse) {
                *line = s_set[f][c].line[k];
                return c;
            }
    return -1;
}

static void add_input(int f, const recomp_cfg *cfg, const char *key, int ctl,
                      const char *name)
{
    file_set *s = &s_set[f][ctl];
    int code, mouse, line, other;
    const char *use;

    if (!xbox_InputNameToCode(name, &code, &mouse)) {
        say(cfg, key, "unknown input '%s' in %s (see docs/input.md); skipped", name, key);
        return;
    }
    other = owner_in_file(f, code, mouse, &line);
    if (other == ctl)
        return;                 /* ["A", "a"]: one input */
    if (other >= 0) {
        say(cfg, key, "'%s' already bound to " PREFIX "%s on line %d; ignored here",
            name, xbox_InputMap_ControlName(other), line);
        return;
    }
    if (s->n == XBOX_BIND_MAX) {
        say(cfg, key, "%s has more than %d inputs; '%s' ignored", key, XBOX_BIND_MAX, name);
        return;
    }
    use = reserved_use(code, mouse);
    if (use)
        say(cfg, key, "%s '%s' is %s; bound anyway", key, name, use);
    s->code[s->n] = (unsigned char)code;
    s->mouse[s->n] = (unsigned char)mouse;
    s->line[s->n] = recomp_cfg_line(cfg, key);
    s->n++;
}

static void scan_file(int f)
{
    const recomp_cfg *cfg = enhance_cfg_file(f);
    char controls[512];
    size_t i;

    for (i = 0; i < recomp_cfg_count(cfg); i++) {
        const char *key = recomp_cfg_key_at(cfg, i);
        recomp_cfg_type t;
        int ctl;

        if (strncmp(key, PREFIX, sizeof PREFIX - 1) != 0)
            continue;
        enhance_cfg_lookup(key);        /* read, for report_unused */
        ctl = xbox_InputMap_ControlFromName(key + sizeof PREFIX - 1);
        if (ctl < 0) {
            control_list(controls, sizeof controls);
            say(cfg, key, "unknown control '%s' under [input.bindings] (controls: %s); ignored",
                key + sizeof PREFIX - 1, controls);
            continue;
        }
        t = recomp_cfg_type_of(cfg, key);
        if (t == RCFG_STRING) {
            s_set[f][ctl].named = 1;
            add_input(f, cfg, key, ctl, recomp_cfg_string(cfg, key, ""));
        } else if (t == RCFG_ARRAY) {
            size_t n = recomp_cfg_array_len(cfg, key), k;
            for (k = 0; k < n; k++)
                if (recomp_cfg_array_type(cfg, key, k) != RCFG_STRING) {
                    say(cfg, key, "%s is a string or a list of strings (got a list"
                        " holding %s); ignored", key,
                        type_name(recomp_cfg_array_type(cfg, key, k)));
                    break;
                }
            if (k < n)
                continue;
            s_set[f][ctl].named = 1;    /* [] unbinds */
            for (k = 0; k < n; k++)
                add_input(f, cfg, key, ctl, recomp_cfg_array_string(cfg, key, k, ""));
        } else {
            say(cfg, key, "%s is a string or a list of strings (got %s); ignored",
                key, type_name(t));
        }
    }
}

/* Whether a file above f (a lower index) names (code, mouse) anywhere. */
static int taken_above(int f, int code, int mouse)
{
    int line;
    for (int g = 0; g < f; g++)
        if (owner_in_file(g, code, mouse, &line) >= 0)
            return 1;
    return 0;
}

/* Resolve and apply; counts[f] is how many controls file f decided. */
static void apply_bindings(int counts[NFILES])
{
    /* The poll keeps the committed table until enhance_input_init commits
     * this one (a live reload). */
    xbox_InputBindingsStage();
    for (int ctl = 0; ctl < INPUT_CONTROL_COUNT; ctl++) {
        int f;
        for (f = 0; f < NFILES && !s_set[f][ctl].named; f++)
            ;
        if (f == NFILES) {
            /* The compiled-in default still loses the keys a file binds
             * elsewhere: "Space is B" means Space no longer jumps either. */
            int codes[XBOX_BIND_MAX], mice[XBOX_BIND_MAX];
            int n = xbox_InputBindingsOf(ctl, codes, mice, XBOX_BIND_MAX), k;
            for (k = 0; k < n && !taken_above(NFILES, codes[k], mice[k]); k++)
                ;
            if (k == n)
                continue;
            xbox_InputBindingsClear(ctl);
            for (k = 0; k < n; k++)
                if (!taken_above(NFILES, codes[k], mice[k]))
                    xbox_InputBind(ctl, codes[k], mice[k]);
            continue;
        }
        counts[f]++;
        xbox_InputBindingsClear(ctl);
        for (int k = 0; k < s_set[f][ctl].n; k++) {
            int code = s_set[f][ctl].code[k], mouse = s_set[f][ctl].mouse[k];
            if (!taken_above(f, code, mouse))
                xbox_InputBind(ctl, code, mouse);
        }
    }
}

/* ---- the pad map ([input.pad]) -------------------------------------------
 * The same layering as the bindings, on host sources instead of keys. The
 * two stick keys pick a whole physical stick and take part in "the highest
 * file wins" only. */

#define PAD_PREFIX  "input.pad."
#define PAD_CELLS   2           /* the main map and the alternate map */
#define PAD_LSTICK  INPUT_PAD_CONTROL_COUNT
#define PAD_RSTICK  (INPUT_PAD_CONTROL_COUNT + 1)
#define PAD_KEYS    (INPUT_PAD_CONTROL_COUNT + 2)

typedef struct {
    unsigned char named;
    unsigned char n;
    unsigned char src[PAD_CELLS];   /* a stick key: src[0] is INPUT_STICK_* */
    int           line[PAD_CELLS];
} pad_set;

static pad_set s_pad[NFILES][PAD_KEYS];
static const char *const s_stick[] = { "left", "right", "none", NULL };

static const char *pad_key_name(int k)
{
    return k == PAD_LSTICK ? "left_stick" : k == PAD_RSTICK ? "right_stick"
         : xbox_InputMap_ControlName(k);
}

static int pad_owner_in_file(int f, int src, int *line)
{
    for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++)
        for (int k = 0; k < s_pad[f][c].n; k++)
            if (s_pad[f][c].src[k] == src) {
                *line = s_pad[f][c].line[k];
                return c;
            }
    return -1;
}

static void pad_add(int f, const recomp_cfg *cfg, const char *key, int ctl, const char *name)
{
    pad_set *s = &s_pad[f][ctl];
    int src = xbox_InputMap_SourceFromName(name), other, line;

    if (src < 0) {
        say(cfg, key, "unknown pad source '%s' in %s (see docs/runtime/config.md); skipped",
            name, key);
        return;
    }
    if (src == INPUT_SRC_NONE)
        return;
    other = pad_owner_in_file(f, src, &line);
    if (other == ctl)
        return;
    if (other >= 0) {
        say(cfg, key, "'%s' already mapped to " PAD_PREFIX "%s on line %d; ignored here",
            name, pad_key_name(other), line);
        return;
    }
    if (s->n == PAD_CELLS) {
        say(cfg, key, "%s has more than %d sources; '%s' ignored", key, PAD_CELLS, name);
        return;
    }
    /* Windows' Game Bar and Steam take Guide before the game sees it. */
    if (src == INPUT_SRC_GUIDE)
        say(cfg, key, "%s 'guide': most hosts keep Guide for themselves; mapped anyway", key);
    s->src[s->n] = (unsigned char)src;
    s->line[s->n] = recomp_cfg_line(cfg, key);
    s->n++;
}

static void pad_scan_stick(int f, const recomp_cfg *cfg, const char *key, int k)
{
    const char *v;

    if (recomp_cfg_type_of(cfg, key) != RCFG_STRING) {
        say(cfg, key, "%s is one of left, right, none (got %s); ignored", key,
            type_name(recomp_cfg_type_of(cfg, key)));
        return;
    }
    v = recomp_cfg_string(cfg, key, "");
    for (int i = 0; s_stick[i]; i++)
        if (!strcmp(v, s_stick[i])) {
            s_pad[f][k].named = 1;
            s_pad[f][k].src[0] = (unsigned char)i;   /* INPUT_STICK_LEFT, RIGHT, NONE */
            return;
        }
    say(cfg, key, "%s '%s' is not one of left, right, none; ignored", key, v);
}

static void pad_scan_file(int f)
{
    const recomp_cfg *cfg = enhance_cfg_file(f);

    for (size_t i = 0; i < recomp_cfg_count(cfg); i++) {
        const char *key = recomp_cfg_key_at(cfg, i), *name;
        recomp_cfg_type t;
        int ctl;

        if (strncmp(key, PAD_PREFIX, sizeof PAD_PREFIX - 1) != 0)
            continue;
        enhance_cfg_lookup(key);        /* read, for report_unused */
        name = key + sizeof PAD_PREFIX - 1;
        if (!strcmp(name, "left_stick") || !strcmp(name, "right_stick")) {
            pad_scan_stick(f, cfg, key, name[0] == 'l' ? PAD_LSTICK : PAD_RSTICK);
            continue;
        }
        ctl = xbox_InputMap_ControlFromName(name);
        if (ctl < 0 || ctl >= INPUT_PAD_CONTROL_COUNT) {
            char controls[512];
            size_t n = 0;
            for (int c = 0; c < PAD_KEYS && n < sizeof controls; c++)
                n += (size_t)snprintf(controls + n, sizeof controls - n, "%s%s", c ? " " : "",
                                      pad_key_name(c));
            say(cfg, key, "unknown control '%s' under [input.pad] (controls: %s); ignored",
                name, controls);
            continue;
        }
        t = recomp_cfg_type_of(cfg, key);
        if (t == RCFG_STRING) {
            s_pad[f][ctl].named = 1;
            pad_add(f, cfg, key, ctl, recomp_cfg_string(cfg, key, ""));
        } else if (t == RCFG_ARRAY) {
            size_t n = recomp_cfg_array_len(cfg, key), k;
            for (k = 0; k < n; k++)
                if (recomp_cfg_array_type(cfg, key, k) != RCFG_STRING) {
                    say(cfg, key, "%s is a string or a list of strings (got a list"
                        " holding %s); ignored", key,
                        type_name(recomp_cfg_array_type(cfg, key, k)));
                    break;
                }
            if (k < n)
                continue;
            s_pad[f][ctl].named = 1;    /* [] unbinds */
            for (k = 0; k < n; k++)
                pad_add(f, cfg, key, ctl, recomp_cfg_array_string(cfg, key, k, ""));
        } else {
            say(cfg, key, "%s is a string or a list of strings (got %s); ignored",
                key, type_name(t));
        }
    }
}

static int pad_taken_above(int f, int src)
{
    int line;
    for (int g = 0; g < f; g++)
        if (pad_owner_in_file(g, src, &line) >= 0)
            return 1;
    return 0;
}

/* One control's cells, less the sources a file above binds elsewhere. */
static void pad_put(xbox_pad_setup *s, int ctl, const unsigned char *src, int n, int above)
{
    int cell = 0;
    s->map.source[ctl] = s->alt.source[ctl] = INPUT_SRC_NONE;
    for (int k = 0; k < n; k++)
        if (src[k] != INPUT_SRC_NONE && !pad_taken_above(above, src[k]))
            (cell++ ? s->alt.source : s->map.source)[ctl] = src[k];
}

/* Resolve into s; counts[f] is how many keys file f decided. */
static void pad_resolve(xbox_pad_setup *s, int counts[NFILES])
{
    xbox_pad_setup id;

    xbox_PadSetupIdentity(&id);
    *s = id;
    for (int k = 0; k < PAD_KEYS; k++) {
        int f;
        for (f = 0; f < NFILES && !s_pad[f][k].named; f++)
            ;
        if (k >= INPUT_PAD_CONTROL_COUNT) {
            if (f < NFILES) {
                counts[f]++;
                *(k == PAD_LSTICK ? &s->map.left_stick : &s->map.right_stick) = s_pad[f][k].src[0];
            }
            continue;
        }
        if (f == NFILES) {
            unsigned char src = id.map.source[k];
            pad_put(s, k, &src, 1, NFILES);
            continue;
        }
        counts[f]++;
        pad_put(s, k, s_pad[f][k].src, s_pad[f][k].n, f);
    }
}

static void pad_init(int none)
{
    xbox_pad_setup s;
    int counts[NFILES] = { 0, 0, 0 };
    int dzl, dzr, glyphs;
    const char *env_dz = recomp_env(RENV_PAD_DEADZONE);

    memset(s_pad, 0, sizeof s_pad);
    for (int f = SLOT_DEF; f >= 0; f--)
        pad_scan_file(f);
    pad_resolve(&s, counts);
    /* Percent of full deflection, upstream's units; 0 keeps the raw stick
     * and RECOMP_PAD_DEADZONE, which stays the stock path's integer. */
    dzl = knob_int("input.pad_deadzone_left", 0, 95, 0);
    dzr = knob_int("input.pad_deadzone_right", 0, 95, 0);
    s.map.deadzone_left = s.alt.deadzone_left = (uint8_t)dzl;
    s.map.deadzone_right = s.alt.deadzone_right = (uint8_t)dzr;
    s.map.button_threshold = s.alt.button_threshold =
        (uint8_t)knob_int("input.pad_threshold", 1, 100, 50);
    s.invert_ly = (uint8_t)knob_bool("input.pad_invert_ly", 0);
    s.invert_ry = (uint8_t)knob_bool("input.pad_invert_ry", 0);
    s.square_left = (uint8_t)knob_bool("input.pad_square_left", 0);
    s.square_right = (uint8_t)knob_bool("input.pad_square_right", 0);
    if (env_dz && *env_dz && (dzl || dzr))
        fprintf(stderr, "[INPUT] config: input.pad_deadzone_%s wins over"
                " RECOMP_PAD_DEADZONE=%s on the %s\n",
                dzl && dzr ? "left/right" : dzl ? "left" : "right", env_dz,
                dzl && dzr ? "both sticks" : dzl ? "left stick" : "right stick");
    xbox_InputSetPadSetup(&s);

    glyphs = knob_choice("input.glyphs", s_glyphs, 0);
    xbox_InputSetGlyphFamily(glyphs ? glyphs - 1 : XBOX_PAD_FAMILY_AUTO);

    if (!none && !xbox_PadSetupIsIdentity(&s)) {
        int listed = 0;
        fprintf(stderr, "[INPUT] pad map:");
        for (int f = SLOT_DEF; f >= 0; f--) {
            const recomp_cfg *cfg = enhance_cfg_file(f);
            if (!cfg || !counts[f])
                continue;
            fprintf(stderr, listed++ ? " +%d (%s)" : " %d (%s)", counts[f], recomp_cfg_name(cfg));
        }
        if (dzl || dzr)
            fprintf(stderr, " deadzone=%d/%d%%", dzl, dzr);
        if (s.invert_ly || s.invert_ry)
            fprintf(stderr, " invert=%s%s", s.invert_ly ? "ly" : "", s.invert_ry ? "ry" : "");
        if (s.square_left || s.square_right)
            fprintf(stderr, " square=%s%s", s.square_left ? "l" : "", s.square_right ? "r" : "");
        fputc('\n', stderr);
    }
}

/* The release key and the walk key keep their own job when a control is
 * bound to the same key: one press then does both (frees the cursor and
 * presses the control, or caps the sticks and presses it). Say so rather
 * than guess which the player meant. */
static void warn_key_clash(const char *key, int code)
{
    if (!code)
        return;
    for (int ctl = 0; ctl < INPUT_CONTROL_COUNT; ctl++) {
        int codes[XBOX_BIND_MAX], mice[XBOX_BIND_MAX];
        int n = xbox_InputBindingsOf(ctl, codes, mice, XBOX_BIND_MAX);
        for (int k = 0; k < n; k++)
            if (!mice[k] && codes[k] == code) {
                fprintf(stderr, "[INPUT] config warning: %s '%s' is also bound to "
                        PREFIX "%s\n", key, xbox_InputCodeName(code, 0),
                        xbox_InputMap_ControlName(ctl));
            }
    }
}

/* The env value is the env path's (xbox_Input*On reads it), so only a file
 * value overrides; that keeps enhance_cfg_source_of truthful for a title
 * that asks where the switch came from. */
static int enable_from(const char *key, int on)
{
    enhance_cfg_source s = enhance_cfg_source_of(key);
    return s == ENHANCE_SRC_FILE || s == ENHANCE_SRC_DEFAULTS ? on : -1;
}

int enhance_input_init(void)
{
    xbox_mouse_opts mo;
    int counts[NFILES] = { 0, 0, 0 };
    int keyboard, mouse, ramp, walk_code;
    double walk_scale;
    const char *over = recomp_env(RENV_CONFIG);
    int none = over && !strcmp(over, "none");

    enhance_cfg_bind_env("input.keyboard", RENV_KEYBOARD);
    enhance_cfg_bind_env("input.mouse", RENV_MOUSE);

    keyboard = knob_bool("input.keyboard", 0);
    mouse = knob_bool("input.mouse", 0);
    xbox_InputKeyboardEnable(enable_from("input.keyboard", keyboard));
    xbox_InputMouseEnable(enable_from("input.mouse", mouse));

    xbox_InputMouseDefaultOpts(&mo);
    mo.look = knob_choice("input.mouse_look", s_look, XBOX_MOUSE_LOOK_RIGHT);
    mo.sensitivity = knob_float("input.mouse_sensitivity", 0.1, 10.0, DEF_SENS, 0);
    mo.invert_y = knob_bool("input.mouse_invert_y", 0);
    mo.invert_x = knob_bool("input.mouse_invert_x", 0);
    mo.min_deflection = knob_float("input.mouse_min_deflection", 0.0, 0.9, DEF_MIN_DEFL, 0);
    mo.capture = knob_choice("input.mouse_capture", s_capture, XBOX_MOUSE_CAPTURE_FOCUS);
    mo.release_key = knob_key("input.mouse_release_key", "F9");
    mo.wheel_pulse_ms = knob_int("input.wheel_pulse_ms", 20, 2000, DEF_PULSE_MS);
    xbox_InputMouseOpts(&mo);

    ramp = knob_int("input.stick_ramp_ms", 0, 2000, DEF_RAMP_MS);
    walk_code = knob_key("input.walk_key", DEF_WALK_KEY);
    walk_scale = knob_float("input.walk_scale", XBOX_KBM_STICK_FLOOR, 1.0, DEF_WALK_SCALE, 1);
    xbox_InputKeyboardOpts(ramp, walk_code, walk_scale);

    memset(s_set, 0, sizeof s_set);
    for (int f = SLOT_DEF; f >= 0; f--)
        scan_file(f);
    apply_bindings(counts);
    /* Published as one table the poll swaps in, so a reload never edits
     * what a poll in flight composes from. */
    xbox_InputBindingsCommit();
    pad_init(none);
    warn_key_clash("input.mouse_release_key", mo.release_key);
    warn_key_clash("input.walk_key", walk_code);
    if (walk_code && walk_code == mo.release_key)
        fprintf(stderr, "[INPUT] config warning: input.walk_key and "
                "input.mouse_release_key are the same key\n");

    keyboard = xbox_InputKeyboardOn();
    mouse = xbox_InputMouseOn();
    if (none) {
        fprintf(stderr, "[INPUT] config: keyboard=%s mouse=%s (RECOMP_CONFIG=none)\n",
                keyboard ? "on" : "off", mouse ? "on" : "off");
    } else {
        int listed = 0;
        fprintf(stderr, "[INPUT] config: keyboard=%s mouse=%s look=%s sens=%.1f",
                keyboard ? "on" : "off", mouse ? "on" : "off", s_look[mo.look],
                mo.sensitivity);
        /* Lowest layer first, as the [ENHANCE] line lists the files: the
         * first with its count, each higher one that decided a control
         * with +count. */
        for (int f = SLOT_DEF; f >= 0; f--) {
            const recomp_cfg *cfg = enhance_cfg_file(f);
            if (!cfg || (listed && !counts[f]))
                continue;
            fprintf(stderr, listed++ ? " +%d (%s)" : " bindings=%d (%s)",
                    counts[f], recomp_cfg_name(cfg));
        }
        if (!listed)
            fprintf(stderr, " bindings=0");
        fputc('\n', stderr);
    }
    /* The script plays through the keyboard and mouse poll, which runs only
     * with a device on; without this line the script just never happens. */
    {
        const char *script = recomp_env(RENV_KBM_SCRIPT);
        if (script && *script && !keyboard && !mouse)
            fprintf(stderr, "[INPUT] kbm_script ignored: keyboard and mouse off"
                    " (%s)\n", script);
    }
    fflush(stderr);
    return 0;
}

/* A live reload (the menu's Apply, R1): the knobs and the bindings again,
 * then the actions, whose rule 2 checks against the new key map. */
int enhance_input_reload(void)
{
    enhance_input_init();
    enhance_actions_rebuild();
    return 0;
}

/* ---- a mod's input ---------------------------------------------------- */

static int s_mod_capture;
/* While the menu holds the mouse (enhance_input_menu_hold) a mod's capture is
 * recorded but the divert stays off. The two run on different threads (a
 * mod's call on the guest's or a hook's, the menu on the guest's), so one
 * short lock keeps the record and the divert in step: without it a capture
 * released between the menu's read and its divert would leave the divert on. */
static int s_menu_hold;
static int s_div_lock;

static void div_lock(void)
{
    while (__atomic_exchange_n(&s_div_lock, 1, __ATOMIC_ACQUIRE))
        ;
}

static void div_unlock(void)
{
    __atomic_store_n(&s_div_lock, 0, __ATOMIC_RELEASE);
}

void enhance_input_capture(int on)
{
    on = on != 0;
    div_lock();
    if (__atomic_exchange_n(&s_mod_capture, on, __ATOMIC_ACQ_REL) != on && !s_menu_hold)
        xbox_InputMouseDivert(on);
    div_unlock();
}

void enhance_input_menu_hold(int on)
{
    on = on != 0;
    div_lock();
    if (on != s_menu_hold) {
        s_menu_hold = on;
        /* The cursor is freed for the menu's pointer; the mod in flight (the
         * free camera) gets no motion while the menu is open. On close the
         * divert follows the capture as it is now, so a capture the mod
         * took or gave back meanwhile counts; the divert's edge drops the
         * counts gathered so far, so the camera does not jump. */
        xbox_InputMouseSuspend(on);
        xbox_InputMouseDivert(on ? 0 : __atomic_load_n(&s_mod_capture, __ATOMIC_ACQUIRE));
    }
    div_unlock();
}

int enhance_input_captured(void)
{
    return __atomic_load_n(&s_mod_capture, __ATOMIC_ACQUIRE);
}

void enhance_input_state(struct enhance_input_state *out)
{
    int b;

    memset(out, 0, sizeof *out);
    out->connected = xbox_InputLastState(0, &out->pad) == ERROR_SUCCESS;
    xbox_InputMouseTakeCounts(&out->mouse_dx, &out->mouse_dy);
    for (b = XBOX_MOUSE_LEFT; b <= XBOX_MOUSE_5; b++)
        if (xbox_InputMouseHeld(b))
            out->mouse_buttons |= 1u << (b - 1);
    out->captured = enhance_input_captured();
}
