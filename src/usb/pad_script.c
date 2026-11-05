/*
 * pad_script.c -- the scripted pad's grammar and the pad recorder.
 * See pad_script.h.
 */
#include "pad_script.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Stick directions, for screens and games that read the stick and ignore
 * the d-pad: Burnout 3 steers and picks map locations with it. */
static const struct { const char *name; uint8_t bit; } s_sticks[] = {
    {"lleft", 0x01}, {"lright", 0x02}, {"lup", 0x04}, {"ldown", 0x08},
    {"rleft", 0x10}, {"rright", 0x20}, {"rup", 0x40}, {"rdown", 0x80},
};

static const struct { const char *name; uint8_t dig, ana; } s_buttons[] = {
    {"up", 0x01, 0}, {"down", 0x02, 0}, {"left", 0x04, 0},
    {"right", 0x08, 0}, {"start", 0x10, 0}, {"back", 0x20, 0},
    {"lthumb", 0x40, 0}, {"rthumb", 0x80, 0},
    {"a", 0, 0x01}, {"b", 0, 0x02}, {"x", 0, 0x04}, {"y", 0, 0x08},
    {"black", 0, 0x10}, {"white", 0, 0x20}, {"lt", 0, 0x40}, {"rt", 0, 0x80},
};

#define COUNT(a) (sizeof (a) / sizeof (a)[0])

static int same_word(const char *a, const char *b, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i]))
            return 0;
    return 1;
}

static const char *const s_axes[4] = {"lx", "ly", "rx", "ry"};

/* "lx=-12000": a stick axis at a value. */
static int axis_value(const char *name, size_t n, PadStep *st)
{
    char num[8], *end;
    long v;
    int i;

    if (n < 4 || n - 3 >= sizeof num || name[2] != '=')
        return 0;
    for (i = 0; i < 4; i++)
        if (same_word(s_axes[i], name, 2))
            break;
    if (i == 4)
        return 0;
    memcpy(num, name + 3, n - 3);
    num[n - 3] = 0;
    v = strtol(num, &end, 10);
    if (*end || end == num || v < -32768 || v > 32767)
        return 0;
    st->axis[i] = (int16_t)v;
    st->axis_set |= (uint8_t)(1u << i);
    return 1;
}

static int button_bits(const char *name, size_t n, PadStep *st)
{
    size_t i;

    if (axis_value(name, n, st))
        return 1;
    for (i = 0; i < COUNT(s_sticks); i++) {
        if (strlen(s_sticks[i].name) == n && same_word(s_sticks[i].name, name, n)) {
            st->stick |= s_sticks[i].bit;
            return 1;
        }
    }
    for (i = 0; i < COUNT(s_buttons); i++) {
        size_t k = strlen(s_buttons[i].name);
        if (k == n && same_word(s_buttons[i].name, name, n)) {
            st->digital |= s_buttons[i].dig;
            st->analog |= s_buttons[i].ana;
            return 1;
        }
        /* "lt=97": an analog button at that depth. */
        if (s_buttons[i].ana && n > k + 1 && n - k - 1 <= 3 && name[k] == '='
            && same_word(s_buttons[i].name, name, k)) {
            char num[4], *end;
            unsigned long v;
            int j;
            memcpy(num, name + k + 1, n - k - 1);
            num[n - k - 1] = 0;
            v = strtoul(num, &end, 10);
            if (*end || !isdigit((unsigned char)num[0]) || v < 1 || v > 255)
                return 0;
            for (j = 0; j < 8; j++)
                if (s_buttons[i].ana == 1u << j)
                    st->aval[j] = (uint8_t)(v == 255 ? 0 : v);
            st->analog |= s_buttons[i].ana;
            return 1;
        }
    }
    return 0;
}

static int contains_ci(const char *hay, const char *needle)
{
    size_t n = strlen(needle), h = strlen(hay), i;
    if (!n)
        return 0;
    for (i = 0; i + n <= h; i++)
        if (same_word(hay + i, needle, n))
            return 1;
    return 0;
}

/* A text with a directory in it matches anywhere in the path. One without
 * (what the recorder writes) matches the start of the file name, so
 * "op.sfd" is not taken by "stop.sfd" and "intro" still takes "intro.sfd". */
static int anchor_matches(const char *path, const char *text)
{
    const char *name;
    size_t n;

    if (strpbrk(text, "\\/"))
        return contains_ci(path, text);
    name = pad_path_name(path);
    n = strlen(text);
    return n && strlen(name) >= n && same_word(name, text, n);
}

/* An anchor's text and its retry's buttons stop at these. */
#define ANCHOR_STOP ":,# \t\r\n"

/* "@open:<text>[:<buttons>/<period>]" after the step's time, which st
 * holds. Inserts it into a, sorted by flip. Returns the text after the
 * entry, or NULL to end the parse (a bad entry, as for a step). */
