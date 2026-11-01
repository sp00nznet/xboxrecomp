/*
 * enhance.c - see enhance.h. Reads the toolkit's keys once and stores them
 * as nv2a_host_opts; the per-frame paths only ever read that struct.
 */
#include "enhance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_actions.h"
#include "enhance_cfg.h"
#include "enhance_input.h"
#include "enhance_menu.h"
#include "hud_anchor.h"
#include "kernel_pacing.h"
#include "kernel_ramfs.h"
#include "mod.h"
#include "nv2a_backend_common.h"
#include "nv2a_overlay.h"
#include "nv2a_present_chain.h"
#include "nv2a_tex_pack.h"
#include "recomp_exe_dir.h"
#include "shader_profile.h"

#define LOGP "[ENHANCE] "
#define SCALE_MAX 4

static int path_is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\'
        || (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
}

/* display.hud_aspect and display.hud_margin (hud_anchor.h): the HUD
 * layer's edge, read whether or not a mod registers a HUD table so they are
 * never reported unused. hud_aspect "display" follows display.aspect; a
 * W:H no wider than 4:3 keeps the HUD in the 4:3 columns. */
static void read_hud_keys(char *txt, size_t len)
{
    const char *v = enhance_cfg_string("display.hud_aspect", "display");
    long long margin = enhance_cfg_int("display.hud_margin", 0);
    unsigned aw = 0, ah = 0;

    if (strcmp(v, "display") != 0) {
        int end = 0;
        if (sscanf(v, "%u:%u%n", &aw, &ah, &end) != 2 || v[end] || !aw || !ah) {
            fprintf(stderr, LOGP "display.hud_aspect=%s is not display or a W:H ratio;"
                    " using display\n", v);
            aw = ah = 0;
            v = "display";
        } else if ((unsigned long long)aw * 3u < (unsigned long long)ah * 4u) {
            fprintf(stderr, LOGP "display.hud_aspect=%s is narrower than 4:3; using 4:3\n", v);
            aw = 4;
            ah = 3;
            v = "4:3";
        }
    }
    if (margin < 0 || margin > 64) {
        long long c = margin < 0 ? 0 : 64;
        fprintf(stderr, LOGP "display.hud_margin=%lld out of range 0..64; using %lld\n",
                margin, c);
        margin = c;
    }
    hud_layer_set_keys(aw, ah, (int)margin);
    snprintf(txt, len, "display.hud_aspect=%s display.hud_margin=%lld", v, margin);
}

#define SHADER_PARAMS "present.shader_params."

/* [present.shader_params.<preset>] <param> = <number>: the overrides of
 * every preset, defaults file first so the user and title files win. No env
 * tier: there is one key per parameter of a preset the code does not know
 * until the chain is built. A key with a name the preset lacks stays
 * harmless (librashader ignores an unknown parameter). */
static void load_shader_params(void)
{
    for (int f = 2; f >= 0; f--) {
        const recomp_cfg *c = enhance_cfg_file(f);
        const char *k;
        for (size_t i = 0; c && (k = recomp_cfg_key_at(c, i)); i++) {
            const char *rest, *dot;
            char key[64];
            recomp_cfg_type t;
            if (strncmp(k, SHADER_PARAMS, sizeof SHADER_PARAMS - 1))
                continue;
            enhance_cfg_lookup(k);   /* read here, not unused */
            rest = k + sizeof SHADER_PARAMS - 1;
            dot = strchr(rest, '.');
            t = recomp_cfg_type_of(c, k);
            if (!dot || dot == rest || !dot[1] || strchr(dot + 1, '.')
                || (size_t)(dot - rest) >= sizeof key || (t != RCFG_FLOAT && t != RCFG_INT)) {
                fprintf(stderr, LOGP "config warning: %s:%d: %s is not"
                        " present.shader_params.<preset>.<param> = <number>\n",
                        recomp_cfg_name(c), recomp_cfg_line(c, k), k);
                continue;
            }
            memcpy(key, rest, (size_t)(dot - rest));
            key[dot - rest] = 0;
            nv2a_present_chain_override_load(key, dot + 1, (float)recomp_cfg_float(c, k, 0));
        }
    }
}

