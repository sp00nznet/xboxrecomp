/*
 * enhance_actions.c - see enhance_actions.h. The rules (docs/runtime/
 * config.md, "[input.actions]"), so a binding never steals what the title
 * uses:
 *
 *   1. a single source the pad map gives the game is refused; sources the
 *      game uses only in a chord of two or more with a hold of at least
 *      200 ms;
 *   2. a key the guest receives (the runtime key table, the walk key, the
 *      mouse release key; modifiers count) is refused;
 *   3. a chord is consumed: from the moment all its buttons are down the
 *      guest sees them released until each is physically released, and
 *      input.chord_delay_ms holds a chord member back from the guest for
 *      that long after its press;
 *   5. actions read the host pad, never the state a script made;
 *   6. only feature keys arm the hook;
 *   7. a modifier chord (guide+a) needs its unmapped members first; a
 *      mapped member pressed while they are held is the guest's no more
 *      until its release, even when a modifier is let go first; one pressed
 *      before them passes and does not complete the chord.
 *   8. one input fires one action: of two bindings that take the same
 *      input, menu.open's keeps it, else the written one (the menu, a
 *      file) beats a default, else the first registered; the other is
 *      refused with a line;
 *   9. the longest match wins: a pad binding whose members all belong to
 *      a longer binding on its port fires at its release, and not at all
 *      once the longer one was held whole, fired or not (guide, then
 *      guide+a, fires only guide+a);
 *  10. nothing fires during the menu's capture (enhance_actions_quiet),
 *      nor on port 0's next step after it, which the key a capture ended
 *      on may reach first.
 *
 * Bindings name host sources (input_map.h's names; a guest control's name
 * means the source the identity gives it), read from the raw host pad
 * before [input.pad], so a remap never moves a binding (gamepad-input-ux
 * D4). Which members the game uses depends on the map, so a pad binding's
 * kind is derived from it at every rebuild: host-only, a guest chord, a
 * modifier chord (its unmapped members the modifiers), or inactive with a
 * warning. Masking clears the guest controls a consumed source feeds
 * through either map, and centres the half of a guest stick axis a
 * consumed stick direction feeds.
 *
 * Threads: the poll thread runs the filter, the ack thread the flip tick,
 * any thread may rebuild. One poll thread per title is assumed (cat polls
 * under its LOCK(), input_core from the guest's thread): keyboard.c's live
 * table swap relies on it too. One spin lock covers the bindings table and
 * the per-port state, held only for a step and never across backend I/O:
 * the tick reads its host pads between two holds, and only tries the lock,
 * so the flip never waits on input. A rebuild builds its table outside the
 * lock and swaps the pointer under it, so the old table is never freed
 * under a step. Nothing called under the lock may take it again (it is not
 * recursive): enhance_actions_mask_until_release is for a menu or a mod's
 * callback, never from inside the filter.
 */
#include "enhance_actions.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "enhance_cfg.h"
#include "input_names.h"
#include "keyboard.h"
#include "pad_map.h"
#include "mod.h"
#include "mouse.h"
#include "nv2a_backend_common.h"
#ifdef XBOXRECOMP_ENHANCE
#include "nv2a_overlay.h"
#endif
#include "recomp_env.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#define spin_yield() SwitchToThread()
#else
#include <sched.h>
#define spin_yield() sched_yield()
#endif

#define PREFIX        "input.actions."
#define NAME_MAX_     48
#define TEXT_MAX      48
#define PORTS         4
#define HOLD_MAX_MS   2000
#define CHORD_MIN_MS  200
#define DELAY_MAX_MS  100
#define STALE_MS      100      /* the tick reads a port not polled for this long */
#define ANALOG_BIT0   8
/* A guest stick's half (up, down, left, right; the left stick first): a
 * consumed stick direction has no button to clear, so its half of the
 * axis is centred instead. */
#define STICK_BIT0    16
#define GUEST_BITS    0xFFFFFFu
#define SRC_BITS      32       /* input_map.h's sources, bit i = source i */
#define ARM_KEYS      8
#define LOG_PER_SEC   10

/* ---- locks ------------------------------------------------------------- */

static int s_step_lock;     /* the table pointer and the per-port state */
static int s_reg_lock;      /* the registry and a rebuild */

static void spin_lock(int *l)
{
    unsigned spins = 0;

    /* Held for a step, so a spin is short; but the holder may be preempted,
     * and on a busy host (Proton's threads plus the guest's) a waiter that
     * never yields can keep it off the core (input_core's last_lock). */
    while (__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE))
        while (__atomic_load_n(l, __ATOMIC_RELAXED))
            if (++spins >= 64) {
                spins = 0;
                spin_yield();
            }
}

static int spin_try(int *l)
{
    return !__atomic_exchange_n(l, 1, __ATOMIC_ACQUIRE);
}

static void spin_unlock(int *l)
{
    __atomic_store_n(l, 0, __ATOMIC_RELEASE);
}

/* ---- the registry ------------------------------------------------------ */

struct action {
    char name[NAME_MAX_];
    char label[64];
    char defs[ENHANCE_ACTION_BINDS][TEXT_MAX];
    int  ndefs;
    int  owner;             /* mod table index, -1 the toolkit */
    int  live;              /* 0: a dropped mod's slot, never reused (ids stay) */
    int  pressed;           /* atomic: fires not yet taken */
    uint32_t down;          /* atomic: bit per binding held past its hold */
    /* The menu's Controls page: bindings set live, over the files and the
     * defaults until Save writes them (enhance_action_set_bindings). */
    char ovr[ENHANCE_ACTION_BINDS][TEXT_MAX];
    int  novr, has_ovr;
    /* A descriptor action (mod.h, mod-settings D3): its retired keys, its
     * env row (-1 none) that replaces every binding when set, and whether
     * its mod is off. A dormant action keeps its bindings in the table, so
     * they still hold their inputs against other actions, but none of them
     * fires, consumes or reads as down. */
    const char *const *aliases;
    int  env;
    int  dormant;
    char envb[ENHANCE_ACTION_BINDS][TEXT_MAX];   /* the env row, split; s_reg_lock */
    int  src;               /* ENHANCE_ACT_SRC_* of the last config_texts */
    char srckey[sizeof PREFIX + NAME_MAX_];      /* the file key it came from */
};

static struct action s_act[ENHANCE_ACTIONS_MAX];
static int s_nact;
static int s_ntoolkit;      /* ids below it are enhance_actions_init's */
static int s_started;

/* ---- bindings ---------------------------------------------------------- */

/* B_PAD: parsed, kind not derived yet; B_OFF: inactive under this map. */
enum { B_KEY, B_CHORD, B_HOST, B_MOD, B_PAD, B_OFF };

_Static_assert(INPUT_SRC_COUNT <= SRC_BITS, "a source bit each");

struct binding {
    int action, idx;        /* idx: the bit in s_act[action].down */
    int kind;
    unsigned port;
    uint32_t bits;          /* every pad member: bit per source */
    uint32_t guest;         /* the members the map gives the game */
    uint32_t modmask;       /* B_MOD: the members it does not (the modifiers) */
    int hold_ms, has_hold;
    int key;                /* B_KEY */
    int mods[3], nmods;     /* B_KEY: modifier codes, as written */
    char text[TEXT_MAX];
    /* The action's name when the table was built, for the fire log: the
     * registry is s_reg_lock's, and a dropped action's trailing slot may be
     * reused before the rebuild that follows the drop swaps this table out.
     * Until then a fire may count on the reused slot; the next step on the
     * new table no longer does. */
    char name[NAME_MAX_];
    int explicit_;          /* from the menu or a file, not the action's defaults */
    int dormant;            /* its action's mod is off: holds the input, never fires */
    /* A longer binding on the same port holds all of its members: this one
     * fires at its release, and only when no longer one fired meanwhile
     * (one press, one action). */
    int shadowed;
    /* state, under the step lock */
    int active, fired;
    int held_over;          /* held when the table was built: no fire, not down */
    int ready;              /* shadowed: its hold passed, it fires at release */
    int want;               /* fires in this step, unless a longer one does */
    uint64_t since;
    uint32_t modpress;      /* B_MOD: guest members pressed with the modifiers held */
};

struct table {
    int n;
    int delay_ms;
    uint32_t feeds[SRC_BITS];       /* per source, the guest bits it feeds */
    uint32_t chord_members[PORTS];  /* sources of guest chords, for chord_delay_ms */
    unsigned ports;                 /* bit per port a pad binding reads */
    int keys;                       /* any B_KEY */
    struct binding b[];
};

struct port_state {
    uint32_t held, consumed;    /* bit per source */
    uint64_t press_ms[32];
    uint64_t last_poll;
    int polled;
    int slot;               /* the backend slot behind the port, -1 unknown */
};

static struct table *s_table;           /* under s_step_lock */
static struct port_state s_port[PORTS]; /* under s_step_lock */
static int s_delay_ms;

/* ---- arming ------------------------------------------------------------ */

static int s_armed;
/* The keys below change at init and, since mod-settings, from the menu's
 * thread when a descriptor mod's arm setting is switched live, while the
 * menu and the overlay read them: s_arm_lock covers the list. Only that
 * one thread arms or disarms after start, so s_armed's flip and the hook
 * install that follows need no more than its atomics. Never held while
 * taking s_reg_lock. */
static int s_arm_lock;
static char s_arm_keys[ARM_KEYS][NAME_MAX_];
static int s_arm_owner[ARM_KEYS];   /* the mod that armed it, -1 the toolkit */
static int s_narm;

static void disarm_where(int all_mods, int owner);

/* ---- the fire log, at most LOG_PER_SEC lines a second ------------------- */

static uint64_t s_log_sec;
static int s_log_n, s_log_dropped;

/* The toolkit's actions that act at the fire. Fires run under
 * s_step_lock, so the strip's read-then-set never races another fire. */
static int s_stats_act = -1;
static int s_menu_act = -1;

/* The menu (enhance_menu.c): its hook runs after each step, outside the
 * step lock, and a menu.open fire is a request it takes there. */
static enhance_menu_hook_fn s_menu_hook;
static void (*s_panel_hook)(int stop);