static const char *parse_anchor(const char *b, const PadStep *st, PadAnchors *a,
                                int *added)
{
    PadAnchor an;
    size_t n;
    int pos;

    memset(&an, 0, sizeof an);
    if (!same_word(b, "@open:", 6)) {
        fprintf(stderr, "  PAD: unknown anchor \"%.20s\" (only @open:<text>)\n", b);
        return NULL;
    }
    b += 6;
    n = strcspn(b, ANCHOR_STOP);
    if (!n || n >= sizeof an.text) {
        fprintf(stderr, "  PAD: anchor without a text, or one past %d characters:"
                " at \"%.20s\"\n", (int)sizeof an.text - 1, b);
        return NULL;
    }
    if (!st->by_flip) {
        /* An anchor ties flips to an event; a millisecond step has none. */
        fprintf(stderr, "  PAD: anchor without f<flip> or g<poll>: at \"%.20s\"\n", b);
        return NULL;
    }
    memcpy(an.text, b, n);
    an.text[n] = 0;
    an.at = st->at;
    b += n;
    if (*b == ':') {
        /* The retry: <buttons>/<period>, with the step's "p2-" prefix. */
        const char *r = b + 1;
        char *end;
        if ((r[0] == 'p' || r[0] == 'P') && r[1] >= '1' && r[1] <= '4' && r[2] == '-') {
            an.retry.pad = (uint8_t)(r[1] - '1');
            r += 3;
        }
        for (;;) {
            size_t m = strcspn(r, "+/" ANCHOR_STOP);
            /* A retry that presses nothing would only wait: refuse it, as a
             * missing period is refused. */
            if (!button_bits(r, m, &an.retry) || an.retry.stick || an.retry.axis_set) {
                fprintf(stderr, "  PAD: anchor retry button \"%.*s\" unknown or"
                        " not a button\n", (int)m, r);
                return NULL;
            }
            r += m;
            if (*r != '+')
                break;
            r++;
        }
        if (*r != '/' || !isdigit((unsigned char)r[1])) {
            fprintf(stderr, "  PAD: anchor retry without /<period>: at \"%.20s\"\n", b);
            return NULL;
        }
        an.period = strtoul(r + 1, &end, 10);
        if (an.period < 2 || an.period > PAD_RETRY_MAX_FLIPS) {
            /* A press every flip is a hold: the title would see no edge.
             * The cap keeps at + period inside 32 bits on every host. */
            fprintf(stderr, "  PAD: anchor retry period not 2..%d flips: at \"%.20s\"\n",
                    PAD_RETRY_MAX_FLIPS, b);
            return NULL;
        }
        an.retry.by_flip = st->by_flip;
        an.retry.hold = an.period / 2;
        b = end;
    }
    if (a->n >= PAD_ANCHOR_MAX) {
        fprintf(stderr, "  PAD: more than %d anchors; @open:%s dropped\n",
                PAD_ANCHOR_MAX, an.text);
        return b;
    }
    if (a->armed > 0 && an.at < a->a[a->armed - 1].at) {
        /* Its open would have to come before one this run has seen. */
        fprintf(stderr, "  PAD: anchor f%lu:@open:%s is before one already seen;"
                " dropped\n", an.at, an.text);
        return b;
    }
    /* After the ones at the same flip: the record writes them in the order
     * the opens came, and they match in that order. */
    pos = a->n;
    while (pos > a->armed && a->a[pos - 1].at > an.at)
        pos--;
    memmove(&a->a[pos + 1], &a->a[pos], (size_t)(a->n - pos) * sizeof a->a[0]);
    a->a[pos] = an;
    a->n++;
    *added = 1;
    return b;
}

int pad_script_parse(const char *text, PadStep *steps, int len, int max)
{
    return pad_script_parse_anchored(text, steps, len, max, NULL);
}

