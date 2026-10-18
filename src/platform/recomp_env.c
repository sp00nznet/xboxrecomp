/*
 * recomp_env.c - read the environment once; see recomp_env.h.
 */
#include "recomp_env.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { TIER_CONFIG, TIER_TRACE, TIER_DEBUG };

typedef struct {
    int tier;
    const char *name;     /* variable (config) or list key */
    const char *legacy;   /* alias for the transition window, or NULL */
    const char *help;
} EnvKey;

static const EnvKey s_keys[RENV_COUNT] = {
#define RECOMP_ENV_ROW_(id, tier, name, legacy, help) { TIER_##tier, name, legacy, help },
    RECOMP_ENV_KEYS(RECOMP_ENV_ROW_)
#undef RECOMP_ENV_ROW_
};

const char *recomp_env_value_[RENV_COUNT];
volatile int recomp_env_ready_;

static const char *const s_list_var[] = { NULL, "RECOMP_TRACE", "RECOMP_DEBUG" };

/* Values are owned copies; on reload the old ones are leaked on purpose (a
 * caller may still hold one, and reload is for tests). */
static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

/* What the reader has to say (deprecations, unknown keys, the help table) is
 * held until recomp_env_flush_notes(): the environment is read before the
 * host redirects stderr (RECOMP_STDIO_LOG is itself read from it), and under
 * Proton anything printed before that redirect is lost. An atexit hook
 * prints whatever a host never flushed. */
static char *s_notes;
static size_t s_notes_len, s_notes_cap;

static void note(const char *fmt, ...)
{
    char line[512];
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n < 0)
        return;
    if ((size_t)n >= sizeof line)
        n = (int)sizeof line - 1;
    if (s_notes_len + (size_t)n + 1 > s_notes_cap) {
        size_t cap = s_notes_cap ? s_notes_cap * 2 : 4096;
        char *b;
        while (cap < s_notes_len + (size_t)n + 1)
            cap *= 2;
        b = (char *)realloc(s_notes, cap);
        if (!b)
            return;
        s_notes = b;
        s_notes_cap = cap;
    }
    memcpy(s_notes + s_notes_len, line, (size_t)n);
    s_notes_len += (size_t)n;
    s_notes[s_notes_len] = 0;
}

void recomp_env_flush_notes(void)
{
    if (s_notes_len) {
        fputs(s_notes, stderr);
        fflush(stderr);
    }
    s_notes_len = 0;
}

/* Keys whose values contain commas. In a list, a fragment that is not a key
 * continues the value before it only after one of these; anywhere else it
 * is a typo and gets a warning, not silently glued onto a path. */
static const char *const s_comma_keys[] = {
    "peek", "peek_chain", "px", "px_consts", "dsp_ack", "apu_dsp_ack",
    "poke", "fb_dump_at", "window_shot", "pad_script", "vsh_const_prog",
    "fb_dump_prog",
#ifdef RECOMP_ENV_GAME_COMMA_KEYS
    RECOMP_ENV_GAME_COMMA_KEYS
#endif
};

static int takes_commas(int id)
{
    size_t i;
    for (i = 0; i < sizeof s_comma_keys / sizeof s_comma_keys[0]; i++)
        if (!strcmp(s_keys[id].name, s_comma_keys[i]))
            return 1;
    return 0;
}

static int find_key(int tier, const char *key, size_t len)
{
    int i;
    for (i = 0; i < RENV_COUNT; i++)
        if (s_keys[i].tier == tier && strlen(s_keys[i].name) == len
                && !strncmp(s_keys[i].name, key, len))
            return i;
    return -1;
}

static void print_help(void)
{
    static const char *const title[] = {
        "config (each its own variable)",
        "RECOMP_TRACE=key[=value],...",
        "RECOMP_DEBUG=key[=value],...",
    };
    int t, i;
    for (t = TIER_CONFIG; t <= TIER_DEBUG; t++) {
        note("[ENV] %s\n", title[t]);
        for (i = 0; i < RENV_COUNT; i++)
            if (s_keys[i].tier == t)
                note("[ENV]   %-26s %s\n", s_keys[i].name, s_keys[i].help);
    }
}