/* The flip tick stopped (disarmed): a panel shown must not stay on the
 * screen with nothing left to hide it. */
static void panel_stop(void)
{
    void (*panel)(int) = __atomic_load_n(&s_panel_hook, __ATOMIC_ACQUIRE);
    if (panel)
        panel(1);
}
static int s_menu_req;
/* The menu's capture: what is pressed meanwhile is the capture's, and
 * fires nothing (enhance_actions_quiet). */
static int s_quiet;

static void toolkit_fire(int action)
{
#ifdef XBOXRECOMP_ENHANCE
    if (action == s_stats_act)
        nv2a_overlay_set_stats(!nv2a_overlay_stats());
    /* Without the menu (menu.enabled off, the hook armed by another key) a
     * toast says why nothing opens: on the Deck a grip goes through Steam
     * Input to a key, and a log line alone cannot be checked from the
     * couch. */
    else if (action == s_menu_act) {
        if (__atomic_load_n(&s_menu_hook, __ATOMIC_ACQUIRE))
            __atomic_store_n(&s_menu_req, 1, __ATOMIC_RELEASE);
        else
            nv2a_overlay_toast("Menu is off (menu.enabled)");
    }
#else
    (void)action;
#endif
}

static void fire_log(const struct binding *b, uint64_t now)
{
    uint64_t sec = now / 1000;
    if (sec != s_log_sec) {
        if (s_log_dropped)
            fprintf(stderr, "[ACTION] %d more fires not logged\n", s_log_dropped);
        s_log_sec = sec;
        s_log_n = s_log_dropped = 0;
    }
    if (s_log_n++ >= LOG_PER_SEC) {
        s_log_dropped++;
        return;
    }
    fprintf(stderr, "[ACTION] %s via %s\n", b->name, b->text);
}

/* ---- names ------------------------------------------------------------- */

/* Control index to its bit, in upstream's InputControl order: the first
 * eight are the wButtons bits in bit order, A..right_trigger the analog
 * buttons in the XBOX_BUTTON_* order (Black before White). A stick
 * direction has none. */
static uint32_t control_bit(int ctl)
{
    if (ctl >= INPUT_DPAD_UP && ctl <= INPUT_RIGHT_THUMB)
        return 1u << ctl;
    if (ctl >= INPUT_A && ctl <= INPUT_RIGHT_TRIGGER)
        return 1u << (ANALOG_BIT0 + ctl - INPUT_A);
    return 0;
}

/* A pad name to its source, 0 if none: lowercase as written (so "A" stays
 * the key and "a" is the button), a source name, or a guest control's name
 * for the source the identity gives it (white is left_shoulder whatever
 * [input.pad] says), so a binding means the same buttons under any map. */
static int pad_name_src(const char *s)
{
    xbox_pad_setup id;
    int src, ctl;

    for (const char *p = s; *p; p++)
        if (isupper((unsigned char)*p))
            return 0;
    src = xbox_InputMap_SourceFromName(s);
    if (src > INPUT_SRC_NONE)
        return src;
    ctl = xbox_InputMap_ControlFromName(s);
    if (ctl < 0 || ctl >= INPUT_PAD_CONTROL_COUNT)
        return 0;
    xbox_PadSetupIdentity(&id);
    return id.map.source[ctl];
}

static int modifier_family(int code)
{
    switch (code) {
    case XBOX_VK_SHIFT: case XBOX_VK_LSHIFT: case XBOX_VK_RSHIFT:     return 1;
    case XBOX_VK_CONTROL: case XBOX_VK_LCONTROL: case XBOX_VK_RCONTROL: return 2;
    case XBOX_VK_MENU: case XBOX_VK_LMENU: case XBOX_VK_RMENU:         return 3;
    }
    return 0;
}

static int either_side(int code)
{
    return code == XBOX_VK_SHIFT || code == XBOX_VK_CONTROL || code == XBOX_VK_MENU;
}

/* Whether a key the guest receives is the same key as code: equal, or the
 * same modifier where either side is the either-side code (LShift is held
 * whenever Shift is, and Shift with LShift), or Return and keypad Enter. */
static int same_key(int a, int b)
{
    int fa, fb;
    if (a == b)
        return 1;
    if ((a == XBOX_VK_RETURN && b == XBOX_VK_KP_ENTER)
        || (b == XBOX_VK_RETURN && a == XBOX_VK_KP_ENTER))
        return 1;
    fa = modifier_family(a);
    fb = modifier_family(b);
    return fa && fa == fb && (either_side(a) || either_side(b));
}

/* Rule 2: what in the effective key map has code, or NULL. */
static const char *guest_key_use(int code, char *buf, size_t size)
{
    xbox_mouse_opts mo;
    int walk = xbox_InputKeyboardWalkKey();

    for (int ctl = 0; ctl < INPUT_CONTROL_COUNT; ctl++) {
        int codes[XBOX_BIND_MAX], mice[XBOX_BIND_MAX];
        int n = xbox_InputBindingsOf(ctl, codes, mice, XBOX_BIND_MAX);
        for (int k = 0; k < n; k++)
            if (!mice[k] && same_key(codes[k], code)) {
                snprintf(buf, size, "bound to the guest's %s (input.bindings)",
                         xbox_InputMap_ControlName(ctl));
                return buf;
            }
    }
    if (walk && same_key(walk, code))
        return "the walk key (input.walk_key)";
    xbox_InputMouseGetOpts(&mo);
    if (mo.release_key && same_key(mo.release_key, code))
        return "the mouse release key (input.mouse_release_key)";
    return NULL;
}

/* ---- the parser -------------------------------------------------------- */

/* enhance_action_check: the reason goes to its caller (the menu shows it
 * on the row) instead of the log. Per thread, so a rebuild on another
 * thread still logs. */
#if defined(_MSC_VER)
#define ACTIONS_THREAD_LOCAL __declspec(thread)   /* as host_time.c: older MSVC has no _Thread_local */
#else
#define ACTIONS_THREAD_LOCAL _Thread_local
#endif
static ACTIONS_THREAD_LOCAL char *s_why;
static ACTIONS_THREAD_LOCAL size_t s_why_n;

static void warn(const char *where, const char *text, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_why) {
        vsnprintf(s_why, s_why_n, fmt, ap);
        va_end(ap);
        return;
    }
    fprintf(stderr, "[INPUT] action %s: '%s' refused: ", where, text);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

static void warn_off(const char *where, const char *text, const char *was,
                     const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (s_why) {
        vsnprintf(s_why, s_why_n, fmt, ap);
        va_end(ap);
        return;
    }
    fprintf(stderr, "[INPUT] action %s: '%s' inactive: ", where, text);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    if (was)
        fprintf(stderr, " (was %s)", was);
    fputc('\n', stderr);
}

static void trim(char *s)
{
    size_t n = strlen(s), i = 0;
    while (n && isspace((unsigned char)s[n - 1]))
        s[--n] = 0;
    while (s[i] && isspace((unsigned char)s[i]))
        i++;
    memmove(s, s + i, n - i + 1);
}

/* One binding into *b, or 0 with a warning. */
static int parse_binding(const char *where, const char *text, struct binding *b)
{
    char buf[TEXT_MAX], *tok[8], *s, *at;
    int ntok = 0, hold = 0, has_hold = 0, allpad = 1;
    int src[8];

    memset(b, 0, sizeof *b);
    if (strlen(text) >= sizeof buf) {
        warn(where, text, "longer than %d characters", TEXT_MAX - 1);
        return 0;
    }
    snprintf(b->text, sizeof b->text, "%s", text);
    snprintf(buf, sizeof buf, "%s", text);
    trim(buf);
    s = buf;

    /* padN: */
    if (!strncmp(s, "pad", 3) && s[3] >= '1' && s[3] <= '4' && s[4] == ':') {
        b->port = (unsigned)(s[3] - '1');
        s += 5;
    }
    /* @ms on the whole binding */
    at = strchr(s, '@');
    if (at) {
        char *end;
        long v;
        *at = 0;
        v = strtol(at + 1, &end, 10);
        trim(end);
        if (end == at + 1 || *end || v < 0 || v > HOLD_MAX_MS) {
            warn(where, text, "the hold after '@' must be 0..%d ms", HOLD_MAX_MS);
            return 0;
        }
        hold = (int)v;
        has_hold = 1;
    }
    for (char *p = s;;) {
        char *plus = strchr(p, '+');
        if (ntok == 8) {
            warn(where, text, "more than 8 members");
            return 0;
        }
        if (plus)
            *plus = 0;
        trim(p);
        if (!*p) {
            warn(where, text, "an empty member");
            return 0;
        }
        tok[ntok++] = p;
        if (!plus)
            break;
        p = plus + 1;
    }
    for (int i = 0; i < ntok; i++) {
        src[i] = pad_name_src(tok[i]);
        if (!src[i])
            allpad = 0;
    }
    b->hold_ms = hold;
    b->has_hold = has_hold;

    /* A pad binding: its kind waits for the map (classify, at build). */
    if (allpad) {
        for (int i = 0; i < ntok; i++) {
            if (b->bits & (1u << src[i])) {
                warn(where, text, "'%s' twice", tok[i]);
                return 0;
            }
            b->bits |= 1u << src[i];
        }
        b->kind = B_PAD;
        return 1;
    }

    /* Name every member first: an old pad name in a chord
     * (back+lthumb@400) is an unknown name, not a key misusing '@'. */
    for (int i = 0; i < ntok; i++) {
        int code, is_mouse;
        if (!src[i] && !xbox_InputNameToCode(tok[i], &code, &is_mouse)) {
            warn(where, text, "unknown name '%s' (keys as in docs/input.md,"
                 " pad buttons in lowercase)", tok[i]);
            return 0;
        }
    }

    /* A key, after its modifiers. */
    if (has_hold || b->port) {
        warn(where, text, "'@' and 'padN:' are for pad bindings");
        return 0;
    }
    for (int i = 0; i < ntok; i++) {
        int code, is_mouse;
        char why[96];
        const char *use;
        if (!xbox_InputNameToCode(tok[i], &code, &is_mouse)) {
            warn(where, text, "unknown name '%s' (keys as in docs/input.md,"
                 " pad buttons in lowercase)", tok[i]);
            return 0;
        }
        if (is_mouse) {
            warn(where, text, "'%s' is a mouse input; actions take keys and pad"
                 " buttons", tok[i]);
            return 0;
        }
        if (i < ntok - 1 && !modifier_family(code)) {
            warn(where, text, "'%s' is not a modifier (Ctrl, Shift, Alt)", tok[i]);
            return 0;
        }
        use = guest_key_use(code, why, sizeof why);
        if (use) {
            warn(where, text, "'%s' is %s; the game keeps it", tok[i], use);
            return 0;
        }
        if (i < ntok - 1)
            b->mods[b->nmods++] = code;
        else
            b->key = code;
    }
    b->kind = B_KEY;
    return 1;
}