int pad_script_parse_anchored(const char *text, PadStep *steps, int len,
                              int max, PadAnchors *anchors)
{
    const char *p = text;
    int first = len, added = 0;

    while (*p) {
        PadStep st;
        char *end;
        const char *b;

        while (*p == ',' || *p == ' ' || *p == '\r' || *p == '\n' || *p == '\t')
            p++;
        if (*p == '#') {
            while (*p && *p != '\n')
                p++;
            continue;
        }
        if (!*p)
            break;
        if (len >= max) {
            fprintf(stderr, "  PAD: script longer than %d steps; the rest is"
                    " dropped\n", max);
            break;
        }
        memset(&st, 0, sizeof st);
        if (*p == 'f' || *p == 'F') {
            st.by_flip = 1;
            p++;
        } else if (*p == 'g' || *p == 'G') {
            st.by_flip = 2;
            p++;
        }
        /* One clock a script: a poll and a flip are not the same count. */
        if (st.by_flip && anchors) {
            if (!anchors->clock) {
                anchors->clock = st.by_flip;
            } else if (anchors->clock != st.by_flip) {
                fprintf(stderr, "  PAD: %s step in a script on the %s clock;"
                        " skipped: at \"%.20s\"\n", st.by_flip == 2 ? "g<poll>" : "f<flip>",
                        anchors->clock == 2 ? "poll" : "flip", p - 1);
                p += strcspn(p, ", \t\r\n");
                continue;
            }
        }
        st.hold = st.by_flip ? PAD_HOLD_FLIPS_DEFAULT : 150;
        /* strtoul alone would take a sign or blanks, and "-1" would wrap
         * to a flip that never comes. */
        st.at = isdigit((unsigned char)*p) ? strtoul(p, &end, 10) : 0;
        if (!isdigit((unsigned char)*p) || *end != ':') {
            fprintf(stderr, "  PAD: script entry without <ms>, f<flip> or g<poll>:"
                    " at \"%.20s\"\n", p);
            break;
        }
        b = end + 1;
        /* "p2-" .. "p4-": the step is for that controller, not the first. */
        if ((b[0] == 'p' || b[0] == 'P') && b[1] >= '1' && b[1] <= '4'
            && b[2] == '-') {
            st.pad = (uint8_t)(b[1] - '1');
            b += 3;
        }
        if (*b == '@' && same_word(b, "@end", 4)
            && (!b[4] || strchr(", \t\r\n#", b[4]))) {
            /* The record's end: a step that presses nothing and ends on its
             * own count, so pad_quit waits for it. */
            if (!st.by_flip || st.pad) {
                fprintf(stderr, "  PAD: @end needs f<flip> or g<poll> and no pad"
                        " prefix: at \"%.20s\"\n", p);
                break;
            }
            st.end = 1;
            st.hold = 0;
            steps[len++] = st;
            p = b + 4;
            continue;
        }
        if (*b == '@') {
            if (st.pad) {
                /* An anchor is an event, for every pad: a pad goes on its
                 * retry (f10:@open:x:p2-a/30). */
                fprintf(stderr, "  PAD: anchor with a pad prefix (put it on the"
                        " retry): at \"%.20s\"\n", p);
                break;
            }
            if (!anchors) {
                fprintf(stderr, "  PAD: anchor \"%.20s\" where anchors are not"
                        " read (anchors only in RECOMP_PAD_SCRIPT, not live lines);"
                        " skipped\n", b);
                p = b + strcspn(b, ", \t\r\n");
                continue;
            }
            b = parse_anchor(b, &st, anchors, &added);
            if (!b)
                break;
            p = b;
            continue;
        }
        for (;;) {
            size_t n = strcspn(b, "+:,\r\n #");
            if (!button_bits(b, n, &st))
                fprintf(stderr, "  PAD: unknown button \"%.*s\"\n", (int)n, b);
            b += n;
            if (*b != '+')
                break;
            b++;
        }
        if (*b == ':') {
            if (!isdigit((unsigned char)b[1])) {
                /* "f10:a:" would be a hold of 0, a step that never presses. */
                fprintf(stderr, "  PAD: script hold without a number: at \"%.20s\"\n", b);
                break;
            }
            st.hold = strtoul(b + 1, &end, 10);
            b = end;
            if (*b == 'f' || *b == 'F') {
                if (!st.by_flip)
                    fprintf(stderr, "  PAD: hold in flips on a step timed in ms;"
                            " read as ms\n");
                b++;
            }
        }
        steps[len++] = st;
        p = b;
    }
    if (anchors)
        pad_anchors_assign(anchors, steps, added ? 0 : first, len);
    return len;
}

void pad_anchors_assign(const PadAnchors *a, PadStep *steps, int from, int len)
{
    int i, k;

    for (i = from; i < len; i++) {
        PadStep *st = &steps[i];
        st->anchor = 0;
        if (!a || !st->by_flip)
            continue;
        /* Sorted: the last one at or before the start. */
        for (k = a->n; k > 0; k--)
            if (a->a[k - 1].at <= st->at)
                break;
        st->anchor = (uint16_t)k;
    }
}

/* Flip, on the clock of anchor k (1-based; 0 is the raw count): flips since
 * the open, from the record's flip of it. 0 when its open has not come. */
static int anchor_clock(const PadAnchors *a, unsigned k, uint32_t flip,
                        uint32_t *out)
{
    const PadAnchor *an;
    long long f;

    if (!a || !k || k > (unsigned)a->n) {
        *out = flip;
        return 1;
    }
    an = &a->a[k - 1];
    /* Set by the thread that opened the file, after seen_flip. */
    if (!__atomic_load_n(&an->seen, __ATOMIC_ACQUIRE))
        return 0;
    f = (long long)flip - an->seen_flip + (long long)an->at;
    *out = f < 0 ? 0u : (uint32_t)f;
    return 1;
}

/* Give up anchors armed..upto-1: seen as if their clocks stood at `clock`
 * at this flip, so a step under one runs as if the record had reached
 * `clock`. The steps that leaves done unpressed are counted later, by
 * count_skipped on the pad thread: the open's thread must not read the
 * script, which the pad thread writes unlocked. */
static void give_up(PadAnchors *a, int upto, uint32_t flip, uint32_t clock)
{
    int i;

    for (i = a->armed; i < upto; i++) {
        PadAnchor *an = &a->a[i];
        an->seen_flip = (long long)flip - (long long)clock + (long long)an->at;
        an->skip_clock = clock;
        an->skip = 1;
        __atomic_store_n(&an->seen, 1, __ATOMIC_RELEASE);
    }
    a->armed = upto;
    a->skip = 1;
}

/* The steps under given-up anchors that their clock had passed when given
 * up: every given-up clock read skip_clock then, so a step is skipped when
 * it ended by it. On the pad thread, which owns the script. A live line's
 * compaction before the tick may drop such steps first: the logged count
 * is then low, nothing else. */