/* textures.pack: a pack file, relative to the executable's directory,
 * where config.default.toml (which --hud-pack writes the key into) and the
 * pack ship, so a bundle names enhance/hud.pack; not relative to `root`,
 * the user data root. A bad or missing pack is said and the run stays
 * stock; nothing in the backends runs until nv2a_tex_pack_count() > 0. */
static void load_tex_pack(const struct nv2a_host_opts *o)
{
    const char *root = recomp_exe_dir();
    const char *pack = enhance_cfg_string("textures.pack", "");
    char path[1024], err[128];
    int n;

    if (!pack || !*pack || !strcmp(pack, "none")) {
        nv2a_tex_pack_unload();     /* a re-init's "none" is true too */
        fprintf(stderr, LOGP "textures.pack=none\n");
        return;
    }
    if (root && *root && !path_is_absolute(pack))
        snprintf(path, sizeof path, "%s/%s", root, pack);
    else
        snprintf(path, sizeof path, "%s", pack);
    n = nv2a_tex_pack_load(path, err, sizeof err);
    if (n < 0)
        fprintf(stderr, LOGP "textures.pack=%s: %s (%s); no pack\n", pack, err, path);
    else
        fprintf(stderr, LOGP "textures.pack=%s (%d entries: %u art, %u font, %u text;"
                " scale %u, text %u; %u bc3) textures.font=%s textures.text=%s\n",
                pack, n, nv2a_tex_pack_kind_count(XRTP_KIND_ART),
                nv2a_tex_pack_kind_count(XRTP_KIND_FONT),
                nv2a_tex_pack_kind_count(XRTP_KIND_TEXT), nv2a_tex_pack_scale(),
                nv2a_tex_pack_text_scale(), nv2a_tex_pack_bc3_count(),
                (o->pack_off & NV2A_PACK_OFF_FONT) ? "off" : "on",
                (o->pack_off & NV2A_PACK_OFF_TEXT) ? "off" : "on");
}

static const char *const s_filters[] = { "nearest", "linear", "integer", NULL };
/* Indexes match XBOX_PACING_SPIN / XBOX_PACING_SLEEP. */
static const char *const s_pacing[] = { "spin", "sleep", NULL };
/* present.shader_lines, in NV2A_PRESENT_LINES_* order. */
static const char *const s_shader_lines[] = { "auto", "guest", "host", NULL };
/* present.surface: "auto" (index 0) is resolved here, never stored. */
static const char *const s_surface[] = { "auto", "window", "internal", NULL };
/* textures.font / textures.text: words, as the menu shows them. */
static const char *const s_onoff[] = { "off", "on", NULL };
/* Indexes match enum nv2a_movie_mode. */
static const char *const s_movie[] = { "black", "fit", "fill", NULL };