/* ---- the step ------------------------------------------------------------ */

/* The sources down on the raw host pad. A source counts once its 0..255
 * reading (PadMap's >> 7) passes the Duke's analog threshold: under the
 * identity that is exactly when the guest button it feeds reads down, as
 * the bindings read it before the map existed; a button source is 0 or
 * 32767. */
static uint32_t host_bits(const struct xbox_host_pad *h)
{
    uint32_t b = 0;
    if (!h || !h->connected)
        return 0;
    for (int i = INPUT_SRC_NONE + 1; i < INPUT_SRC_COUNT; i++)
        if ((h->raw.src[i] >> 7) > XBOX_ANALOG_BUTTON_THRESHOLD)
            b |= 1u << i;
    return b;
}

/* The key and exactly its modifiers: Ctrl+F6 is not F6, and F6 with Ctrl
 * held is not F6, so two bindings never both fire. */
static int key_cond(const struct binding *b)
{
    int want[4] = { 0, 0, 0, 0 }, self = modifier_family(b->key);

    if (!xbox_FramebufferKeyDown(b->key))
        return 0;
    for (int i = 0; i < b->nmods; i++) {
        if (!xbox_FramebufferKeyDown(b->mods[i]))
            return 0;
        want[modifier_family(b->mods[i])] = 1;
    }
    if (self != 1 && !want[1] && xbox_FramebufferKeyDown(XBOX_VK_SHIFT))
        return 0;
    if (self != 2 && !want[2] && xbox_FramebufferKeyDown(XBOX_VK_CONTROL))
        return 0;
    if (self != 3 && !want[3] && xbox_FramebufferKeyDown(XBOX_VK_MENU))
        return 0;
    return 1;
}

/* Whether pad binding a is a strict part of pad binding c: the same port,
 * every member of a in c, and c longer. */
static int pad_inside(const struct binding *a, const struct binding *c)
{
    return a->kind != B_KEY && c->kind != B_KEY && a->kind != B_OFF && c->kind != B_OFF
        && a->port == c->port && (a->bits & c->bits) == a->bits && a->bits != c->bits;
}

static void fire(const struct binding *b, uint64_t now)
{
    __atomic_add_fetch(&s_act[b->action].pressed, 1, __ATOMIC_ACQ_REL);
    fire_log(b, now);
    toolkit_fire(b->action);
}

/* One step of port's bindings with the host pad's bits. tick: the flip's
 * step, which leaves guest chords alone (only a poll can consume them).
 *
 * Two passes, so the order of the table never decides which of two
 * bindings fires: the first finds what would fire, the second fires the
 * longest of them. A binding inside a longer one (guide inside guide+a)
 * waits for its release, and a longer one firing first takes the press
 * from it. */
static void step(struct table *t, unsigned port, uint32_t held, int tick, uint64_t now)
{
    struct port_state *ps = &s_port[port];
    uint32_t fresh = held & ~ps->held;
    int quiet = __atomic_load_n(&s_quiet, __ATOMIC_ACQUIRE), wants = 0;

    /* Port 0's step after a capture is still quiet: the menu reads its
     * keys after this step, so a key that went down in between ended the
     * capture unseen here and must not fire now. Keys step on port 0 only;
     * the pad hands the menu the same held this step saw, so the other
     * ports need no drain and must not wait for port 0's step (a title
     * that polls no port 0 on a loading screen). */
    if (quiet == 2 && port == 0)
        __atomic_compare_exchange_n(&s_quiet, &quiet, 0, 0, __ATOMIC_ACQ_REL,
                                    __ATOMIC_ACQUIRE);
    else if (quiet == 2)
        quiet = 0;
    ps->held = held;
    ps->consumed &= held;
    for (int i = 0; i < 32; i++)
        if (fresh & (1u << i))
            ps->press_ms[i] = now;
    if (!t)
        return;
    for (int i = 0; i < t->n; i++) {
        struct binding *b = &t->b[i];
        int cond = 0;

        b->want = 0;
        if (b->kind == B_OFF || b->dormant || (b->kind == B_KEY ? port != 0 : b->port != port))
            continue;
        if (tick && b->kind == B_CHORD && b->guest)
            continue;
        switch (b->kind) {
        case B_KEY:
            cond = key_cond(b);
            break;
        case B_HOST:
        case B_CHORD:
            cond = (held & b->bits) == b->bits;
            if (cond)
                ps->consumed |= b->guest;
            break;
        case B_MOD: {
            int mods = (held & b->modmask) == b->modmask;
            b->modpress &= held;
            if (mods) {
                uint32_t m = fresh & b->guest;
                b->modpress |= m;
                ps->consumed |= m;
            }
            cond = mods && (b->modpress & b->guest) == b->guest;
            break;
        }
        }
        if (!cond) {
            /* A shorter binding's tap: let go after its hold, nothing
             * longer took it. */
            if (b->shadowed && b->active && b->ready && !b->fired && !quiet)
                b->want = 1;
            b->active = b->fired = b->held_over = b->ready = 0;
        } else if (!b->active) {
            b->active = 1;
            b->fired = 0;
            b->since = now;
        }
        /* Held during a capture: the capture's, and it stays spent until
         * released, as a binding held when its table was built. */
        if (quiet && b->active)
            b->fired = b->held_over = 1;
        if (b->active && !b->fired && now - b->since >= (uint64_t)b->hold_ms) {
            if (b->shadowed)
                b->ready = 1;
            else
                b->want = 1;
        }
        wants |= b->want;
    }
    /* A longer binding held whole takes the press from those inside it,
     * whether or not it fires: let go before its hold, it fires nothing,
     * and neither does the shorter one (one press, one action). */
    for (int i = 0; i < t->n; i++) {
        const struct binding *c = &t->b[i];
        if (!c->active || c->kind == B_KEY || c->kind == B_OFF || c->port != port)
            continue;
        for (int k = 0; k < t->n; k++)
            if (t->b[k].active && pad_inside(&t->b[k], c))
                t->b[k].fired = 1;
    }
    for (int i = 0; wants && i < t->n; i++) {
        struct binding *b = &t->b[i];
        int longer = 0;

        if (!b->want)
            continue;
        for (int k = 0; k < t->n && !longer; k++)
            longer = t->b[k].want && pad_inside(b, &t->b[k]);
        if (b->active)
            b->fired = 1;
        if (longer)
            continue;
        fire(b, now);
        /* The shorter bindings held with it are spent until released. */
        for (int k = 0; k < t->n; k++)
            if (t->b[k].active && pad_inside(&t->b[k], b))
                t->b[k].fired = 1;
    }
    for (int i = 0; i < t->n; i++) {
        struct binding *b = &t->b[i];
        if (b->kind == B_OFF || b->dormant || (b->kind == B_KEY ? port != 0 : b->port != port))
            continue;
        /* A shorter binding is a tap: it never reads as down. */
        if (b->active && b->fired && !b->held_over && !b->shadowed)
            __atomic_or_fetch(&s_act[b->action].down, 1u << b->idx, __ATOMIC_ACQ_REL);
        else
            __atomic_and_fetch(&s_act[b->action].down, ~(1u << b->idx), __ATOMIC_ACQ_REL);
    }
}

/* What the guest does not see on port: the guest controls fed by the
 * consumed sources and by chord members inside chord_delay_ms of their
 * press. */
static uint32_t guest_mask(const struct table *t, unsigned port, uint64_t now)
{
    const struct port_state *ps = &s_port[port];
    uint32_t src = ps->consumed, mask = 0;

    if (!t)
        return 0;
    if (t->delay_ms) {
        uint32_t wait = ps->held & t->chord_members[port] & ~src;
        for (int i = 0; i < SRC_BITS; i++)
            if ((wait & (1u << i)) && now - ps->press_ms[i] < (uint64_t)t->delay_ms)
                src |= 1u << i;
    }
    for (int i = 0; i < SRC_BITS; i++)
        if (src & (1u << i))
            mask |= t->feeds[i];
    return mask & GUEST_BITS;
}

/* Centre *v when it points the way a masked half does: the other half
 * still reaches the game, so a consumed "up" never hides a push down. */
static void mask_axis(int16_t *v, uint32_t mask, int pos_bit, int neg_bit)
{
    if (((mask >> pos_bit) & 1u) && *v > 0)
        *v = 0;
    if (((mask >> neg_bit) & 1u) && *v < 0)
        *v = 0;
}

static void apply_mask(XBOX_INPUT_STATE *st, uint32_t mask)
{
    XBOX_GAMEPAD *g = &st->Gamepad;

    g->wButtons &= (WORD)~(mask & 0xFFu);
    for (int i = 0; i < 8; i++)
        if (mask & (1u << (ANALOG_BIT0 + i)))
            g->bAnalogButtons[i] = 0;
    /* Up is Y positive, as in InputRaw. */
    mask_axis(&g->sThumbLY, mask, STICK_BIT0 + 0, STICK_BIT0 + 1);
    mask_axis(&g->sThumbLX, mask, STICK_BIT0 + 3, STICK_BIT0 + 2);
    mask_axis(&g->sThumbRY, mask, STICK_BIT0 + 4, STICK_BIT0 + 5);
    mask_axis(&g->sThumbRX, mask, STICK_BIT0 + 7, STICK_BIT0 + 6);
}