static int count_skipped(PadAnchors *a, const PadStep *steps, int nsteps)
{
    int i, n = 0;

    if (!a->skip)
        return 0;
    for (i = 0; i < nsteps; i++) {
        const PadStep *st = &steps[i];
        const PadAnchor *an;
        /* @end presses nothing: passing it skips no input. */
        if (st->end || !st->anchor || st->anchor > a->n || !st->by_flip)
            continue;
        an = &a->a[st->anchor - 1];
        if (an->skip && st->at + st->hold <= an->skip_clock)
            n++;
    }
    for (i = 0; i < a->n; i++)
        a->a[i].skip = 0;
    a->skip = 0;
    return n;
}

/* Into log[0..logn) when log is set, else stderr. A line cut or dropped
 * by a full buffer leaves "...\n" at its end. */
static void anchor_log(char *log, size_t logn, const char *fmt, ...)
{
    va_list ap;
    size_t used = log ? strlen(log) : 0;

    va_start(ap, fmt);
    if (log) {
        if (logn < 5) {
            /* Too small to say anything. */
        } else if (used + 1 >= logn) {
            memcpy(log + logn - 5, "...\n", 5);
        } else {
            int w = vsnprintf(log + used, logn - used, fmt, ap);
            if (w < 0 || (size_t)w >= logn - used)
                memcpy(log + logn - 5, "...\n", 5);
        }
    } else {
        vfprintf(stderr, fmt, ap);
        fflush(stderr);
    }
    va_end(ap);
}

/* The open, at clock f, of a file whose anchor was the last one seen (or
 * one seen on its record flip), matched to that name's later anchor j: the
 * recorder writes one anchor for a file opened twice in a row (a probe,
 * then the read; a movie that loops), so nearer the seen one it is that
 * one again. Nearer j it is j's own open, the anchors between missed. */
static int reopen(const PadAnchors *a, const char *path, uint32_t f, int j)
{
    int i = a->armed - 1;
    unsigned long at;

    if (i < 0)
        return 0;
    at = a->a[i].at;
    if ((unsigned long)f >= at + (a->a[j].at - at) / 2)
        return 0;
    for (; i >= 0 && a->a[i].at == at; i--)
        if (anchor_matches(path, a->a[i].text))
            return 1;
    return 0;
}

int pad_anchors_open(PadAnchors *a, const char *path, uint32_t flip, char *log,
                     size_t logn)
{
    PadAnchor *an;
    uint32_t f, f0;
    int k, j, late;

    if (log && logn)
        log[0] = 0;
    if (!a || !path)
        return -1;
    k = a->armed;
    if (k >= a->n)
        return -1;
    /* How far the open came from where the record had it, on the clock it
     * was due by: k (1-based) is the anchor before the armed one. */
    if (!anchor_clock(a, (unsigned)k, flip, &f))
        f = flip;
    j = k;
    if (!anchor_matches(path, a->a[k].text)) {
        /* Late, the armed one may never open (a file the cache serves): a
         * later anchor's open, within the wait the give-up allows, gives up
         * the ones before it. On time, only one on the armed one's record
         * flip may (two loader opens in the other order); later ones keep
         * the order, as cat's `wait open` consumes opens. */
        late = f >= a->a[k].at;
        for (j = k + 1; j < a->n; j++) {
            if (a->a[j].at > a->a[k].at + (late ? PAD_ANCHOR_GIVE_UP_FLIPS : 0)) {
                j = a->n;
                break;
            }
            if (anchor_matches(path, a->a[j].text))
                break;
        }
        /* Inside one record flip there is no re-open to tell: the
         * recorder collapses only against the previous name, so x, y, x
         * on one flip is three anchors and x, x, y takes them in turn. */
        if (j >= a->n || (a->a[j].at > a->a[k].at && reopen(a, path, f, j)))
            return -1;
        /* The game has reached anchor j's open: everything recorded before
         * it has passed. */
        f0 = f;
        if (f < a->a[j].at)
            f = (uint32_t)a->a[j].at;
        give_up(a, j, flip, f);
        if (a->a[j].at == a->a[k].at)
            anchor_log(log, logn, "  PAD: @open:%s taken before @open:%s (same record"
                       " flip)\n", a->a[j].text, a->a[k].text);
        else
            anchor_log(log, logn, "  PAD: gave up on %d anchor(s) from @open:%s at flip"
                       " %u: @open:%s came first; the steps it passed are skipped\n",
                       j - k, a->a[k].text, (unsigned)flip, a->a[j].text);
        f = f0;
    }
    an = &a->a[j];
    an->seen_flip = flip;
    __atomic_store_n(&an->seen, 1, __ATOMIC_RELEASE);
    a->armed = j + 1;
    anchor_log(log, logn, "  PAD: @open:%s at flip %u (%+ld flips against the record)\n",
               an->text, (unsigned)flip, (long)f - (long)an->at);
    return j;
}

