/*
 * demo_mod.c - the smallest mod that shows every part a mod usually has: a
 * descriptor with an enable switch and a setting, a rebindable control, a
 * frame callback, a status panel, and a hook when the game gives one.
 *
 * It counts frames and shows the count on a status panel. It needs no game
 * address, so it builds into any title, and it reads and writes no guest
 * memory unless a hook is compiled in. Off by default, and off it does
 * nothing: its frame callback returns at its first test, it draws no panel
 * and its hook is not called. docs/runtime/mod-howto.md walks through it.
 *
 * Copy this file and its header into the game's src/mods/, rename them, and
 * replace what on_frame does. tests/mod_demo builds this file as it is.
 *
 * To see a hook, build with DEMO_MOD_HOOK_VA set to a function the game
 * lists in game.toml [mods] hook_functions: the panel then also counts that
 * function's calls and shows its first stack argument.
 */
#include <stddef.h>
#include "demo_mod.h"

/* A game pins one toolkit, so the guard only has to stop the build of a mod
 * newer than the pin. The menu's and the toolkit's headers are there only in
 * a build with the enhancements layer. */
#if defined(RECOMP_ENV_HAVE_ENHANCE_KEYS) && defined(__has_include)
#  if __has_include("mod.h")
#    include "mod.h"
#    if XBOX_MOD_API_VERSION >= 7
#      define DEMO_MOD 1
#    endif
#  endif
#endif

#ifdef DEMO_MOD

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "enhance_menu.h"

/* Settings and controls are read by index, so name the indexes. */
enum { SET_ENABLE, SET_INTERVAL, SET_N };
enum { CTL_RESET, CTL_N };

static const struct xbox_mod_setting k_set[SET_N] = {
    /* name, key, env, aliases, label, help, unit, kind, live, flags,
     * def, lo, hi, step, choices */
    [SET_ENABLE] = { "enable", NULL, NULL, NULL, "Frame counter",
      "Shows how many frames the game has run since the last reset.",
      NULL, XBOX_SET_BOOL, ENHANCE_LIVE, XBOX_SET_ENABLE, 0, 0, 1, 1, NULL },
    [SET_INTERVAL] = { "interval", NULL, NULL, NULL, "Refresh every",
      "How often the panel updates.",
      "frames", XBOX_SET_INT, ENHANCE_LIVE, 0, 30, 1, 600, 1, NULL },
};

static const char *const k_reset_def[] = { "F8", NULL };

static const struct xbox_mod_control k_ctl[CTL_N] = {
    /* id, label, help, when, kind, defaults, aliases, env */
    [CTL_RESET] = { "reset", "Reset the count", NULL, NULL, XBOX_CTL_ACTION, k_reset_def,
                    NULL, NULL },
};

static const struct xbox_mod_desc k_desc = {
    XBOX_MOD_DESC_VERSION, sizeof k_desc, "Frame counter",
    "The template mod: counts frames and shows them on a status panel.",
    NULL, "1",
    k_set, SET_N, k_ctl, CTL_N
};

/* The table holds a pointer to this and the descriptor is const data, so the
 * Mods page can show the mod without running it. */
static const struct xbox_mod s_mod;
static const struct xbox_mod_api *s_api;

/* All on the guest's thread: on_frame, on_enable and on_setting run there. */
static int s_on, s_reset_id;
static uint32_t s_frames, s_since_draw, s_interval;
static char s_shown[XBOX_MOD_STATUS_LINE];

#ifdef DEMO_MOD_HOOK_VA
#include <stdatomic.h>

/* A hook runs on whichever guest thread calls the function, and the panel
 * reads these on the frame's, so they are atomics. */
static _Atomic uint32_t s_calls, s_last_arg;

static void on_call(struct xbox_mod_call *c, void *user)
{
    (void)user;
    atomic_fetch_add(&s_calls, 1);
    atomic_store(&s_last_arg, XBOX_MOD_ARG32(s_api, c, 0));
}
#endif