void enhance_actions_filter(unsigned port, const struct xbox_host_pad *host,
                            XBOX_INPUT_STATE *st)
{
    uint64_t now;
    uint32_t mask, held;
    enhance_menu_hook_fn hook;

    if (port >= PORTS || !__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    now = xbox_InputNowMs();
    spin_lock(&s_step_lock);
    s_port[port].last_poll = now;
    s_port[port].polled = 1;
    /* -1 too: a port whose pad went away has no slot for the tick to read. */
    s_port[port].slot = host ? host->slot : -1;
    held = host_bits(host);
    step(s_table, port, held, 0, now);
    mask = guest_mask(s_table, port, now);
    spin_unlock(&s_step_lock);
    if (st)
        apply_mask(st, mask);
    hook = __atomic_load_n(&s_menu_hook, __ATOMIC_ACQUIRE);
    if (hook)
        hook(port, held, st, now);
}

/* A port the tick steps: one its table reads that the title has not
 * polled for STALE_MS. Under the step lock. */
static int tick_wants(const struct table *t, unsigned port, uint64_t now)
{
    const struct port_state *ps = &s_port[port];

    if (!(t->ports & (1u << port)) && !(port == 0 && t->keys))
        return 0;
    return !(ps->polled && now - ps->last_poll < STALE_MS);
}

/* The slot the tick reads for port, -1 none. A port never polled is its
 * own slot (input_core's ports are). Once polled, the title's slot, and
 * none is not read: in a title that maps pads itself (cat), port n may be
 * another pad's slot. Under the step lock. */
static int tick_slot(unsigned port)
{
    return s_port[port].polled ? s_port[port].slot : (int)port;
}

void enhance_actions_tick(void)
{
    uint64_t now;
    struct table *t;
    unsigned want = 0, read = 0;
    int slot[PORTS], polled[PORTS];
    uint64_t last[PORTS];
    uint32_t got[PORTS], held0 = 0;
    DWORD rc[PORTS];
    int stale0 = 0;
    enhance_menu_hook_fn hook;

    /* A debug key's live mod switches, from the thread the menu steps on. */
    xbox_mod_debug_tick();
    /* The mods' status panels, on the overlay's thread. This tick only
     * runs while the hook is armed (a feature key on, rule 6), so the
     * panels show only then; the disarm below stops them. */
    {
        void (*panel)(int) = __atomic_load_n(&s_panel_hook, __ATOMIC_ACQUIRE);
        if (panel)
            panel(0);
    }
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    /* Which ports to read, and their slots. The clock is read under the
     * lock: a poll's last_poll from after it would wrap now - last_poll. */
    if (!spin_try(&s_step_lock))
        return;     /* a poll is stepping now */
    now = xbox_InputNowMs();
    t = s_table;
    for (unsigned port = 0; t && port < PORTS; port++) {
        if (!tick_wants(t, port, now))
            continue;
        want |= 1u << port;
        slot[port] = tick_slot(port);
        last[port] = s_port[port].last_poll;
        polled[port] = s_port[port].polled;
        if ((t->ports & (1u << port)) && slot[port] >= 0)
            read |= 1u << port;
    }
    /* The menu steps from the flip when the title has stopped polling (a
     * loading screen): port 0, its keys and the pad the tick read. */
    stale0 = !(s_port[0].polled && now - s_port[0].last_poll < STALE_MS);
    held0 = s_port[0].held;
    spin_unlock(&s_step_lock);
    hook = __atomic_load_n(&s_menu_hook, __ATOMIC_ACQUIRE);
    if (!want) {
        if (hook && stale0)
            hook(0, held0, NULL, now);
        return;
    }
    /* The backend outside the lock: XInput under Wine can take a while. */
    for (unsigned port = 0; port < PORTS; port++) {
        struct xbox_host_pad h;
        if (!(read & (1u << port)))
            continue;
        rc[port] = xbox_input_backend_read_host((DWORD)slot[port], &h);
        got[port] = rc[port] == ERROR_SUCCESS ? host_bits(&h) : 0;
    }
    if (!spin_try(&s_step_lock))
        return;
    now = xbox_InputNowMs();
    t = s_table;
    for (unsigned port = 0; t && port < PORTS; port++) {
        struct port_state *ps = &s_port[port];
        uint32_t held = ps->held;

        /* A poll during the read has stepped the port with a newer state:
         * stepping it again with ours would refire a press or start a hold
         * early. The slot too: a poll in the same millisecond keeps
         * last_poll. Or the table changed. */
        if (!(want & (1u << port)) || ps->last_poll != last[port] ||
            ps->polled != polled[port] || tick_slot(port) != slot[port] ||
            !tick_wants(t, port, now))
            continue;
        if (t->ports & (1u << port)) {
            if (slot[port] < 0)
                held = 0;   /* no pad behind it now */
            else if (!(read & (1u << port)))
                continue;   /* the new table reads a port we did not */
            else if (rc[port] == ERROR_SUCCESS)
                held = got[port];
            else if (rc[port] != XBOX_INPUT_BUSY)
                held = 0;   /* no pad: a modifier held as it went away is let go */
        }
        step(t, port, held, 1, now);
        if (port == 0)
            held0 = held;
    }
    spin_unlock(&s_step_lock);
    if (hook && stale0)
        hook(0, held0, NULL, now);
}

void enhance_actions_mask_until_release(unsigned port, uint32_t bits)
{
    if (port >= PORTS)
        return;
    spin_lock(&s_step_lock);
    /* The held sources that feed them: the map may give a control two. */
    for (int i = 0; s_table && i < SRC_BITS; i++)
        if (s_table->feeds[i] & bits & GUEST_BITS)
            s_port[port].consumed |= (1u << i) & s_port[port].held;
    spin_unlock(&s_step_lock);
}

/* ---- the registry's calls --------------------------------------------- */

static int good_name(const char *s)
{
    int dot = 0;
    if (!s || !*s || strlen(s) >= NAME_MAX_ || *s == '.')
        return 0;
    for (; *s; s++) {
        if (*s == '.')
            dot = 1;
        else if (!(islower((unsigned char)*s) || isdigit((unsigned char)*s) || *s == '_'))
            return 0;
    }
    return dot && s[-1] != '.';
}

/* 1 if a is b or a dotted prefix of it ("menu.open" of "menu.open.all"). */
static int name_prefix(const char *a, const char *b)
{
    size_t n = strlen(a);
    return !strncmp(a, b, n) && (b[n] == 0 || b[n] == '.');
}

/* A live action that x.y beside x.y.z would clash with: written bare in
 * TOML, one is a value and the other a table under it. */
static int clashing(const char *name)
{
    for (int i = 0; i < s_nact; i++)
        if (s_act[i].live && (name_prefix(s_act[i].name, name) ||
                              name_prefix(name, s_act[i].name)))
            return i;
    return -1;
}

int enhance_action_find(const char *name)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    if (!name)
        return -1;
    for (int i = 0; i < n; i++)
        if (s_act[i].live && !strcmp(s_act[i].name, name))
            return i;
    return -1;
}

const char *enhance_action_name(int id)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    return id >= 0 && id < n && s_act[id].live ? s_act[id].name : NULL;
}

int enhance_action_register_owned(const char *name, const char *label,
                                  const char *const *defaults, int owner)
{
    return enhance_action_register_full(name, label, defaults, owner, NULL, -1);
}

int enhance_action_register_full(const char *name, const char *label,
                                 const char *const *defaults, int owner,
                                 const char *const *aliases, int env)
{
    struct action *a;
    int id, started, c;

    spin_lock(&s_reg_lock);
    if (!good_name(name)) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: names are <owner>.<verb>"
                " in lowercase\n", name ? name : "(null)");
        return -1;
    }
    if (enhance_action_find(name) >= 0) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: already registered\n", name);
        return -1;
    }
    if ((c = clashing(name)) >= 0) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: it and '%s' cannot both be"
                " TOML keys (one would be a table of the other)\n", name, s_act[c].name);
        return -1;
    }
    if (s_nact >= ENHANCE_ACTIONS_MAX) {
        spin_unlock(&s_reg_lock);
        fprintf(stderr, "[INPUT] action '%s' refused: %d actions already\n", name,
                ENHANCE_ACTIONS_MAX);
        return -1;
    }
    id = s_nact;
    a = &s_act[id];
    memset(a, 0, sizeof *a);
    snprintf(a->name, sizeof a->name, "%s", name);
    snprintf(a->label, sizeof a->label, "%s", label ? label : name);
    for (int i = 0; defaults && defaults[i] && a->ndefs < ENHANCE_ACTION_BINDS; i++)
        snprintf(a->defs[a->ndefs++], TEXT_MAX, "%s", defaults[i]);
    a->owner = owner;
    a->aliases = aliases;
    a->env = env;
    a->live = 1;
    __atomic_store_n(&s_nact, id + 1, __ATOMIC_RELEASE);
    started = s_started;
    spin_unlock(&s_reg_lock);
    if (started)
        enhance_actions_rebuild();
    return id;
}

int enhance_action_register(const char *name, const char *label,
                            const char *const *defaults)
{
    return enhance_action_register_owned(name, label, defaults, -1);
}

static void drop_where(int all_mods, int owner)
{
    int started;
    spin_lock(&s_reg_lock);
    for (int i = 0; i < s_nact; i++)
        if (s_act[i].owner >= 0 && (all_mods || s_act[i].owner == owner))
            s_act[i].live = 0;
    /* Mods start over from slot 0 of theirs: trailing dead slots go. */
    while (s_nact && !s_act[s_nact - 1].live)
        s_nact--;
    started = s_started;
    spin_unlock(&s_reg_lock);
    disarm_where(all_mods, owner);
    if (started)
        enhance_actions_rebuild();
}

void enhance_actions_drop_owner(int owner)
{
    drop_where(0, owner);
}

void enhance_actions_drop_mods(void)
{
    drop_where(1, 0);
}

int enhance_action_pressed(int id)
{
    if (id < 0 || id >= ENHANCE_ACTIONS_MAX)
        return 0;
    return __atomic_exchange_n(&s_act[id].pressed, 0, __ATOMIC_ACQ_REL);
}