int pad_anchors_tick(PadAnchors *a, const PadStep *steps, int nsteps, uint32_t flip,
                     char *log, size_t logn)
{
    int gave = 0, skipped;

    if (log && logn)
        log[0] = 0;
    if (!a)
        return 0;
    /* An open's give-up since the last tick. */
    if ((skipped = count_skipped(a, steps, nsteps)) > 0)
        anchor_log(log, logn, "  PAD: %d step(s) skipped by that give-up\n", skipped);
    while (a->armed < a->n) {
        PadAnchor *an = &a->a[a->armed];
        uint32_t f;
        int k = a->armed, j;
        char more[48];
        /* The clock it is due by (armed, read 1-based, is the one before). */
        if (!anchor_clock(a, (unsigned)k, flip, &f)
            || f < an->at + PAD_ANCHOR_GIVE_UP_FLIPS)
            break;
        /* Seen as if it came on time: its steps run on the clock it was due
         * by, the raw count when it is the first, as a record without
         * anchors would. Later anchors already due on that clock go with
         * it: their opens would have been taken during the wait, so they
         * did not come, and their retries must not all tap at once. */
        for (j = k + 1; j < a->n && a->a[j].at <= f; j++)
            ;
        give_up(a, j, flip, f);
        skipped = count_skipped(a, steps, nsteps);
        gave += j - k;
        if (j - k > 1)
            snprintf(more, sizeof more, ", and %d later anchor(s) it passed", j - k - 1);
        else
            more[0] = 0;
        anchor_log(log, logn, "  PAD: gave up on @open:%s at flip %u, %u flips late%s;"
                   " %d step(s) skipped, the rest on the record's clock\n", an->text,
                   (unsigned)flip, (unsigned)(f - an->at), more, skipped);
    }
    return gave;
}

int pad_step_active_at(const PadStep *st, const PadAnchors *a,
                       unsigned long t_ms, uint32_t flip)
{
    uint32_t f;
    if (!anchor_clock(a, st->anchor, flip, &f))
        return 0;
    return pad_step_active(st, t_ms, f);
}

int pad_step_done_at(const PadStep *st, const PadAnchors *a,
                     unsigned long t_ms, uint32_t flip)
{
    uint32_t f;
    if (!anchor_clock(a, st->anchor, flip, &f))
        return 0;
    return pad_step_done(st, t_ms, f);
}

int pad_anchor_retry(PadAnchors *a, int pad, uint32_t flip, PadStep *out,
                     char *log, size_t logn)
{
    PadAnchor *an;
    uint32_t f;
    unsigned long half, start;

    if (log && logn)
        log[0] = 0;
    if (!a || a->armed >= a->n)
        return 0;
    an = &a->a[a->armed];
    /* Late once the clock it is due by passes it: armed, read 1-based, is
     * the anchor before it. */
    if (!anchor_clock(a, (unsigned)a->armed, flip, &f) || f < an->at)
        return 0;
    if (!(an->logged & 1)) {
        an->logged |= 1;
        anchor_log(log, logn, "  PAD: waiting on @open:%s from flip %u%s\n", an->text,
                   (unsigned)flip, an->period ? ", tapping its retry" : "");
    }
    if (!an->period || an->retry.pad != pad)
        return 0;
    /* Half a period after the record's open, so the last recorded press
     * has a release before the first retry: a new edge. */
    half = an->period / 2;
    start = an->at + half;
    if (f < start || (f - start) % an->period >= half)
        return 0;
    /* A wrong guess taps at whatever screen is up: a few, then wait for
     * the open or the give-up. */
    if ((f - start) / an->period >= PAD_RETRY_MAX_TAPS) {
        if (!(an->logged & 2)) {
            an->logged |= 2;
            anchor_log(log, logn, "  PAD: @open:%s still late after %d retries; waiting\n",
                       an->text, PAD_RETRY_MAX_TAPS);
        }
        return 0;
    }
    *out = an->retry;
    return 1;
}

int pad_step_active(const PadStep *st, unsigned long t_ms, uint32_t flip)
{
    unsigned long now = st->by_flip ? (unsigned long)flip : t_ms;
    return now >= st->at && now < st->at + st->hold;
}

int pad_step_done(const PadStep *st, unsigned long t_ms, uint32_t flip)
{
    unsigned long now = st->by_flip ? (unsigned long)flip : t_ms;
    return now >= st->at + st->hold;
}

void pad_step_not_before(PadStep *st, uint32_t flip)
{
    if (st->by_flip && st->at < (unsigned long)flip)
        st->at = flip;
}

void pad_step_press(const PadStep *st, uint8_t *out)
{
    int j;

    out[2] |= st->digital;
    for (j = 0; j < 8; j++)
        if (st->analog & (1u << j))
            out[4 + j] = st->aval[j] ? st->aval[j] : 0xFF;
    if (st->stick) {
        /* Axes at 12..19: LX LY RX RY, signed 16-bit, up positive. */
        static const int16_t full = 32767;
        int16_t ax[4];
        memcpy(ax, &out[12], sizeof ax);
        if (st->stick & 0x01) ax[0] = (int16_t)-full;
        if (st->stick & 0x02) ax[0] = full;
        if (st->stick & 0x04) ax[1] = full;
        if (st->stick & 0x08) ax[1] = (int16_t)-full;
        if (st->stick & 0x10) ax[2] = (int16_t)-full;
        if (st->stick & 0x20) ax[2] = full;
        if (st->stick & 0x40) ax[3] = full;
        if (st->stick & 0x80) ax[3] = (int16_t)-full;
        memcpy(&out[12], ax, sizeof ax);
    }
    if (st->axis_set) {
        int16_t ax[4];
        memcpy(ax, &out[12], sizeof ax);
        for (j = 0; j < 4; j++)
            if (st->axis_set & (1u << j))
                ax[j] = st->axis[j];
        memcpy(&out[12], ax, sizeof ax);
    }
}