static void draw_panel(void)
{
    char text[sizeof s_shown];

#ifdef DEMO_MOD_HOOK_VA
    snprintf(text, sizeof text, "Frames\n%u\ncalls %u arg %08X\n{ctl:reset} resets", s_frames,
             (unsigned)atomic_load(&s_calls), (unsigned)atomic_load(&s_last_arg));
#else
    snprintf(text, sizeof text, "Frames\n%u\n{ctl:reset} resets", s_frames);
#endif
    /* Tokens stay as written and expand at each draw, so the panel follows a
     * rebinding and the device in use. Only a change costs a call. */
    if (strcmp(text, s_shown) == 0)
        return;
    memcpy(s_shown, text, sizeof s_shown);
    s_api->status(&s_mod, text);
}

static void on_frame(void *user)
{
    (void)user;
    /* The toolkit keeps calling this while the mod is off: gating is ours. */
    if (!s_on)
        return;
    s_frames++;
    if (s_api->action_pressed(s_reset_id)) {
        s_frames = 0;
        s_since_draw = s_interval;      /* show it now */
    }
    if (++s_since_draw >= s_interval) {
        s_since_draw = 0;
        draw_panel();
    }
}

/* May be told the state it is already in, so it must be idempotent. */
static void on_enable(int on, void *user)
{
    (void)user;
    if (on == s_on)
        return;
    s_on = on;
    /* The toolkit drops the panel when a mod goes off, so a new one comes
     * with the next frame after it is back. */
    s_frames = 0;
    s_since_draw = s_interval;
    s_shown[0] = '\0';
}

static void on_setting(uint32_t idx, double value, void *user)
{
    (void)user;
    if (idx == SET_INTERVAL)
        s_interval = (uint32_t)value;
}

static int init(const struct xbox_mod_api *api)
{
    s_api = api;
    s_frames = 0;
    s_shown[0] = '\0';
#ifdef DEMO_MOD_HOOK_VA
    atomic_store(&s_calls, 0);
    atomic_store(&s_last_arg, 0);
#endif
    /* A mod built against v7 checks before it uses the panel. Non-zero leaves
     * the mod out with its registrations dropped. */
    if (api->size < offsetof(struct xbox_mod_api, status) + sizeof api->status || !api->status) {
        api->log(&s_mod, "no status panel in this toolkit: left out");
        return 1;
    }
    if (!api->on_frame(on_frame, NULL)) {
        api->log(&s_mod, "no frame slot: left out");
        return 1;
    }
#ifdef DEMO_MOD_HOOK_VA
    /* A slot the game does not declare is one [MOD] line and a 0: the panel
     * still works, so the mod stays. A mod that needs its hook returns 1. */
    if (!api->hook(DEMO_MOD_HOOK_VA, on_call, NULL, 0, NULL))
        api->log(&s_mod, "no hook slot: the panel counts frames only");
#endif
    /* The toolkit registered the control's action before this init. Its key
     * only fires, and the panel only draws, while a feature key arms the
     * actions tick: the menu arms it when on, and this for a run without. */
    s_reset_id = api->control_action(&k_desc, CTL_RESET);
    api->actions_arm("mods.demo.enable");
    s_interval = (uint32_t)api->setting(&k_desc, SET_INTERVAL);
    /* Inited dormant (off, the menu on): the menu may turn it on later. */
    s_on = api->setting(&k_desc, SET_ENABLE) != 0;
    s_since_draw = s_interval;
    api->on_enable(on_enable, NULL);
    api->on_setting(&k_desc, on_setting, NULL);
    if (s_on)
        api->log(&s_mod, "on");
    return 0;
}

static const struct xbox_mod s_mod = {
    "demo", init, NULL, &k_desc,
#ifdef DEMO_MOD_HOOK_VA
    XBOX_MOD_NEED_HOOK,
#else
    0,
#endif
    NULL
};

const void *demo_mod(void)
{
    return &s_mod;
}

#else /* no mod API v7 in this build: no mod */

const void *demo_mod(void)
{
    return NULL;
}

#endif