int enhance_action_down(int id)
{
    if (id < 0 || id >= ENHANCE_ACTIONS_MAX)
        return 0;
    return __atomic_load_n(&s_act[id].down, __ATOMIC_ACQUIRE) != 0;
}

/* ---- building the table ------------------------------------------------ */

/* One file row's bindings: 1 with the texts, 0 when the value is not a
 * string or a list of strings (a line; the next row answers). */
static int file_texts(const recomp_cfg *c, const char *key, const char *where,
                      const char *texts[ENHANCE_ACTION_BINDS], int *n)
{
    recomp_cfg_type t = recomp_cfg_type_of(c, key);
    *n = 0;
    if (t == RCFG_STRING) {
        texts[(*n)++] = recomp_cfg_string(c, key, "");
        return 1;
    }
    if (t == RCFG_ARRAY) {
        size_t len = recomp_cfg_array_len(c, key);
        for (size_t i = 0; i < len; i++) {
            if (recomp_cfg_array_type(c, key, i) != RCFG_STRING) {
                fprintf(stderr, "[INPUT] action %s: element %zu is not a"
                        " string; skipped\n", where, i + 1);
                continue;
            }
            if (*n == ENHANCE_ACTION_BINDS) {
                fprintf(stderr, "[INPUT] action %s: more than %d bindings;"
                        " the rest skipped\n", where, ENHANCE_ACTION_BINDS);
                break;
            }
            texts[(*n)++] = recomp_cfg_array_string(c, key, i, "");
        }
        return 1;
    }
    fprintf(stderr, "[INPUT] action %s: should be a list of strings; skipped\n", where);
    return 0;
}

/* The bindings of one action, highest first (mod-settings D4): its env
 * row, the menu's list, the user files' key then its aliases, the defaults
 * file's key then its aliases, its defaults. A file row names all of them
 * (an empty list unbinds). Returns how many texts; *explicit_ (when not
 * NULL) is 0 for the defaults. Under s_reg_lock: the env split lands in
 * the action. */
static int config_texts(struct action *a, const char *texts[ENHANCE_ACTION_BINDS],
                        char where[96], int *explicit_)
{
    char key[sizeof PREFIX + NAME_MAX_];
    char alias[ENHANCE_ACTION_BINDS][sizeof PREFIX + NAME_MAX_];
    const char *aliases[ENHANCE_ACTION_BINDS + 1];
    struct enhance_cfg_hit hits[2 * (ENHANCE_ACTION_BINDS + 1) + 2];
    const char *env = a->env >= 0 ? recomp_env((recomp_env_id)a->env) : NULL;
    int n = 0, na = 0, nh, dummy;

    if (!explicit_)
        explicit_ = &dummy;
    *explicit_ = 1;
    if (env && *env) {
        const char *p = env;
        snprintf(where, 96, "%s (%s)", a->name, recomp_env_name((recomp_env_id)a->env));
        a->src = ENHANCE_ACT_SRC_ENV;
        while (*p && n < ENHANCE_ACTION_BINDS) {
            const char *c = strchr(p, ',');
            size_t len = c ? (size_t)(c - p) : strlen(p);
            if (len >= TEXT_MAX)
                len = TEXT_MAX - 1;
            memcpy(a->envb[n], p, len);
            a->envb[n][len] = 0;
            trim(a->envb[n]);
            if (a->envb[n][0]) {
                texts[n] = a->envb[n];
                n++;
            }
            if (!c)
                break;
            p = c + 1;
        }
        return n;
    }
    if (a->has_ovr) {
        snprintf(where, 96, "%s (menu)", a->name);
        a->src = ENHANCE_ACT_SRC_MENU;
        for (int i = 0; i < a->novr; i++)
            texts[n++] = a->ovr[i];
        return n;
    }
    snprintf(key, sizeof key, PREFIX "%s", a->name);
    for (int i = 0; a->aliases && a->aliases[i] && na < ENHANCE_ACTION_BINDS; i++) {
        /* An alias is a full key ("input.actions.camera.toggle"). */
        snprintf(alias[na], sizeof alias[na], "%s", a->aliases[i]);
        aliases[na] = alias[na];
        na++;
    }
    aliases[na] = NULL;
    nh = enhance_cfg_layered(key, aliases, hits, (int)(sizeof hits / sizeof hits[0]));
    for (int h = 0; h < nh; h++) {
        snprintf(where, 96, "%s (%s:%d)", hits[h].key, recomp_cfg_name(hits[h].file),
                 recomp_cfg_line(hits[h].file, hits[h].key));
        if (file_texts(hits[h].file, hits[h].key, where, texts, &n)) {
            snprintf(a->srckey, sizeof a->srckey, "%s", hits[h].key);
            a->src = hits[h].row == ENHANCE_ROW_USER ? ENHANCE_ACT_SRC_USER
                   : hits[h].row == ENHANCE_ROW_USER_ALIAS ? ENHANCE_ACT_SRC_USER_ALIAS
                   : hits[h].row == ENHANCE_ROW_DEFAULTS ? ENHANCE_ACT_SRC_DEFAULTS
                   : ENHANCE_ACT_SRC_DEFAULTS_ALIAS;
            return n;
        }
    }
    snprintf(where, 96, "%s (default)", a->name);
    *explicit_ = 0;
    a->src = ENHANCE_ACT_SRC_DEFAULT;
    n = 0;
    for (int i = 0; i < a->ndefs; i++)
        texts[n++] = a->defs[i];
    return n;
}

/* What the map gives the game: each source's guest bits (feeds) and the
 * sources the game reads at all. A physical stick's directions feed the
 * halves of the guest stick a map selects it for (raw is inverted before
 * the map, so the directions match the guest's). */
static uint32_t map_feeds(const xbox_pad_setup *ps, uint32_t feeds[SRC_BITS])
{
    const InputPadMap *m[2] = { &ps->map, &ps->alt };
    uint32_t used = 0;

    memset(feeds, 0, SRC_BITS * sizeof feeds[0]);
    for (int k = 0; k < 2; k++) {
        for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++) {
            int src = m[k]->source[c];
            if (src > INPUT_SRC_NONE && src < INPUT_SRC_COUNT) {
                feeds[src] |= control_bit(c);
                used |= 1u << src;
            }
        }
        for (int st = 0; st < 2; st++) {
            int which = st ? m[k]->right_stick : m[k]->left_stick;
            int first = which == INPUT_STICK_LEFT ? INPUT_SRC_LSTICK_UP
                      : which == INPUT_STICK_RIGHT ? INPUT_SRC_RSTICK_UP : 0;
            for (int d = 0; first && d < 4; d++) {
                feeds[first + d] |= 1u << (STICK_BIT0 + st * 4 + d);
                used |= 1u << (first + d);
            }
        }
    }
    return used;
}

/* "guide (the game's start), south (a)": who took srcs from the host. */
static void took(const xbox_pad_setup *ps, uint32_t srcs, char *buf, size_t size)
{
    size_t n = 0;

    buf[0] = 0;
    for (int i = 0; i < SRC_BITS && n < size; i++) {
        const char *ctl = "a stick";
        if (!(srcs & (1u << i)))
            continue;
        for (int c = 0; c < INPUT_PAD_CONTROL_COUNT; c++)
            if (ps->map.source[c] == i || ps->alt.source[c] == i) {
                ctl = xbox_InputMap_ControlName(c);
                break;
            }
        n += (size_t)snprintf(buf + n, size - n, n ? ", %s (%s)" : "%s (the game's %s)",
                              xbox_InputMap_SourceName(i), ctl);
    }
}

static const char *kind_name(const struct binding *b)
{
    switch (b->kind) {
    case B_HOST:  return "host-only";
    case B_CHORD: return b->guest ? "a guest chord" : "a host-only chord";
    case B_MOD:   return "a modifier chord";
    case B_OFF:   return "inactive";
    }
    return "a key";
}

/* The kind of a pad binding under the current map (gamepad-input-ux D4),
 * with one line when it is inactive or its kind differs from the binding
 * of the same action and text in old. */
static void classify(struct binding *b, const char *where, const xbox_pad_setup *ps,
                     uint32_t used, const struct table *old)
{
    const char *was = NULL;
    uint32_t mapped = b->bits & used, free_ = b->bits & ~used;
    char who[160];
    int single = !(b->bits & (b->bits - 1));

    b->guest = mapped;
    if (single)
        b->kind = mapped ? B_OFF : B_HOST;
    else if (!mapped)
        b->kind = B_CHORD;          /* nothing reaches the game */
    else if (!free_)
        b->kind = b->has_hold && b->hold_ms >= CHORD_MIN_MS ? B_CHORD : B_OFF;
    else {
        b->kind = B_MOD;
        b->modmask = free_;
    }
    for (int i = 0; old && i < old->n; i++)
        if (!strcmp(old->b[i].name, b->name) && !strcmp(old->b[i].text, b->text)) {
            if (strcmp(kind_name(&old->b[i]), kind_name(b)))
                was = kind_name(&old->b[i]);
            break;
        }
    if (b->kind == B_OFF) {
        took(ps, mapped, who, sizeof who);
        if (single)
            warn_off(where, b->text, was, "%s goes to the game; a single source the game"
                     " uses would take it from the game; use a chord of two with a hold"
                     " (back+left_thumb@400) or a source the game does not use (guide,"
                     " paddle1)", who);
        else
            warn_off(where, b->text, was, "every member goes to the game (%s); such a"
                     " chord needs a hold of at least %d ms (@400)", who, CHORD_MIN_MS);
    } else if (was) {
        fprintf(stderr, "[INPUT] action %s: '%s' is now %s (was %s)\n", where, b->text,
                kind_name(b), was);
    }
}

/* Whether each of a's modifiers is one of b's, as key_cond reads them:
 * an either-side modifier matches either side, LCtrl never RCtrl. */
static int mods_within(const struct binding *a, const struct binding *b)
{
    for (int i = 0; i < a->nmods; i++) {
        int found = 0;
        for (int k = 0; k < b->nmods && !found; k++)
            found = same_key(a->mods[i], b->mods[k]);
        if (!found)
            return 0;
    }
    return 1;
}

