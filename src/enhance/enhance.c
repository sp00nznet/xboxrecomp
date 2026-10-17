/*
 * enhance.c - see enhance.h. Reads the toolkit's keys once and stores them
 * as nv2a_host_opts; the per-frame paths only ever read that struct.
 */
#include "enhance.h"

#include <stdio.h>
#include <string.h>

#include "enhance_cfg.h"
#include "enhance_input.h"
#include "kernel_pacing.h"
#include "nv2a_backend_common.h"
#include "nv2a_tex_pack.h"
#include "recomp_exe_dir.h"

#define LOGP "[ENHANCE] "
#define SCALE_MAX 4

static int path_is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\'
        || (((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':');
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
/* present.surface: "auto" (index 0) is resolved here, never stored. */
static const char *const s_surface[] = { "auto", "window", "internal", NULL };
/* textures.font / textures.text: words, as the menu shows them. */
static const char *const s_onoff[] = { "off", "on", NULL };

int xbox_enhance_init(const char *root, const char *title)
{
    struct nv2a_host_opts o;
    const char *aspect;
    long long scale, aniso;
    int rc, pacing, surface;
    char surface_txt[48];

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

    /* Hor+ is a later slice; a file or env that asks for it must not be
     * ignored silently. */
    aspect = enhance_cfg_string("display.aspect", "4:3");
    if (strcmp(aspect, "4:3") != 0) {
        fprintf(stderr, LOGP "display.aspect=%s not implemented yet (hor+ is a"
                " later slice); using 4:3\n", aspect);
        aspect = "4:3";
    }

    /* The pack's kinds a player can turn off; read whether or not a pack
     * is configured, so the keys are never reported unused. */
    if (!enhance_cfg_choice("textures.font", s_onoff, 1))
        o.pack_off |= NV2A_PACK_OFF_FONT;
    if (!enhance_cfg_choice("textures.text", s_onoff, 1))
        o.pack_off |= NV2A_PACK_OFF_TEXT;

    nv2a_host_opts_set(&o);
    fprintf(stderr, LOGP "render.scale=%u render.aniso=%u present.filter=%s"
            " present.fullscreen=%d present.pacing=%s present.surface=%s"
            " (display.aspect=%s)\n", o.render_scale, o.aniso,
            s_filters[o.present_filter], o.fullscreen, s_pacing[pacing], surface_txt, aspect);
    load_tex_pack(&o);
    enhance_input_init();
    /* No enhance_cfg_report_unused() here: the title reads its own keys
     * (game.mode, say) after this, and calls it then. */
    return rc;
}