/* ---- the recorder ------------------------------------------------------ */

/* XINPUT_GAMEPAD_TRIGGER_THRESHOLD: below it a trigger or analog button is
 * resting under a finger, not pressed. */
#define ANALOG_PRESSED 30

/* The recorder's lines are built without snprintf: pad_record's SIGINT and
 * SIGTERM handler ends the record through pad_recorder_end and
 * pad_recorder_end_line, and POSIX does not list snprintf as
 * async-signal-safe (a C library may take the locale's lock or allocate in
 * it). strlen and memcpy it does list. A piece is built whole, then goes
 * into the line all or nothing, so a line that does not fit leaves no cut
 * text behind. */
typedef struct {
    char s[192];             /* a step's line: "g<u32>:p4-" + 127 + ":<u32>\n" */
    size_t len;
    int over;
} Piece;

static void pc_str(Piece *p, const char *s)
{
    size_t k = strlen(s);
    if (p->over || k >= sizeof p->s - p->len) {
        p->over = 1;
        return;
    }
    memcpy(p->s + p->len, s, k);
    p->len += k;
    p->s[p->len] = 0;
}

static void pc_char(Piece *p, char c)
{
    char one[2];
    one[0] = c;
    one[1] = 0;
    pc_str(p, one);
}

static void pc_uint(Piece *p, unsigned long v)
{
    char d[24];
    size_t i = sizeof d - 1;
    d[i] = 0;
    do {
        d[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v && i);
    pc_str(p, d + i);
}

static void pc_int(Piece *p, long v)
{
    if (v < 0) {
        pc_char(p, '-');
        pc_uint(p, 0ul - (unsigned long)v);
    } else {
        pc_uint(p, (unsigned long)v);
    }
}

/* The piece onto out at *used, whole or not at all: 1 when it went. */
static int pc_put(const Piece *p, char *out, size_t n, size_t *used)
{
    if (p->over || *used >= n || p->len >= n - *used)
        return 0;
    memcpy(out + *used, p->s, p->len + 1);
    *used += p->len;
    return 1;
}

PadBits pad_bits_from_report(const uint8_t *r)
{
    PadBits b;
    int j;

    memset(&b, 0, sizeof b);
    b.digital = r[2];
    for (j = 0; j < 8; j++)
        if (r[4 + j] > ANALOG_PRESSED)
            b.analog |= (uint8_t)(1u << j);
    memcpy(b.av, &r[4], sizeof b.av);
    memcpy(b.ax, &r[12], sizeof b.ax);
    return b;
}

/* The poll clock's exact form: an analog byte is pressed from 1, at its
 * depth. */
static PadBits exact_bits(PadBits b)
{
    int j;

    b.analog = 0;
    for (j = 0; j < 8; j++)
        if (b.av[j])
            b.analog |= (uint8_t)(1u << j);
    return b;
}

static void append(char *out, size_t n, size_t *used, const char *name)
{
    Piece p = {{0}, 0, 0};
    if (*used)
        pc_char(&p, '+');
    pc_str(&p, name);
    (void)pc_put(&p, out, n, used);
}

/* With depth, an analog button short of full is written name=<depth>. */
static void bits_name(PadBits b, int depth, char *out, size_t n)
{
    size_t i, used = 0;
    int j;

    if (n)
        out[0] = 0;
    for (i = 0; i < COUNT(s_buttons); i++) {
        if (s_buttons[i].dig & b.digital) {
            append(out, n, &used, s_buttons[i].name);
        } else if (s_buttons[i].ana & b.analog) {
            for (j = 0; j < 8; j++)
                if (s_buttons[i].ana == 1u << j)
                    break;
            if (depth && j < 8 && b.av[j] && b.av[j] < 255) {
                Piece p = {{0}, 0, 0};
                pc_str(&p, s_buttons[i].name);
                pc_char(&p, '=');
                pc_uint(&p, b.av[j]);
                append(out, n, &used, p.s);
            } else {
                append(out, n, &used, s_buttons[i].name);
            }
        }
    }
}

void pad_bits_name(PadBits b, char *out, size_t n)
{
    bits_name(b, 0, out, n);
}

static void axes_name(const int16_t *ax, char *out, size_t n)
{
    size_t used = 0;
    int i;

    if (n)
        out[0] = 0;
    for (i = 0; i < 4; i++) {
        Piece p = {{0}, 0, 0};
        if (!ax[i])
            continue;
        pc_str(&p, s_axes[i]);
        pc_char(&p, '=');
        pc_int(&p, ax[i]);
        append(out, n, &used, p.s);
    }
}

static int buttons_any(PadBits b)
{
    return b.digital || b.analog;
}

static int axes_any(const int16_t *ax)
{
    return ax[0] || ax[1] || ax[2] || ax[3];
}

/* A new stick step: past the band, or onto rest or full throw, which a
 * band would otherwise round away. Leaving rest takes the band too: a pad
 * at rest reads a few hundred off centre, inside any title's dead zone. */
static int axes_moved(const int16_t *cur, const int16_t *now, int exact)
{
    int i;
    for (i = 0; i < 4; i++) {
        int d = (int)now[i] - (int)cur[i];
        if (!d)
            continue;
        if (exact || d >= PAD_AXIS_BAND || d <= -PAD_AXIS_BAND || !now[i]
            || now[i] >= 32767 || now[i] <= -32767)
            return 1;
    }
    return 0;
}

static int emit(char *out, size_t n, size_t *used, int poll, int pad,
                uint32_t at, const char *names, uint32_t hold)
{
    Piece p = {{0}, 0, 0};

    pc_char(&p, poll ? 'g' : 'f');
    pc_uint(&p, at);
    pc_char(&p, ':');
    if (pad > 0) {
        pc_char(&p, 'p');
        pc_uint(&p, (unsigned long)pad + 1);
        pc_char(&p, '-');
    }
    pc_str(&p, names);
    pc_char(&p, ':');
    pc_uint(&p, hold);
    pc_char(&p, '\n');
    return pc_put(&p, out, n, used);
}

/* The state s holds from flip on: end what changed. */
static int commit(PadRecorder *r, PadBits s, uint32_t flip, char *out, size_t n,
                  size_t *used)
{
    char names[128];
    int lines = 0;

    int bits = s.digital != r->cur.digital || s.analog != r->cur.analog;

    if (bits || (r->poll && memcmp(s.av, r->cur.av, sizeof s.av))) {
        if (buttons_any(r->cur)) {
            bits_name(r->cur, r->poll, names, sizeof names);
            lines += emit(out, n, used, r->poll, r->pad, r->since, names,
                          flip - r->since);
        }
        r->cur.digital = s.digital;
        r->cur.analog = s.analog;
        memcpy(r->cur.av, s.av, sizeof s.av);
        r->since = flip;
        /* A press is new buttons: a trigger's depth wavering under a held
         * finger is not a tap, and would forge a retry and push real taps
         * out of the history. */
        if (bits && buttons_any(s)) {
            if (r->presses == PAD_RETRY_PRESSES) {
                memmove(&r->press[0], &r->press[1], (PAD_RETRY_PRESSES - 1) * sizeof r->press[0]);
                memmove(&r->press_at[0], &r->press_at[1],
                        (PAD_RETRY_PRESSES - 1) * sizeof r->press_at[0]);
                r->presses = PAD_RETRY_PRESSES - 1;
            }
            r->press[r->presses] = s;
            r->press_at[r->presses] = flip;
            r->presses++;
        }
    }
    if (axes_moved(r->cur.ax, s.ax, r->poll)) {
        if (axes_any(r->cur.ax)) {
            axes_name(r->cur.ax, names, sizeof names);
            lines += emit(out, n, used, r->poll, r->pad, r->ax_since, names,
                          flip - r->ax_since);
        }
        memcpy(r->cur.ax, s.ax, sizeof s.ax);
        r->ax_since = flip;
    }
    return lines;
}

int pad_recorder_feed(PadRecorder *r, PadBits b, uint32_t flip, char *out, size_t n)
{
    size_t used = 0;
    int lines = 0;

    if (n)
        out[0] = 0;
    if (r->poll)
        b = exact_bits(b);
    if (!r->started) {
        r->started = 1;
        r->presses = 0;
        memset(&r->cur, 0, sizeof r->cur);
        r->pend = b;
        r->pend_flip = flip;
        return 0;
    }
    if (flip == r->pend_flip) {
        int j;
        r->pend.digital |= b.digital;
        r->pend.analog |= b.analog;
        for (j = 0; j < 8; j++)
            if (b.av[j] > r->pend.av[j])
                r->pend.av[j] = b.av[j];
        memcpy(r->pend.ax, b.ax, sizeof b.ax);
        return 0;
    }
    lines = commit(r, r->pend, r->pend_flip, out, n, &used);
    r->pend = b;
    r->pend_flip = flip;
    return lines;
}

int pad_recorder_end(PadRecorder *r, uint32_t flip, char *out, size_t n)
{
    PadBits none;
    size_t used = 0;
    int lines;

    if (n)
        out[0] = 0;
    if (!r->started)
        return 0;
    memset(&none, 0, sizeof none);
    lines = commit(r, r->pend, r->pend_flip, out, n, &used);
    if (flip <= r->pend_flip)
        flip = r->pend_flip + 1;
    lines += commit(r, none, flip, out, n, &used);
    r->started = 0;
    return lines;
}

int pad_recorder_end_line(const PadRecorder *recs, int n, uint32_t flip,
                          char *out, size_t size)
{
    int i, any = 0, poll = 0;
    size_t used = 0;
    Piece p = {{0}, 0, 0};

    if (size)
        out[0] = 0;
    for (i = 0; i < n; i++) {
        if (!recs[i].started)
            continue;
        any = 1;
        poll = recs[i].poll;
        /* pad_recorder_end ends the held steps there at the earliest. */
        if (flip <= recs[i].pend_flip)
            flip = recs[i].pend_flip + 1;
    }
    if (!any)
        return 0;
    pc_char(&p, poll ? 'g' : 'f');
    pc_uint(&p, flip);
    pc_str(&p, ":@end\n");
    if (!pc_put(&p, out, size, &used))
        return 0;
    return (int)used;
}

const char *pad_path_name(const char *path)
{
    const char *name = path, *c;

    for (c = path; *c; c++)
        if (*c == '\\' || *c == '/' || *c == ':')
            name = c + 1;
    return name;
}

/* The flip under gathering starts a press: it holds buttons the steps
 * written so far do not. */
static int pending_press(const PadRecorder *r)
{
    return buttons_any(r->pend) && (r->pend.digital != r->cur.digital
                                    || r->pend.analog != r->cur.analog);
}

int pad_recorder_anchor(const PadRecorder *recs, int n, const char *path,
                        uint32_t flip, uint32_t since, char *out, size_t size,
                        uint32_t *at)
{
    const char *name = pad_path_name(path);
    size_t len = strlen(name);
    char retry[160], why[320];
    uint32_t a = flip;
    int i, w;

    if (size)
        out[0] = 0;
    retry[0] = why[0] = 0;
    if (!len || len >= PAD_ANCHOR_TEXT || strpbrk(name, ANCHOR_STOP))
        return 0;
    /* A press polled earlier in this flip than the open: the anchor's flip
     * would take it (ties count from the open), and it would wait for the
     * open it caused. */
    for (i = 0; i < n; i++)
        if (recs[i].started && recs[i].pend_flip == flip && pending_press(&recs[i]))
            a = flip + 1;
    for (i = 0; i < n && !retry[0]; i++) {
        const PadRecorder *r = &recs[i];
        PadBits b[PAD_RETRY_PRESSES + 1];
        uint32_t t[PAD_RETRY_PRESSES + 1], g1, g2;
        int m = 0, j;

        if (!r->started)
            continue;
        for (j = 0; j < r->presses; j++) {
            b[m] = r->press[j];
            t[m++] = r->press_at[j];
        }
        if (pending_press(r) && (!m || r->pend_flip > t[m - 1])) {
            b[m] = r->pend;
            t[m++] = r->pend_flip;
        }
        /* A tap until the open: the newest three presses, all after the
         * previous anchor, the same buttons at an even spacing, and the
         * open within that spacing of the last. Two menu choices with A
         * are not one. */
        if (m < 3 || t[m - 3] < since || t[m - 2] <= t[m - 3] || t[m - 1] <= t[m - 2])
            continue;
        g1 = t[m - 2] - t[m - 3];
        g2 = t[m - 1] - t[m - 2];
        for (j = m - 3; j < m - 1; j++)
            if (b[j].digital != b[m - 1].digital || b[j].analog != b[m - 1].analog)
                break;
        if (j < m - 1)
            continue;
        {
            char names[128], pfx[8] = "";
            pad_bits_name(b[m - 1], names, sizeof names);
            if (r->pad > 0)
                snprintf(pfx, sizeof pfx, "p%d-", r->pad + 1);
            if (g2 < 2 || g1 > PAD_RETRY_MAX_FLIPS || g2 > PAD_RETRY_MAX_FLIPS
                || (g1 > g2 ? g1 - g2 : g2 - g1) > PAD_RETRY_SLACK
                || flip - t[m - 1] > g2 + PAD_RETRY_SLACK) {
                /* The same buttons, but not a tap by the rule (a hand's
                 * uneven one, or the open a little late): say so, so a
                 * person can add the retry by hand. Presses more than a
                 * few spacings before the open are a menu walk, not a
                 * wait for it: no note. The first pad's note stands. */
                if (!why[0] && flip - t[m - 1] <= PAD_NO_RETRY_NEAR * (g1 > g2 ? g1 : g2))
                    snprintf(why, sizeof why, "# no retry %s%s: pressed at f%u f%u f%u"
                             " (gaps %u %u), the open %u flips after the last\n",
                             pfx, names, (unsigned)t[m - 3], (unsigned)t[m - 2],
                             (unsigned)t[m - 1], (unsigned)g1, (unsigned)g2,
                             (unsigned)(flip - t[m - 1]));
                continue;
            }
            snprintf(retry, sizeof retry, ":%s%s/%u", pfx, names, (unsigned)g2);
            /* Why, for a person reading the record: delete the retry when
             * the presses were not a tap. */
            snprintf(why, sizeof why, "# retry %s%s/%u: pressed at f%u f%u f%u,"
                     " the open %u flips after the last\n", pfx, names, (unsigned)g2,
                     (unsigned)t[m - 3], (unsigned)t[m - 2], (unsigned)t[m - 1],
                     (unsigned)(flip - t[m - 1]));
        }
    }
    w = snprintf(out, size, "%s%c%u:@open:%s%s\n", why,
                 n > 0 && recs[0].poll ? 'g' : 'f', (unsigned)a, name, retry);
    if (w <= 0 || (size_t)w >= size) {
        if (size)
            out[0] = 0;
        return -1;
    }
    if (at)
        *at = a;
    return w;
}