/* Whether two bindings take the same input: the same key with the same
 * modifiers, or the same pad members on the same port whatever their
 * holds. Either way both would fire on one press. */
static int same_input(const struct binding *a, const struct binding *b)
{
    if (a->kind == B_OFF || b->kind == B_OFF)
        return 0;
    if (a->kind == B_KEY || b->kind == B_KEY)
        return a->kind == b->kind && same_key(a->key, b->key) && mods_within(a, b)
            && mods_within(b, a);
    return a->port == b->port && a->bits == b->bits;
}

/* A binding's claim on its input. menu.open's never gives way: the menu is
 * the only way in the game to repair a binding, and on the Deck under
 * Proton F10 (a grip through Steam Input) is the one way to reach it, so a
 * written binding that took F10 would lock the player out. */
static int claim(const struct binding *b)
{
    return b->action == s_menu_act ? 2 : b->explicit_ ? 1 : 0;
}

/* One input, one action: of two bindings that take the same input, the
 * stronger claim keeps it (menu.open's, then one written in the menu or a
 * file over an action's default), else the first registered (the
 * toolkit's before the mods'). The loser goes with a line; the menu's
 * capture refuses such a binding up front (enhance_action_owner). A
 * binding is judged against every kept one before it takes any input, so
 * no line says a binding won that is then refused. */
static int dedup(struct table *t)
{
    int n = 0;

    for (int i = 0; i < t->n; i++) {
        struct binding *b = &t->b[i];
        const struct binding *o = NULL;
        for (int k = 0; k < n && !o; k++)
            if (same_input(&t->b[k], b) && claim(&t->b[k]) >= claim(b))
                o = &t->b[k];
        if (o) {
            fprintf(stderr, "[INPUT] action %s: '%s' refused: %s has it ('%s');"
                    " one input fires one action\n", b->name, b->text, o->name, o->text);
            /* Two defaults on one input with a mod off: the first
             * registered keeps it whether or not the mod is on, so
             * switching it live moves nothing (mod-settings D3). */
            if (!o->explicit_ && !b->explicit_ && (o->dormant || b->dormant))
                fprintf(stderr, "[MOD] %s: %s keeps it over %s (dormant)\n", b->text,
                        o->name, b->name);
            continue;
        }
        for (int k = 0; k < n; k++) {
            struct binding *w = &t->b[k];
            if (!same_input(w, b))
                continue;
            fprintf(stderr, "[INPUT] action %s%s: '%s' gives way to %s ('%s')\n", w->name,
                    w->explicit_ ? "" : " (default)", w->text, b->name, b->text);
            memmove(w, w + 1, (size_t)(n - k - 1) * sizeof *w);
            n--;
            k--;
        }
        if (n != i)
            t->b[n] = *b;
        n++;
    }
    return n;
}

/* Under s_reg_lock; old is the table in use, for the kind changes. */
static struct table *build(const struct table *old)
{
    struct table *t;
    xbox_pad_setup ps;
    uint32_t used;
    int cap = 1;

    for (int i = 0; i < s_nact; i++)
        cap += ENHANCE_ACTION_BINDS;
    t = calloc(1, sizeof *t + (size_t)cap * sizeof t->b[0]);
    if (!t)
        return NULL;
    t->delay_ms = s_delay_ms;
    xbox_InputPadSetup(&ps);
    used = map_feeds(&ps, t->feeds);
    for (int i = 0; i < s_nact; i++) {
        const char *texts[ENHANCE_ACTION_BINDS];
        char where[96];
        int n, expl;
        if (!s_act[i].live)
            continue;
        n = config_texts(&s_act[i], texts, where, &expl);
        for (int k = 0; k < n; k++) {
            struct binding *b = &t->b[t->n];
            if (!texts[k] || !*texts[k])
                continue;
            if (!parse_binding(where, texts[k], b))
                continue;
            b->action = i;
            b->explicit_ = expl;
            b->dormant = s_act[i].dormant;
            snprintf(b->name, sizeof b->name, "%s", s_act[i].name);
            if (b->kind == B_PAD)
                classify(b, where, &ps, used, old);
            t->n++;
        }
    }
    t->n = dedup(t);
    for (int i = 0, idx = 0; i < t->n; i++) {
        struct binding *b = &t->b[i];
        if (i && b->action != t->b[i - 1].action)
            idx = 0;
        b->idx = idx++;
        /* A dormant binding keeps its input from the others (dedup above)
         * but takes no part in a step: it shadows nothing, reads no port. */
        if (b->dormant)
            continue;
        for (int k = 0; k < t->n && !b->shadowed; k++)
            b->shadowed = !t->b[k].dormant && pad_inside(b, &t->b[k]);
        /* A held input must be let go before a new binding fires on it,
         * and does not read as down until then. */
        b->active = b->fired = b->held_over = 1;
        if (b->kind == B_KEY) {
            t->keys = 1;
        } else if (b->kind != B_OFF) {
            t->ports |= 1u << b->port;
            if (b->kind == B_CHORD)
                t->chord_members[b->port] |= b->guest;
        }
    }
    return t;
}

/* Keys under [input.actions] that name no action: a typo is visible. */
static void report_unknown(void)
{
    for (int f = 0; f < 3; f++) {
        const recomp_cfg *c = enhance_cfg_file(f);
        const char *k;
        for (size_t i = 0; c && (k = recomp_cfg_key_at(c, i)); i++) {
            if (strncmp(k, PREFIX, sizeof PREFIX - 1))
                continue;
            if (enhance_action_find(k + sizeof PREFIX - 1) < 0)
                fprintf(stderr, "[INPUT] config warning: %s:%d: no action named"
                        " '%s'\n", recomp_cfg_name(c), recomp_cfg_line(c, k),
                        k + sizeof PREFIX - 1);
            enhance_cfg_lookup(k);   /* reported here, not as unused */
        }
    }
}

static void read_delay(void)
{
    long long v;
    enhance_cfg_bind_env("input.chord_delay_ms", RENV_INPUT_CHORD_DELAY_MS);
    v = enhance_cfg_int("input.chord_delay_ms", 0);
    if (v < 0 || v > DELAY_MAX_MS) {
        fprintf(stderr, "[INPUT] config warning: input.chord_delay_ms=%lld out of"
                " 0..%d; using %d\n", v, DELAY_MAX_MS, v < 0 ? 0 : DELAY_MAX_MS);
        v = v < 0 ? 0 : DELAY_MAX_MS;
    }
    s_delay_ms = (int)v;
}

void enhance_actions_rebuild(void)
{
    struct table *t, *old;

    /* The registry lock spans the swap: build reads the table in use for
     * the kind changes, and another rebuild must not free it meanwhile. */
    spin_lock(&s_reg_lock);
    read_delay();
    t = build(s_table);
    if (!t) {
        spin_unlock(&s_reg_lock);
        return;
    }
    spin_lock(&s_step_lock);
    old = s_table;
    s_table = t;
    /* The down bits index the old table's bindings; the new one sets its
     * own from the next step. */
    for (int i = 0; i < ENHANCE_ACTIONS_MAX; i++)
        __atomic_store_n(&s_act[i].down, 0u, __ATOMIC_RELEASE);
    spin_unlock(&s_step_lock);
    spin_unlock(&s_reg_lock);
    free(old);
}

/* ---- arming -------------------------------------------------------------- */

static void hook_line(void)
{
    if (!s_armed) {
        fprintf(stderr, "[ENHANCE] actions hook=off\n");
        return;
    }
    fprintf(stderr, "[ENHANCE] actions hook=on (");
    spin_lock(&s_arm_lock);
    for (int i = 0; i < s_narm; i++)
        fprintf(stderr, i ? ", %s" : "%s", s_arm_keys[i]);
    spin_unlock(&s_arm_lock);
    fprintf(stderr, ")\n");
}

static int arm_key_on(const char *key, int owner, int on)
{
    if (!key || !on)
        return 0;
    spin_lock(&s_arm_lock);
    for (int i = 0; i < s_narm; i++)
        if (!strcmp(s_arm_keys[i], key)) {
            if (owner < 0)
                s_arm_owner[i] = -1;    /* the toolkit's key stays */
            spin_unlock(&s_arm_lock);
            return 1;
        }
    if (s_narm < ARM_KEYS) {
        s_arm_owner[s_narm] = owner;
        snprintf(s_arm_keys[s_narm++], NAME_MAX_, "%s", key);
    }
    spin_unlock(&s_arm_lock);
    return 1;
}

static int arm_key(const char *key, int owner)
{
    return key && arm_key_on(key, owner, enhance_cfg_bool(key, 0));
}

/* The title's pad glue (mod.h) and input_core's seam. */
void xbox_enhance_pad_filter(DWORD port, const struct xbox_host_pad *host,
                             XBOX_INPUT_STATE *st)
{
    if (!__atomic_load_n(&s_armed, __ATOMIC_ACQUIRE))
        return;
    enhance_actions_filter((unsigned)port, host, st);
}

static void install(void)
{
    xbox_InputSetPadFilter(xbox_enhance_pad_filter);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_flip_hook_set(enhance_actions_tick);
#endif
}

int enhance_actions_arm(const char *key)
{
    return enhance_actions_arm_owned(key, -1);
}

int enhance_actions_arm_owned(const char *key, int owner)
{
    int was = __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
    if (!arm_key(key, owner))
        return was;
    if (!s_started)
        return 1;   /* start arms and logs */
    if (!was) {
        __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
        install();
        hook_line();
    }
    return 1;
}

static void disarm_key(const char *key, int owner);

int enhance_actions_arm_set(const char *key, int owner, int on)
{
    int was = __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
    if (!on) {
        disarm_key(key, owner);
        return enhance_actions_armed();
    }
    if (!arm_key_on(key, owner, 1))
        return was;
    if (!s_started)
        return 1;
    if (!was) {
        __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
        install();
        hook_line();
    }
    return 1;
}

int enhance_actions_armed(void)
{
    return __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE);
}

