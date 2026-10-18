/*
 * kbm_script: key and mouse events injected on the pad poll, for tests that
 * have no one at the keyboard. RECOMP_DEBUG=kbm_script=<file>.
 *
 *   2.0   key down Return      # seconds since the first poll
 *   2.2   key up Return
 *   40p   key down W           # polls since the first poll (that one is 0p)
 *   5.0   mouse move 20 -5     # counts, applied as one poll's motion
 *   5.1   mouse down MouseLeft
 *   6.0   wheel 1
 *
 * The events go into the same tables the windows write (xbox_InputKeyEvent,
 * the mouse calls), so a headless run exercises everything from the name
 * table to the title's pad struct. Wayland sessions refuse synthetic input
 * (xdotool), and a Mac run cannot be given a key without a person; this is
 * the one path that tests the whole chain unattended.
 *
 * The script captures the mouse itself, since a headless run has no window
 * to focus. It is refused when the title runs its own input script
 * (xbox_InputKbmScriptAllow(0)): two scripts on one port would race.
 */
#include "keyboard.h"
#include "input_names.h"
#include "mouse.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { EV_KEY, EV_MOVE, EV_BUTTON, EV_WHEEL };

typedef struct {
    uint64_t at;             /* ms, or polls when in_polls */
    unsigned char in_polls, kind, done;
    int a, b, down;
} kbm_event;

static kbm_event *s_ev;
static int        s_n, s_cap;
static int        s_allow = 1, s_tried;
static uint64_t   s_polls, s_t0;
static int        s_started;

void xbox_InputKbmScriptAllow(int allow)
{
    s_allow = allow != 0;
}

void xbox_InputKbmScriptReset(void)
{
    free(s_ev);
    s_ev = NULL;
    s_n = s_cap = 0;
    s_tried = 1;             /* a test's script, not the environment's */
    s_polls = s_t0 = 0;
    s_started = 0;
}

static void bad(const char *name, int line, const char *msg, const char *what)
{
    fprintf(stderr, "[INPUT] kbm_script: %s:%d: %s '%s'; skipped\n",
            name, line, msg, what);
}

/* One line, already stripped of its comment. 1 when an event was added. */
static int parse_line(char *s, const char *name, int line)
{
    char *tok[6];
    int nt = 0;
    kbm_event e;
    char *end;
    int code, mouse;

    for (char *p = strtok(s, " \t\r\n"); p && nt < 6; p = strtok(NULL, " \t\r\n"))
        tok[nt++] = p;
    if (!nt)
        return 0;
    memset(&e, 0, sizeof e);
    {
        size_t len = strlen(tok[0]);
        if (len > 1 && (tok[0][len - 1] == 'p' || tok[0][len - 1] == 'P')) {
            unsigned long long v = strtoull(tok[0], &end, 10);
            if (end != tok[0] + len - 1) { bad(name, line, "bad time", tok[0]); return 0; }
            e.at = v;
            e.in_polls = 1;
        } else {
            double v = strtod(tok[0], &end);
            if (*end || v < 0) { bad(name, line, "bad time", tok[0]); return 0; }
            e.at = (uint64_t)(v * 1000.0 + 0.5);
        }
    }
    if (nt == 4 && !strcmp(tok[1], "key")
        && (!strcmp(tok[2], "down") || !strcmp(tok[2], "up"))) {
        if (!xbox_InputNameToCode(tok[3], &code, &mouse) || mouse) {
            bad(name, line, "not a key name", tok[3]);
            return 0;
        }
        e.kind = EV_KEY; e.a = code; e.down = !strcmp(tok[2], "down");
    } else if (nt == 5 && !strcmp(tok[1], "mouse") && !strcmp(tok[2], "move")) {
        long dx = strtol(tok[3], &end, 10);
        if (*end) { bad(name, line, "bad count", tok[3]); return 0; }
        long dy = strtol(tok[4], &end, 10);
        if (*end) { bad(name, line, "bad count", tok[4]); return 0; }
        e.kind = EV_MOVE; e.a = (int)dx; e.b = (int)dy;
    } else if (nt == 4 && !strcmp(tok[1], "mouse")
               && (!strcmp(tok[2], "down") || !strcmp(tok[2], "up"))) {
        if (!xbox_InputNameToCode(tok[3], &code, &mouse) || !mouse
            || code > XBOX_MOUSE_5) {
            bad(name, line, "not a mouse button", tok[3]);
            return 0;
        }
        e.kind = EV_BUTTON; e.a = code; e.down = !strcmp(tok[2], "down");
    } else if (nt == 3 && !strcmp(tok[1], "wheel")) {
        long n = strtol(tok[2], &end, 10);
        if (*end || !n) { bad(name, line, "bad notch count", tok[2]); return 0; }
        e.kind = EV_WHEEL; e.a = (int)n;
    } else {
        bad(name, line, "not an event", tok[1]);
        return 0;
    }
    if (s_n == s_cap) {
        int cap = s_cap ? s_cap * 2 : 64;
        kbm_event *p = realloc(s_ev, (size_t)cap * sizeof *p);
        if (!p)
            return 0;
        s_ev = p;
        s_cap = cap;
    }
    s_ev[s_n++] = e;
    return 1;
}