/* RECOMP_TRACE / RECOMP_DEBUG. Sets values[] and marks set[]. */
static void parse_list(int tier, const char **values, char *set)
{
    const char *var = s_list_var[tier];
    const char *s = getenv(var);
    int last = -1;               /* key whose value a bare fragment extends */

    while (s && *s) {
        const char *frag = s, *end = strchr(s, ','), *eq;
        size_t len = end ? (size_t)(end - s) : strlen(s);
        size_t klen;
        int id, other = 0;

        s = end ? end + 1 : NULL;
        while (len && (*frag == ' ' || *frag == '\t')) { frag++; len--; }
        while (len && (frag[len - 1] == ' ' || frag[len - 1] == '\t')) len--;
        if (!len)
            continue;
        eq = memchr(frag, '=', len);
        klen = eq ? (size_t)(eq - frag) : len;
        if (klen == 4 && !strncmp(frag, "help", 4)) {
            print_help();
            last = -1;
            continue;
        }
        id = find_key(tier, frag, klen);
        if (id < 0) {
            id = find_key(tier == TIER_TRACE ? TIER_DEBUG : TIER_TRACE, frag, klen);
            other = id >= 0;
        }
        if (id < 0) {
            if (last >= 0 && takes_commas(last)) {
                /* A value with commas in it: fb_dump_at=61,121,181. */
                size_t old = strlen(values[last]);
                char *v = (char *)malloc(old + 1 + len + 1);
                if (v) {
                    memcpy(v, values[last], old);
                    v[old] = ',';
                    memcpy(v + old + 1, frag, len);
                    v[old + 1 + len] = 0;
                    values[last] = v;
                }
                continue;
            }
            note("[ENV] %s: unknown key '%.*s' ignored (%s=help lists them)\n",
                    var, (int)klen, frag, var);
            continue;
        }
        if (other)
            note("[ENV] %s: '%s' belongs in %s; taken anyway\n",
                    var, s_keys[id].name, s_list_var[s_keys[id].tier]);
        if (eq) {
            char *v = (char *)malloc(len - klen);
            if (v) {
                memcpy(v, eq + 1, len - klen - 1);
                v[len - klen - 1] = 0;
            }
            values[id] = v;
            last = id;
        } else {
            values[id] = "1";
            last = -1;
        }
        set[id] = 1;
    }
}

static void set_process_env(const char *name, const char *value)
{
#if defined(_WIN32)
    _putenv_s(name, value ? value : "");
#else
    if (value)
        setenv(name, value, 1);
    else
        unsetenv(name);
#endif
}

static void default_on(const char **values, int id, int supported)
{
    const char *v = values[id];
    int on = v ? (*v && *v != '0') : 1;

    if (on && !supported) {
        if (v)
            note("[ENV] %s: audio unsupported on this host; ignored\n", s_keys[id].name);
        on = 0;
    }
    values[id] = on ? (v ? v : "1") : NULL;
}