int enhance_actions_keyed(const char *key)
{
    int hit = 0;
    spin_lock(&s_arm_lock);
    for (int i = 0; key && i < s_narm && !hit; i++)
        hit = !strcmp(s_arm_keys[i], key);
    spin_unlock(&s_arm_lock);
    return hit;
}

/* A mod left out (its init failed) or dropped takes the keys it armed
 * with; the last one gone disarms the hook. */
static void disarm_match(int all_mods, int owner, const char *key)
{
    int n = 0, was;

    spin_lock(&s_arm_lock);
    was = s_narm;
    for (int i = 0; i < s_narm; i++) {
        if (s_arm_owner[i] >= 0 && (all_mods || s_arm_owner[i] == owner) &&
            (!key || !strcmp(s_arm_keys[i], key)))
            continue;
        if (n != i) {
            memcpy(s_arm_keys[n], s_arm_keys[i], NAME_MAX_);
            s_arm_owner[n] = s_arm_owner[i];
        }
        n++;
    }
    s_narm = n;
    spin_unlock(&s_arm_lock);
    if (n == was)
        return;
    if (!n && s_started && __atomic_load_n(&s_armed, __ATOMIC_ACQUIRE)) {
        __atomic_store_n(&s_armed, 0, __ATOMIC_RELEASE);
        xbox_InputSetPadFilter(NULL);
#ifdef XBOXRECOMP_ENHANCE
        nv2a_flip_hook_set(NULL);
#endif
        panel_stop();
        hook_line();
    }
}

static void disarm_where(int all_mods, int owner)
{
    disarm_match(all_mods, owner, NULL);
}

/* A mod's key switched off live: only that mod's entry goes (the toolkit's
 * own entry for the same key stays). */
static void disarm_key(const char *key, int owner)
{
    if (key && owner >= 0)
        disarm_match(0, owner, key);
}

/* ---- the menu's calls --------------------------------------------------- */

void enhance_actions_set_dormant(int owner, int on)
{
    int changed = 0, started;

    spin_lock(&s_reg_lock);
    for (int i = 0; i < s_nact; i++)
        if (s_act[i].live && s_act[i].owner == owner && owner >= 0 &&
            s_act[i].dormant != !!on) {
            s_act[i].dormant = !!on;
            changed = 1;
        }
    started = s_started;
    spin_unlock(&s_reg_lock);
    if (changed && started)
        enhance_actions_rebuild();
}

int enhance_action_dormant(int id)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    return id >= 0 && id < n && s_act[id].live && s_act[id].dormant;
}

int enhance_action_mod(int id)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    if (id < 0 || id >= n || !s_act[id].live)
        return ENHANCE_ACT_TOOLKIT;
    if (s_act[id].owner >= 0)
        return s_act[id].owner;
    return id < s_ntoolkit ? ENHANCE_ACT_TOOLKIT : ENHANCE_ACT_GAME;
}

int enhance_action_source(int id, char *key, size_t n)
{
    const char *texts[ENHANCE_ACTION_BINDS];
    char where[96];
    int src = ENHANCE_ACT_SRC_DEFAULT;

    if (key && n)
        key[0] = 0;
    spin_lock(&s_reg_lock);
    if (id >= 0 && id < s_nact && s_act[id].live) {
        s_act[id].srckey[0] = 0;
        config_texts(&s_act[id], texts, where, NULL);
        src = s_act[id].src;
        if (key && n)
            snprintf(key, n, "%s", s_act[id].srckey);
    }
    spin_unlock(&s_reg_lock);
    return src;
}

const char *enhance_action_source_name(int src)
{
    static const char *const names[] = { "default", "env", "menu", "user", "user_alias",
                                         "defaults", "defaults_alias" };
    return src >= 0 && src < (int)(sizeof names / sizeof names[0]) ? names[src] : "?";
}

int enhance_action_count(void)
{
    return __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
}

const char *enhance_action_label(int id)
{
    int n = __atomic_load_n(&s_nact, __ATOMIC_ACQUIRE);
    return id >= 0 && id < n && s_act[id].live ? s_act[id].label : NULL;
}

int enhance_action_bindings(int id, char out[][ENHANCE_ACTION_TEXT], int max)
{
    const char *texts[ENHANCE_ACTION_BINDS];
    char where[96];
    int n = 0, k;

    spin_lock(&s_reg_lock);
    if (id < 0 || id >= s_nact || !s_act[id].live) {
        spin_unlock(&s_reg_lock);
        return 0;
    }
    k = config_texts(&s_act[id], texts, where, NULL);
    for (int i = 0; i < k && n < max; i++)
        if (texts[i] && *texts[i])
            snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s", texts[i]);
    spin_unlock(&s_reg_lock);
    return n;
}

int enhance_action_refused(int id, char out[][ENHANCE_ACTION_TEXT], int max)
{
    const char *texts[ENHANCE_ACTION_BINDS];
    char where[96];
    int n = 0, k;

    /* s_reg_lock then s_step_lock, as a rebuild takes them. */
    spin_lock(&s_reg_lock);
    if (id < 0 || id >= s_nact || !s_act[id].live) {
        spin_unlock(&s_reg_lock);
        return 0;
    }
    k = config_texts(&s_act[id], texts, where, NULL);
    spin_lock(&s_step_lock);
    for (int i = 0; i < k && n < max; i++) {
        int held = 0;
        if (!texts[i] || !*texts[i])
            continue;
        for (int j = 0; s_table && j < s_table->n && !held; j++)
            /* An off binding stays in the table but never fires: the
             * game keeps that input (classify). */
            held = s_table->b[j].action == id && s_table->b[j].kind != B_OFF &&
                   !strcmp(s_table->b[j].text, texts[i]);
        if (!held)
            snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s", texts[i]);
    }
    spin_unlock(&s_step_lock);
    spin_unlock(&s_reg_lock);
    return n;
}

int enhance_action_default_bindings(int id, char out[][ENHANCE_ACTION_TEXT], int max)
{
    int n = 0;

    spin_lock(&s_reg_lock);
    if (id >= 0 && id < s_nact && s_act[id].live)
        for (int i = 0; i < s_act[id].ndefs && n < max; i++)
            snprintf(out[n++], ENHANCE_ACTION_TEXT, "%s", s_act[id].defs[i]);
    spin_unlock(&s_reg_lock);
    return n;
}

int enhance_action_set_bindings(int id, const char *const *texts, int n)
{
    struct action *a;

    if (n > ENHANCE_ACTION_BINDS)
        return 0;
    spin_lock(&s_reg_lock);
    if (id < 0 || id >= s_nact || !s_act[id].live) {
        spin_unlock(&s_reg_lock);
        return 0;
    }
    a = &s_act[id];
    a->has_ovr = n >= 0;
    a->novr = 0;
    for (int i = 0; i < n; i++)
        if (texts[i] && *texts[i])
            snprintf(a->ovr[a->novr++], TEXT_MAX, "%s", texts[i]);
    spin_unlock(&s_reg_lock);
    enhance_actions_rebuild();
    return 1;
}

int enhance_action_check(const char *text, char *why, size_t n)
{
    struct binding b;
    xbox_pad_setup ps;
    uint32_t feeds[SRC_BITS], used;
    char dummy[8];
    int ok;

    if (!why || !n) {
        why = dummy;
        n = sizeof dummy;
    }
    why[0] = 0;
    s_why = why;
    s_why_n = n;
    ok = parse_binding("check", text, &b);
    if (ok && b.kind == B_PAD) {
        xbox_InputPadSetup(&ps);
        used = map_feeds(&ps, feeds);
        classify(&b, "check", &ps, used, NULL);
        ok = b.kind != B_OFF;
    }
    s_why = NULL;
    if (ok)
        snprintf(why, n, "%s", kind_name(&b));
    return ok;
}

int enhance_action_owner(const char *text, int except, char *bound, size_t n)
{
    struct binding b;
    xbox_pad_setup ps;
    uint32_t feeds[SRC_BITS], used;
    char why[8];
    int owner = -1;

    if (bound && n)
        bound[0] = 0;
    s_why = why;
    s_why_n = sizeof why;
    if (!parse_binding("check", text, &b)) {
        s_why = NULL;
        return -1;
    }
    if (b.kind == B_PAD) {
        xbox_InputPadSetup(&ps);
        used = map_feeds(&ps, feeds);
        classify(&b, "check", &ps, used, NULL);
    }
    s_why = NULL;
    spin_lock(&s_step_lock);
    for (int i = 0; s_table && i < s_table->n && owner < 0; i++)
        if (s_table->b[i].action != except && same_input(&s_table->b[i], &b)) {
            owner = s_table->b[i].action;
            if (bound && n)
                snprintf(bound, n, "%s", s_table->b[i].text);
        }
    spin_unlock(&s_step_lock);
    return owner;
}

/* ---- captured controls (mod-settings D3) --------------------------------- */

int enhance_control_parse(const char *text, struct enhance_ctl_src *out)
{
    char buf[TEXT_MAX];
    int lower = 1, ctl, code, mouse;

    memset(out, 0, sizeof *out);
    if (!text || strlen(text) >= sizeof buf)
        return 0;
    snprintf(buf, sizeof buf, "%s", text);
    trim(buf);
    for (const char *p = buf; *p; p++)
        if (isupper((unsigned char)*p))
            lower = 0;
    /* The action bindings' namespace (pad_name_src): a guest name in
     * lowercase is the pad ("x" the button), anything else a key ("X",
     * "space"). */
    if (lower && (!strcmp(buf, "left_stick") || !strcmp(buf, "right_stick"))) {
        out->type = ENHANCE_CTL_STICK;
        out->idx = buf[0] == 'l' ? (int)XBOX_CTL_SRC_LEFT : (int)XBOX_CTL_SRC_RIGHT;
        return 1;
    }
    ctl = !lower ? -1
        : !strcmp(buf, "lt") ? INPUT_LEFT_TRIGGER
        : !strcmp(buf, "rt") ? INPUT_RIGHT_TRIGGER : xbox_InputMap_ControlFromName(buf);
    /* A stick direction is no source: a stick is bound whole. */
    if (ctl >= INPUT_DPAD_UP && ctl < INPUT_A) {
        out->type = ENHANCE_CTL_BUTTON;
        out->idx = ctl;
        return 1;
    }
    if (ctl >= INPUT_A && ctl <= INPUT_RIGHT_TRIGGER) {
        out->type = ENHANCE_CTL_ANALOG;
        out->idx = ctl - INPUT_A;
        return 1;
    }
    if (ctl >= 0 || !xbox_InputNameToCode(buf, &code, &mouse) || mouse)
        return 0;
    out->type = ENHANCE_CTL_KEY;
    out->key = code;
    return 1;
}