int xbox_InputKbmScriptParse(const char *text, const char *name)
{
    int line = 0, added = 0;
    const char *p = text;

    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        char buf[256];
        char *hash;

        line++;
        if (len >= sizeof buf)
            len = sizeof buf - 1;
        memcpy(buf, p, len);
        buf[len] = 0;
        if ((hash = strchr(buf, '#')))
            *hash = 0;
        added += parse_line(buf, name, line);
        p = nl ? nl + 1 : NULL;
    }
    return added;
}

static void load(void)
{
    const char *path = recomp_env(RENV_KBM_SCRIPT);
    FILE *f;
    char *text;
    long size;
    int n;

    s_tried = 1;
    if (!path || !*path)
        return;
    if (!s_allow) {
        fprintf(stderr, "[INPUT] kbm_script refused: RECOMP_INPUT_SCRIPT is set"
                        " (two scripts on one port would race)\n");
        fflush(stderr);
        return;
    }
    f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "[INPUT] kbm_script: cannot open %s\n", path);
        fflush(stderr);
        return;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    text = size >= 0 ? malloc((size_t)size + 1) : NULL;
    if (!text) {
        fclose(f);
        return;
    }
    size = (long)fread(text, 1, (size_t)size, f);
    text[size] = 0;
    fclose(f);
    n = xbox_InputKbmScriptParse(text, path);
    free(text);
    fprintf(stderr, "[INPUT] kbm_script: %d events (%s)\n", n, path);
    fflush(stderr);
    xbox_InputMouseForceCapture(1);
}

void xbox_InputKbmScriptPoll(uint64_t now_ms, int have_clock)
{
    uint64_t poll, ms;

    if (!s_tried)
        load();
    if (!s_n)
        return;
    if (!s_started) {
        s_started = 1;
        s_t0 = now_ms;
    }
    poll = s_polls++;
    ms = have_clock ? now_ms - s_t0 : 0;
    for (int i = 0; i < s_n; i++) {
        kbm_event *e = &s_ev[i];
        if (e->done || (e->in_polls ? e->at > poll : (!have_clock || e->at > ms)))
            continue;
        e->done = 1;
        /* These run on the poll thread while the window thread may feed real
         * keys: a debug-tier tool for unattended runs, where nobody types,
         * so the two writers of the key bytes are not serialised. */
        switch (e->kind) {
        case EV_KEY:    xbox_InputKeyEvent(e->a, e->down);    break;
        case EV_MOVE:   xbox_InputMouseMotion(e->a, e->b);    break;
        case EV_BUTTON: xbox_InputMouseButton(e->a, e->down); break;
        case EV_WHEEL:  xbox_InputMouseWheel(e->a);           break;
        }
    }
}