static void load(void)
{
    const char *values[RENV_COUNT];
    char set[RENV_COUNT];
    int i;

    memset(values, 0, sizeof values);
    memset(set, 0, sizeof set);
    parse_list(TIER_TRACE, values, set);
    parse_list(TIER_DEBUG, values, set);

    for (i = 0; i < RENV_COUNT; i++) {
        const EnvKey *k = &s_keys[i];
        const char *old;

        if (k->tier == TIER_CONFIG) {
            const char *v = getenv(k->name);
            if (v) {
                values[i] = dup_str(v);
                set[i] = 1;
            }
        }
        old = k->legacy ? getenv(k->legacy) : NULL;
        if (!old)
            continue;
        /* The old name: still honoured, once per run says what replaces it. */
        if (k->tier == TIER_CONFIG)
            note("[ENV] %s is deprecated; use %s%s\n", k->legacy, k->name,
                    set[i] ? " (set too, so it wins)" : "");
        else
            note("[ENV] %s is deprecated; use %s=%s%s%s%s\n", k->legacy,
                    s_list_var[k->tier], k->name,
                    (*old && strcmp(old, "1")) ? "=" : "",
                    (*old && strcmp(old, "1")) ? old : "",
                    set[i] ? " (that is set too, so it wins)" : "");
        if (!set[i]) {
            values[i] = dup_str(old);
            set[i] = 1;
        }
    }

    /* Switches that default on: unset means on, =0 (or empty) off, so a
     * reader keeps asking only "is it set". Audio needs the host to service
     * the APU's MMIO faults (x86-64 Windows/Proton, or POSIX arm64 with the
     * A64 decoder); elsewhere it is off,
     * with a note when asked for explicitly. */
    default_on(values, RENV_PB_EXEC, 1);
#if (defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))) || \
    (!defined(_WIN32) && defined(__aarch64__))
    default_on(values, RENV_AC97_READY, 1);
#else
    default_on(values, RENV_AC97_READY, 0);
#endif

    /* The DSOUND GP doorbell ack defaults to `auto`: the XDK's GP program
     * takes commands through a scratch block whose dword +0x810 the DSP
     * clears, and every XDK title seen (three so far) spins on it after
     * each submit. `auto` finds the block from GPSADDR, so a title whose
     * DSOUND never programs it gets no ack and nothing changes for it;
     * =0 turns it off, =va,... names the words (apu_dsp.c). */
    if (!set[RENV_APU_DSP_ACK])
        values[RENV_APU_DSP_ACK] = "auto";

#ifdef RECOMP_ENV_GAME_DEFAULTS
    /* The game's defaults for toolkit keys (recomp_env_game.h): used when
     * neither spelling is in the environment; they win over the toolkit's
     * own defaults above. */
#define RENV_GAME_DEFAULT(id, v) if (!set[RENV_##id]) values[RENV_##id] = (v);
    RECOMP_ENV_GAME_DEFAULTS(RENV_GAME_DEFAULT)
#undef RENV_GAME_DEFAULT
#endif

    /* Generated code (the recompiler's unresolved-stub file) reads its
     * switch with getenv and only changes on a regen; hand it the value. */
    if (set[RENV_STUB_LOG] && !getenv("RECOMP_STUB_LOG"))
        set_process_env("RECOMP_STUB_LOG", values[RENV_STUB_LOG]);

    for (i = 0; i < RENV_COUNT; i++)
        recomp_env_value_[i] = values[i];
}

/* State: 0 unread, 1 reading, 2 ready. Normally main reads it before any
 * thread exists; the spin only covers a lazy first lookup racing another. */
static volatile long s_state;

#if defined(_MSC_VER)
#include <intrin.h>
static long cas(volatile long *p, long expect, long want)
{
    return _InterlockedCompareExchange(p, want, expect);
}
#else
static long cas(volatile long *p, long expect, long want)
{
    return __sync_val_compare_and_swap(p, expect, want);
}
#endif

void recomp_env_init(void)
{
    if (recomp_env_ready_)
        return;
    if (cas(&s_state, 0, 1) == 0) {
        load();
        atexit(recomp_env_flush_notes);
        cas(&s_state, 1, 2);              /* full barrier: values first */
        recomp_env_ready_ = 1;
        return;
    }
    while (cas(&s_state, 2, 2) != 2)
        ;
}

void recomp_env_reload(void)
{
    recomp_env_init();
    load();
}

void recomp_env_set(recomp_env_id id, const char *value)
{
    recomp_env_init();
    if ((unsigned)id < RENV_COUNT)
        recomp_env_value_[id] = value ? dup_str(value) : NULL;
}

long recomp_env_int(recomp_env_id id, long def)
{
    const char *v = recomp_env(id);
    return v ? atol(v) : def;
}

unsigned long recomp_env_ulong(recomp_env_id id, unsigned long def)
{
    const char *v = recomp_env(id);
    return v ? strtoul(v, NULL, 0) : def;
}

double recomp_env_double(recomp_env_id id, double def)
{
    const char *v = recomp_env(id);
    return v ? atof(v) : def;
}

const char *recomp_env_name(recomp_env_id id)
{
    return (unsigned)id < RENV_COUNT ? s_keys[id].name : "?";
}