int xbox_enhance_init(const char *root, const char *title)
{
    struct nv2a_host_opts o;
    const char *aspect, *shader_lib;
    long long scale, aniso, overscan;
    int rc, pacing, surface, shader_lines;
    char surface_txt[48], overscan_txt[64] = "", hud_txt[96] = "", shader[512];

    rc = enhance_cfg_init(root, title) < 0 ? -1 : 0;
    memset(&o, 0, sizeof o);

    scale = enhance_cfg_int("render.scale", 1);
    if (scale < 1 || scale > SCALE_MAX) {
        long long c = scale < 1 ? 1 : SCALE_MAX;
        fprintf(stderr, LOGP "render.scale=%lld out of range 1..%d; using %lld\n",
                scale, SCALE_MAX, c);
        scale = c;
    }
    o.render_scale = (unsigned)scale;
    /* Anisotropic filtering: 0 or 1 off (stock), else 2..16. */
    aniso = enhance_cfg_int("render.aniso", 0);
    if (aniso < 0 || aniso > 16) {
        long long c = aniso < 0 ? 0 : 16;
        fprintf(stderr, LOGP "render.aniso=%lld out of range 0..16; using %lld\n",
                aniso, c);
        aniso = c;
    }
    o.aniso = aniso < 2 ? 0 : (unsigned)aniso;
    /* 2D on the guest grid above scale 1 (nv2a_snap_2d_on): off is stock,
     * and scale 1 ignores it. */
    o.snap_2d = enhance_cfg_bool("render.snap_2d", 0);
    /* Indexes match enum nv2a_present_filter. */
    o.present_filter = enhance_cfg_choice("present.filter", s_filters, NV2A_PRESENT_NEAREST);
    o.fullscreen = enhance_cfg_bool("present.fullscreen", 0);
    /* How lowered guest spin waits wait (kernel_pacing.h): only titles
     * generated with --spin-waits have any. Set before the guest starts.
     * The default sleeps: the wait frees a host core and the frames and
     * pace match the spin. "spin" is the console-original timing, where
     * the wait holds a core as on the Xbox; a title's config.default.toml
     * can keep it, and without this layer the kernel stays at spin. */
    pacing = enhance_cfg_choice("present.pacing", s_pacing, XBOX_PACING_SLEEP);
    xbox_PacingSetMode(pacing);
    /* The cache partition in memory (kernel_ramfs.h): on by default, an
     * exception to stock-off. Titles rewrite their cache partition as they
     * play, gigabytes an hour of host SSD writes for data the console's
     * dashboard throws away anyway. Set before the guest opens a
     * file; the store fixes on or off at its first use. */
    xbox_RamCacheEnable(enhance_cfg_bool("storage.ram_cache", 1));
    /* Who scales the frame to the screen. Under gamescope the compositor
     * should: its filter (Steam's FSR) upscales only a surface smaller than
     * its output, and a window at the screen size leaves it nothing to do.
     * The runtime does not look for gamescope itself; the SteamOS launcher
     * says so through RECOMP_HOST_GAMESCOPE, and auto follows it. */
    surface = enhance_cfg_choice("present.surface", s_surface, 0);
    if (surface == 0) {
        int gs = recomp_env_on(RENV_HOST_GAMESCOPE);
        o.present_surface = gs ? NV2A_PRESENT_SURFACE_INTERNAL : NV2A_PRESENT_SURFACE_WINDOW;
        snprintf(surface_txt, sizeof surface_txt, "%s(auto%s)",
                 gs ? "internal" : "window", gs ? ": gamescope" : "");
    } else {
        o.present_surface = surface == 2 ? NV2A_PRESENT_SURFACE_INTERNAL
                                         : NV2A_PRESENT_SURFACE_WINDOW;
        snprintf(surface_txt, sizeof surface_txt, "%s", s_surface[surface]);
    }

    /* Hor+: "W:H" wider than 4:3 pads the 4:3-shaped and square targets (nv2a_host_pad)
     * and the title's frame sits centred in them; 4:3 is stock. The GPU
     * backends honour it; the CPU rasteriser draws 4:3 (it says so). The
     * HUD keeps its 4:3 layout: there is no option for it. The movie key
     * only matters with a wide aspect, but is read either way so it is
     * never reported unused. */
    aspect = enhance_cfg_string("display.aspect", "4:3");
    if (strcmp(aspect, "4:3") != 0) {
        unsigned aw = 0, ah = 0;
        int end = 0;
        /* the whole value: "16:9x" is a typo, not 16:9 */
        if (sscanf(aspect, "%u:%u%n", &aw, &ah, &end) == 2 && !aspect[end]
                && aw && ah && aw * 3u > ah * 4u) {
            o.aspect_w = aw;
            o.aspect_h = ah;
        } else {
            fprintf(stderr, LOGP "display.aspect=%s is not a W:H ratio wider than"
                    " 4:3; using 4:3\n", aspect);
            aspect = "4:3";
        }
    }
    o.movie_mode = enhance_cfg_choice("display.movie", s_movie, NV2A_MOVIE_BLACK);
    read_hud_keys(hud_txt, sizeof hud_txt);
    /* A TV's overscan: guest px cut from each side edge at present, the
     * top and bottom in proportion (nv2a_present_crop), for a title that
     * left a border its CRT hid. Under hor+ the 4:3 edges sit inside the
     * padded frame, where no source rectangle can cut them: refused. */
    overscan = enhance_cfg_int("display.overscan", 0);
    if (overscan < 0 || overscan > NV2A_OVERSCAN_MAX) {
        long long c = overscan < 0 ? 0 : NV2A_OVERSCAN_MAX;
        fprintf(stderr, LOGP "display.overscan=%lld out of range 0..%u; using %lld\n",
                overscan, NV2A_OVERSCAN_MAX, c);
        overscan = c;
    }
    if (overscan && o.aspect_w) {
        fprintf(stderr, LOGP "display.overscan=%lld ignored with display.aspect=%s:"
                " the 4:3 edges are inside the padded frame; using 0\n", overscan, aspect);
        overscan = 0;
    }
    o.overscan = (unsigned)overscan;
    if (o.overscan)
        snprintf(overscan_txt, sizeof overscan_txt, " display.overscan=%u", o.overscan);
    /* Named only when on, as the overscan: the stock line stays as it was. */
    if (o.snap_2d)
        snprintf(overscan_txt + strlen(overscan_txt), sizeof overscan_txt - strlen(overscan_txt),
                 " render.snap_2d=1");

    /* The pack's kinds a player can turn off; read whether or not a pack
     * is configured, so the keys are never reported unused. */
    if (!enhance_cfg_choice("textures.font", s_onoff, 1))
        o.pack_off |= NV2A_PACK_OFF_FONT;
    if (!enhance_cfg_choice("textures.text", s_onoff, 1))
        o.pack_off |= NV2A_PACK_OFF_TEXT;

    nv2a_host_opts_set(&o);
    fprintf(stderr, LOGP "render.scale=%u render.aniso=%u present.filter=%s"
            " present.fullscreen=%d present.pacing=%s present.surface=%s"
            " (display.aspect=%s%s)\n", o.render_scale, o.aniso,
            s_filters[o.present_filter], o.fullscreen, s_pacing[pacing], surface_txt, aspect,
            overscan_txt);
    if (o.aspect_w)
        fprintf(stderr, LOGP "display.aspect=%s display.movie=%s\n", aspect,
                s_movie[o.movie_mode]);
    /* A slang preset or a shader profile in place of the present draw
     * (nv2a_present_chain.h, shader_profile.h). Only the request is stored
     * here: the library loads at the first present that wants it, so a
     * stock start never touches it, nor any profile file. */
    shader_lib = enhance_cfg_string("present.shader_lib", "");
    nv2a_present_chain_set_lib(shader_lib);
    /* A config key only, no env row: it matters only with a shader on,
     * and a preset the name test misses is a config fix, not a debug one.
     * Said only when set, so the stock start line stays as it was. */
    shader_lines = enhance_cfg_choice("present.shader_lines", s_shader_lines,
                                      NV2A_PRESENT_LINES_AUTO);
    nv2a_present_chain_set_lines(shader_lines);
    if (shader_lines != NV2A_PRESENT_LINES_AUTO)
        fprintf(stderr, LOGP "present.shader_lines=%s\n", s_shader_lines[shader_lines]);
    load_shader_params();
    switch (shader_profile_start(root, shader, sizeof shader)) {
    case 1:
        fprintf(stderr, LOGP "present.shader=%s%s%s\n", shader,
                shader_lib && *shader_lib ? " present.shader_lib=" : "",
                shader_lib && *shader_lib ? shader_lib : "");
        break;
    case 2:
        /* Logged only when chosen, as present.shader: the golden check
         * reads "[ENHANCE] present.profile=" for its stock value. */
        shader_profile_start_slug(shader, sizeof shader);
        fprintf(stderr, LOGP "present.profile=%s\n", shader);
        break;
    }
    load_tex_pack(&o);
    enhance_input_init();
    /* The toolkit's host actions before the mods register theirs. */
    enhance_actions_init();
    /* The descriptor dump (RECOMP_MODS_DESCRIBE=json, mod.h): every value and
     * binding resolved as a start would, then the JSON and exit, with no
     * mod init, no hook and no hook line. Before any window, audio or guest,
     * which the title opens after this call. */
    if (xbox_mod_describe_wanted()) {
        xbox_mod_init_describe();
        enhance_actions_rebuild();
        exit(xbox_mod_describe_write(recomp_env(RENV_MODS_DESCRIBE_OUT)));
    }
    /* Last: a mod's init reads its keys over every file and the [input]
     * keys already applied (mod.h). */
    xbox_mod_init();
    /* The HUD keys only mean something once a mod registered a HUD table,
     * so they are named only then: the stock line stays as it was. */
    /* Only with a wide aspect (a pad): a HUD mod at 4:3 adds no line, so
     * the goldens' startup log is the same with the mod loaded. */
    if (hud_layer_has_mod_table() && hud_layer_pad() > 0)
        fprintf(stderr, LOGP "%s hud_pad=%d\n", hud_txt, hud_layer_pad_hud());
    /* [input.actions] once every action is registered (the key map of
     * rule 2 is the one enhance_input_init applied), then the hook line. */
    enhance_actions_start();
    /* The stats strip (R2) and the toasts. Both layers are armed here, on
     * the init thread, whenever the actions hook is (a feature key is on,
     * rule 6): the overlay.stats action and a mod's toast can then show
     * them later without arming under a running presenter. Stock leaves
     * both the hook and the overlay disarmed, so a golden run has no atlas
     * line; a toast from a mod's init, before this, is its log line only.
     * Armed but hidden, nothing is drawn; the cost is the font bake and
     * two layers' lists. overlay.stats is read once, by the hook's arming
     * (enhance_actions_start). */
    /* overlay.ui_scale (D2) before any arm: the atlas is baked at the
     * first, so the key is read once and a change waits for a restart. A
     * line only when it is set, so the stock log stays as it was. */
    {
        double want = enhance_cfg_float("overlay.ui_scale", 1.0);
        float got = nv2a_overlay_set_ui_scale((float)want);
        if (enhance_cfg_source_of("overlay.ui_scale") != ENHANCE_SRC_DEFAULT)
            fprintf(stderr, LOGP "overlay.ui_scale=%g%s\n", got,
                    (float)want != got ? " (clamped to 0.75..2.0)" : "");
    }
    {
        int stats = enhance_actions_keyed("overlay.stats");
        fprintf(stderr, LOGP "overlay.stats=%s\n", stats ? "on" : "off");
        if (enhance_actions_armed()
            && nv2a_overlay_arm((1u << NV2A_OVL_STATS) | (1u << NV2A_OVL_TOAST), 1080) == 0
            && stats)
            nv2a_overlay_set_stats(1);
    }
    /* The menu (menu.enabled) arms its own layer and the actions hook's
     * menu step; off, it logs that and nothing more. */
    enhance_menu_start();
    /* No enhance_cfg_report_unused() here: the title reads its own keys
     * (game.mode, say) after this, and calls it then. */
    return rc;
}

void xbox_enhance_set_pacing(int mode)
{
    if (mode != XBOX_PACING_SPIN && mode != XBOX_PACING_SLEEP)
        return;
    xbox_PacingSetMode(mode);
    fprintf(stderr, LOGP "present.pacing=%s (live)\n", s_pacing[mode]);
}