int enhance_control_same(const struct enhance_ctl_src *a, const struct enhance_ctl_src *b)
{
    if (a->type != b->type || a->type == ENHANCE_CTL_NONE)
        return 0;
    return a->type == ENHANCE_CTL_KEY ? same_key(a->key, b->key) : a->idx == b->idx;
}

int enhance_control_from_host(int src, int stick, char *out, size_t n, char *why, size_t wn)
{
    xbox_pad_setup ps;
    uint32_t feeds[SRC_BITS], f;
    int c;

    if (n)
        out[0] = 0;
    if (wn)
        why[0] = 0;
    if (src <= INPUT_SRC_NONE || src >= INPUT_SRC_COUNT)
        return 0;
    xbox_InputPadSetup(&ps);
    map_feeds(&ps, feeds);
    f = feeds[src] & GUEST_BITS;
    /* The game's side of the map is what control_value reads, so a source
     * that feeds nothing there (a paddle, Guide) could never move it. */
    if (!f) {
        snprintf(why, wn, "fly controls take buttons the game sees");
        return 0;
    }
    if (f >> STICK_BIT0) {
        if (!stick) {
            snprintf(why, wn, "a stick goes on a stick row");
            return 0;
        }
        /* Bits 16..19 are the guest's left stick, 20..23 its right. */
        snprintf(out, n, "%s", (f >> STICK_BIT0) & 0xFu ? "left_stick" : "right_stick");
        return 1;
    }
    if (stick) {
        snprintf(why, wn, "a stick row takes a stick");
        return 0;
    }
    for (c = 0; c < INPUT_PAD_CONTROL_COUNT && !(f & control_bit(c)); c++)
        ;
    /* The triggers as the defaults spell them. */
    snprintf(out, n, "%s", c == INPUT_LEFT_TRIGGER ? "lt" : c == INPUT_RIGHT_TRIGGER ? "rt"
                         : xbox_InputMap_ControlName(c));
    return 1;
}

static void action_owner_text(int id, const char *input, char *why, size_t n)
{
    int g = enhance_action_mod(id);
    const struct xbox_mod_desc *d = g >= 0 ? xbox_mod_desc_shown(g) : NULL;
    const char *label = enhance_action_label(id);
    if (g >= 0)
        snprintf(why, n, "%s is %s: %s", input,
                 d && d->title && *d->title ? d->title : xbox_mod_name(g), label ? label : "?");
    else
        snprintf(why, n, "%s is %s", input, label ? label : "another action's");
}

/* The action in the table whose binding is key alone, or -1. The built
 * table holds dormant bindings too (rule 5). */
static int single_key_owner(int key)
{
    int owner = -1;

    spin_lock(&s_step_lock);
    for (int i = 0; s_table && i < s_table->n && owner < 0; i++)
        if (s_table->b[i].kind == B_KEY && !s_table->b[i].nmods
            && same_key(s_table->b[i].key, key))
            owner = s_table->b[i].action;
    spin_unlock(&s_step_lock);
    return owner;
}

int enhance_control_conflict(int m, uint32_t k, const char *text, int *other,
                             char *why, size_t n)
{
    const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
    struct enhance_ctl_src want, have;
    char l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT];
    int owner;

    if (other)
        *other = -1;
    if (n)
        why[0] = 0;
    if (!d || !enhance_control_parse(text, &want))
        return 0;
    /* Rule 1: two of one mod's fly controls on one source cannot both
     * work. */
    for (uint32_t c = 0; c < d->n_controls; c++) {
        const struct xbox_mod_control *ct = &d->controls[c];
        int nb;
        if (c == k || (ct->kind != XBOX_CTL_BUTTON && ct->kind != XBOX_CTL_STICK))
            continue;
        nb = xbox_mod_control_bindings(m, c, l, ENHANCE_ACTION_BINDS, NULL);
        for (int b = 0; b < nb; b++)
            if (enhance_control_parse(l[b], &have) && enhance_control_same(&want, &have)) {
                if (other)
                    *other = (int)c;
                snprintf(why, n, "%s is %s: %s", text,
                         d->title && *d->title ? d->title : xbox_mod_name(m),
                         ct->label ? ct->label : ct->id);
                return 1;
            }
    }
    /* Rule 2: the actions are consumed first, so the control would never
     * see a key an action has. A guest button needs no check: no action
     * is a single game button (R3). */
    if (want.type == ENHANCE_CTL_KEY && (owner = single_key_owner(want.key)) >= 0) {
        action_owner_text(owner, text, why, n);
        return 1;
    }
    return 0;
}

int enhance_action_control_clash(const char *text, char *why, size_t n)
{
    struct binding b;
    struct enhance_ctl_src have;
    char l[ENHANCE_ACTION_BINDS][ENHANCE_ACTION_TEXT], quiet[8];

    if (n)
        why[0] = 0;
    s_why = quiet;
    s_why_n = sizeof quiet;
    if (!parse_binding("check", text, &b) || b.kind != B_KEY || b.nmods) {
        s_why = NULL;
        return 0;
    }
    s_why = NULL;
    for (int m = 0; m < xbox_mod_count(); m++) {
        const struct xbox_mod_desc *d = xbox_mod_desc_of(m);
        for (uint32_t c = 0; d && c < d->n_controls; c++) {
            const struct xbox_mod_control *ct = &d->controls[c];
            int nb;
            if (ct->kind != XBOX_CTL_BUTTON && ct->kind != XBOX_CTL_STICK)
                continue;
            nb = xbox_mod_control_bindings(m, c, l, ENHANCE_ACTION_BINDS, NULL);
            for (int i = 0; i < nb; i++)
                if (enhance_control_parse(l[i], &have) && have.type == ENHANCE_CTL_KEY
                    && same_key(have.key, b.key)) {
                    snprintf(why, n, "%s: %s uses it",
                             d->title && *d->title ? d->title : xbox_mod_name(m),
                             ct->label ? ct->label : ct->id);
                    return 1;
                }
        }
    }
    return 0;
}

void enhance_actions_quiet(int on)
{
    /* Off drains through one more quiet step (2), see step(). */
    int was = __atomic_load_n(&s_quiet, __ATOMIC_ACQUIRE);
    __atomic_store_n(&s_quiet, on ? 1 : was ? 2 : 0, __ATOMIC_RELEASE);
}

void enhance_actions_set_menu_hook(enhance_menu_hook_fn fn)
{
    __atomic_store_n(&s_menu_req, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_menu_hook, fn, __ATOMIC_RELEASE);
}

void enhance_actions_set_panel_hook(void (*fn)(int stop))
{
    void (*old)(int) = __atomic_exchange_n(&s_panel_hook, fn, __ATOMIC_ACQ_REL);
    if (old && old != fn)
        old(1);
}

int enhance_actions_menu_request(void)
{
    return __atomic_exchange_n(&s_menu_req, 0, __ATOMIC_ACQ_REL);
}

/* ---- the toolkit's actions and the start ------------------------------ */

void enhance_actions_init(void)
{
    /* paddle1 is the Deck's R4 (an Elite's P1). Steam keeps the Steam
     * button and Steam+A is its screen recording, so guide+a is no use on
     * the Deck. The SDL3 backend reads the paddle; XInput has none, so
     * under Proton Steam Input maps R4 to F10 instead (config.md, "Host
     * actions"). Host-only, so it never takes a press from the game. */
    static const char *const menu_open[] = { "F10", "guide+a", "paddle1", NULL };

    enhance_actions_reset();
    s_menu_act = enhance_action_register("menu.open", "Open the menu", menu_open);
    s_stats_act = enhance_action_register("overlay.stats", "Stats strip", NULL);
    enhance_action_register("shot.window", "Window screenshot", NULL);
    s_ntoolkit = s_nact;
}

void enhance_actions_start(void)
{
    int keyed;

    /* Feature keys only (rule 6): the menu and the stats strip, plus a
     * mod's own key it armed with during its init. */
    arm_key("menu.enabled", -1);
    arm_key("overlay.stats", -1);
    spin_lock(&s_arm_lock);
    keyed = s_narm > 0;
    spin_unlock(&s_arm_lock);
    report_unknown();
    enhance_actions_rebuild();
    spin_lock(&s_reg_lock);
    s_started = 1;
    spin_unlock(&s_reg_lock);
    if (keyed) {
        __atomic_store_n(&s_armed, 1, __ATOMIC_RELEASE);
        install();
    }
    hook_line();
    fflush(stderr);
}

void enhance_actions_reset(void)
{
    __atomic_store_n(&s_armed, 0, __ATOMIC_RELEASE);
    xbox_InputSetPadFilter(NULL);
#ifdef XBOXRECOMP_ENHANCE
    nv2a_flip_hook_set(NULL);
#endif
    panel_stop();
    enhance_actions_set_menu_hook(NULL);
    enhance_actions_set_panel_hook(NULL);
    spin_lock(&s_reg_lock);
    s_nact = 0;
    s_ntoolkit = 0;
    s_started = 0;
    spin_lock(&s_arm_lock);
    s_narm = 0;
    spin_unlock(&s_arm_lock);
    s_delay_ms = 0;
    memset(s_act, 0, sizeof s_act);
    spin_unlock(&s_reg_lock);
    spin_lock(&s_step_lock);
    free(s_table);
    s_table = NULL;
    memset(s_port, 0, sizeof s_port);
    for (int i = 0; i < PORTS; i++)
        s_port[i].slot = -1;
    spin_unlock(&s_step_lock);
    s_log_sec = 0;
    s_log_n = s_log_dropped = 0;
    __atomic_store_n(&s_quiet, 0, __ATOMIC_RELEASE);
}
