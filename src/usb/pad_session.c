/*
 * pad_session.c -- a pad recording or playback started in the game
 * (pad_session.h; pad-recording-ui R2, R3, R5, R7).
 */
#include "pad_session.h"
#include "pad_script.h"
#include "pad_header.h"
#include "usb_gamepad.h"        /* USB_GAMEPAD_MAX */
#include "recomp_env.h"
#include "recomp_exit_hook.h"
#include "platform/xbox_winnt.h"
#include "../kernel/guest_time.h"   /* xbox_GuestTimeVirtual: deterministic = 1 */

#include <ctype.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <io.h>
#define ps_write _write
#define ps_lseek _lseek
#else
#include <unistd.h>
#define ps_write write
#define ps_lseek lseek
#endif

/* A recording or a setup is text at about 32 bytes a step: PAD_STEPS_MAX
 * steps fit in 4 MB, the import cap (R5). A larger file is not read. */
#define PAD_FILE_MAX (4L << 20)

/* ---- the pad lock -------------------------------------------------------- */

/* Opens come on the game's threads, any of them at once (a loader thread
 * and the main one), and the pad thread ticks the give-up, records and
 * swaps sessions: an anchor matched twice, or lost, would hang a replay.
 * usb_gamepad.c takes it only with a pad script, a record or a session. */
static SRWLOCK s_pad_lock = SRWLOCK_INIT;

void pad_lock(void) { AcquireSRWLockExclusive(&s_pad_lock); }
void pad_unlock(void) { ReleaseSRWLockExclusive(&s_pad_lock); }

int pad_anchor_wanted(const char *path)
{
    const char *list = recomp_env(RENV_PAD_ANCHOR), *p;

    if (!list || !*list)
        return 0;
    for (p = list; *p; ) {
        size_t n = strcspn(p, "|"), i, j;
        for (i = 0; n && path[i]; i++) {
            for (j = 0; j < n && path[i + j]; j++)
                if (tolower((unsigned char)path[i + j]) != tolower((unsigned char)p[j]))
                    break;
            if (j == n)
                return 1;
        }
        p += n;
        if (*p == '|')
            p++;
    }
    return 0;
}

/* ---- files --------------------------------------------------------------- */

/* A UTF-8 path, as the menu's browser gives it: on Windows through _wfopen,
 * so a user directory under a non-ASCII name opens (R5). The file browser's
 * fb_read_file does the same, but it is built only with the enhancements
 * and this is not. */
static FILE *utf8_fopen(const char *path, const char *mode)
{
#ifdef _WIN32
    wchar_t wp[1024], wm[8];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, 1024)
        || !MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8))
        return NULL;
    return _wfopen(wp, wm);
#else
    return fopen(path, mode);
#endif
}

static void utf8_remove(const char *path)
{
#ifdef _WIN32
    wchar_t wp[1024];
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wp, 1024))
        _wremove(wp);
#else
    remove(path);
#endif
}

static char *read_text(const char *path, char *why, size_t whyn)
{
    FILE *f = utf8_fopen(path, "rb");
    char *buf;
    long n;

    if (!f) {
        snprintf(why, whyn, "cannot open %s", pad_path_name(path));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        snprintf(why, whyn, "cannot read %s", pad_path_name(path));
        return NULL;
    }
    if (n > PAD_FILE_MAX) {
        fclose(f);
        snprintf(why, whyn, "%s is over 4 MB", pad_path_name(path));
        return NULL;
    }
    buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        snprintf(why, whyn, "out of memory");
        return NULL;
    }
    n = (long)fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[n] = 0;
    return buf;
}

/* ---- the session --------------------------------------------------------- */

enum { KIND_REC = 1, KIND_PLAY = 2 };

typedef struct PadSession {
    int kind;
    uint32_t serial;
    char name[PADREC_NAME_MAX];
    int clock;                  /* 1 flip, 2 poll, 0 milliseconds */
    uint32_t origin;            /* the clock's count at the first poll */
    unsigned long ms0;          /* pad ms then, for millisecond steps */
    DWORD host0;                /* host ms then, for the strip's rate */
    int ended;                  /* retire at the next poll */
    int end_state;
    char warn[PADREC_WARN_MAX];

    /* a playback */
    PadStep *steps;
    int nsteps;
    PadAnchors *anchors;
    uint32_t pads;
    uint32_t total;
    int stepped, from;
    char want_open[PAD_ANCHOR_TEXT];   /* the record's start_open */

    /* a record */
    FILE *f;                    /* NULL once its end is written */
    char *path;                 /* removed when it never started */
    PadHeader *hdr;             /* written at the first poll, then freed */
    PadRecorder rec[USB_GAMEPAD_MAX];
    long len_off;               /* where the length value sits */
    int lines;
    int full;
    uint32_t anchor_at;
    char anchor_name[PAD_ANCHOR_TEXT];
} PadSession;

static void session_free(PadSession *s)
{
    if (!s)
        return;
    if (s->f)
        fclose(s->f);
    free(s->path);
    free(s->steps);
    free(s->anchors);
    free(s->hdr);
    free(s);
}

/* Published by the menu thread, taken by the pad thread with an exchange:
 * whoever exchanges it out owns it. */
static PadSession *s_pending;
/* The pad thread's, changed only under the pad lock. */
static PadSession *s_active;
/* Freed at the next retire, under the pad lock: a file open that held the
 * active session under the lock has let it go by then. */
static PadSession *s_retired;
/* A pending session waits while this is set (ARMED, the menu is open). */
static int s_hold;
static int s_stop;
/* Any session was ever published: the pad thread's fast path. */
static int s_ever;
/* Flags others read without the lock and without touching a session. */
static int s_live, s_playing;
static uint32_t s_play_pads, s_pending_pads;
static uint32_t s_serial;
/* The record the signal handler ends, and its length offset. */
static PadSession *s_sig_rec;

/* The last poll padrec_sync ran for. */
static int s_synced;
static uint32_t s_sync_poll;

/* The clock key from config.toml (padrec.clock), set by the menu. */
static const char *s_cfg_clock;

void padrec_set_clock_key(const char *v) { s_cfg_clock = v; }

/* ---- the status seqlock -------------------------------------------------- */

/* Copied word by word with relaxed atomics: the page and the strip read a
 * whole copy or retry, and a sanitiser sees no plain racing access. Two
 * writers (the pad thread and the menu thread publishing ARMED) take
 * s_status_w between them. */
static uint32_t s_status_seq;
static uint32_t s_status_words[sizeof(PadrecStatus) / 4];
static SRWLOCK s_status_w = SRWLOCK_INIT;

typedef char status_is_words[sizeof(PadrecStatus) % 4 == 0 ? 1 : -1];

static void status_put(const PadrecStatus *st)
{
    const uint32_t *w = (const uint32_t *)(const void *)st;
    size_t i;

    AcquireSRWLockExclusive(&s_status_w);
    __atomic_store_n(&s_status_seq, s_status_seq + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    for (i = 0; i < sizeof s_status_words / 4; i++)
        __atomic_store_n(&s_status_words[i], w[i], __ATOMIC_RELAXED);
    __atomic_store_n(&s_status_seq, s_status_seq + 1, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&s_status_w);
}

void padrec_status(PadrecStatus *out)
{
    uint32_t *w = (uint32_t *)(void *)out;
    uint32_t a, b;
    size_t i;

    do {
        while ((a = __atomic_load_n(&s_status_seq, __ATOMIC_ACQUIRE)) & 1)
            SwitchToThread();
        for (i = 0; i < sizeof s_status_words / 4; i++)
            w[i] = __atomic_load_n(&s_status_words[i], __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&s_status_seq, __ATOMIC_RELAXED);
    } while (a != b);
}

static void status_of(const PadSession *s, int state, uint32_t count, PadrecStatus *st)
{
    memset(st, 0, sizeof *st);
    st->serial = s->serial;
    st->state = state;
    st->clock = s->clock;
    st->count = count;
    st->total = s->total;
    st->ms = s->host0 ? (uint32_t)(GetTickCount() - s->host0) : 0;
    st->step = s->stepped;
    st->nsteps = s->nsteps;
    st->full = s->full;
    snprintf(st->name, sizeof st->name, "%s", s->name);
    snprintf(st->warn, sizeof st->warn, "%s", s->warn);
}

/* ---- the last pad_anchor open (start_open) -------------------------------- */

/* Written by any thread that opens a file, read by the menu when a record
 * starts: a sequence count and a fixed buffer, never the pad lock, since a
 * stock run with pad_anchor set pays this on every matching open (R2).
 * Writers take the odd count with a compare-exchange, so two loaders
 * opening at once write one after the other. */
static uint32_t s_open_seq;
static char s_open_name[PAD_ANCHOR_TEXT];

void padrec_open_name(const char *path)
{
    const char *name;
    uint32_t s;
    size_t i, n;

    if (!pad_anchor_wanted(path))
        return;
    name = pad_path_name(path);
    for (;;) {
        s = __atomic_load_n(&s_open_seq, __ATOMIC_RELAXED);
        if (!(s & 1) && __atomic_compare_exchange_n(&s_open_seq, &s, s + 1, 0,
                                                    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
            break;
        SwitchToThread();
    }
    __atomic_thread_fence(__ATOMIC_RELEASE);
    n = strlen(name);
    if (n >= sizeof s_open_name)
        n = sizeof s_open_name - 1;
    for (i = 0; i < n; i++)
        __atomic_store_n(&s_open_name[i], name[i], __ATOMIC_RELAXED);
    __atomic_store_n(&s_open_name[n], 0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_open_seq, s + 2, __ATOMIC_RELEASE);
}

void padrec_last_open(char *out, size_t n)
{
    uint32_t a, b;
    size_t i;

    if (!n)
        return;
    do {
        while ((a = __atomic_load_n(&s_open_seq, __ATOMIC_ACQUIRE)) & 1)
            SwitchToThread();
        for (i = 0; i + 1 < n && i < sizeof s_open_name; i++)
            if (!(out[i] = __atomic_load_n(&s_open_name[i], __ATOMIC_RELAXED)))
                break;
        out[i < n ? i : n - 1] = 0;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        b = __atomic_load_n(&s_open_seq, __ATOMIC_RELAXED);
    } while (a != b);
}

/* ---- the env keys win ---------------------------------------------------- */

const char *padrec_env_driven(void)
{
    static const struct { recomp_env_id id; const char *name; } keys[] = {
        { RENV_PAD_SCRIPT, "RECOMP_PAD_SCRIPT" },
        { RENV_PAD_LIVE, "RECOMP_PAD_LIVE" },
        { RENV_PAD_PRESS, "RECOMP_PAD_PRESS" },
        { RENV_PAD_RECORD, "RECOMP_PAD_RECORD" },
    };
    size_t i;
    for (i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = recomp_env(keys[i].id);
        if (v && *v)
            return keys[i].name;
    }
    return NULL;
}

/* ---- the clock ------------------------------------------------------------- */

const char *padrec_clock_pick(int game_tap, uint32_t flips, const char **why)
{
    /* The key is an enhancements row: sessions start from the menu or an
     * action, so a build without the layer never gets here with one. */
#ifdef RECOMP_ENV_HAVE_ENHANCE_KEYS
    const char *key = s_cfg_clock ? s_cfg_clock : recomp_env(RENV_PADREC_CLOCK);
#else
    const char *key = s_cfg_clock;
#endif
    const char *game = recomp_env(RENV_PAD_CLOCK);

    if (key && (!strcmp(key, "poll") || !strcmp(key, "flip"))) {
        *why = "padrec.clock";
        return key;
    }
    /* The game's pad_clock default: BLiNX 2 polls once a frame but not in
     * step with the walker, so its records are exact only on polls. */
    if (game && (!strcmp(game, "poll") || !strcmp(game, "flip"))) {
        *why = "the game's pad_clock";
        return game;
    }
    /* On the game's own tap a poll is a title frame and stops in loads:
     * the closest clock to what the title saw. */
    if (game_tap) {
        *why = "the game's own pad poll";
        return "poll";
    }
    /* On the USB model a poll is 4 ms of host time; a flip follows the
     * title's frames. The HLE present path counts no flips. */
    if (flips) {
        *why = "the USB model, flips counted";
        return "flip";
    }
    *why = "the USB model, no flips counted";
    return "poll";
}

static uint32_t clock_now(const PadSession *s, const PadrecClocks *c)
{
    return s->clock == 2 ? c->polls : s->clock == 1 ? c->flip : 0;
}

static uint32_t rel_now(const PadSession *s, const PadrecClocks *c)
{
    return clock_now(s, c) - s->origin;
}

/* ---- a record's end ---------------------------------------------------------- */

/* The length placeholder is 10 spaces wide: a uint32 count fits, and a
 * TOML reader takes the trailing spaces as whitespace. */
#define LEN_WIDTH 10

static int format_len(char *out, uint32_t v)
{
    char d[16];
    int n = 0, i;
    do {
        d[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    for (i = 0; i < n; i++)
        out[i] = d[n - 1 - i];
    for (; i < LEN_WIDTH; i++)
        out[i] = ' ';
    return LEN_WIDTH;
}

/* Under the pad lock, on the pad thread or at exit. */
static void record_end(PadSession *s, uint32_t rel)
{
    char lines[512], end[32], len[LEN_WIDTH];
    FILE *f = __atomic_exchange_n(&s->f, NULL, __ATOMIC_ACQ_REL);
    int i, w;

    if (!f)
        return;
    __atomic_store_n(&s_sig_rec, NULL, __ATOMIC_RELEASE);
    /* Before pad_recorder_end, which forgets the pads' last reports. */
    w = pad_recorder_end_line(s->rec, USB_GAMEPAD_MAX, rel, end, sizeof end);
    for (i = 0; i < USB_GAMEPAD_MAX; i++)
        if (pad_recorder_end(&s->rec[i], rel, lines, sizeof lines))
            fputs(lines, f);
    if (w > 0)
        fputs(end, f);
    if (s->len_off > 0 && fflush(f) == 0 && fseek(f, s->len_off, SEEK_SET) == 0) {
        format_len(len, rel);
        fwrite(len, 1, LEN_WIDTH, f);
    }
    fclose(f);
    s->total = rel;
    fprintf(stderr, "[PADREC] record %s ended at %s %u\n", s->name,
            s->clock == 2 ? "poll" : "flip", (unsigned)rel);
    fflush(stderr);
}

/* SIGINT and SIGTERM never reach atexit; a record stopped that way still
 * gets its held steps and @end, as the env record does (usb_gamepad.c's
 * pad_record_signal): write(2) only, no lock, then the old handler. */
typedef void (*ps_sig_fn)(int);
static void pending_drop(PadSession *s);
static ps_sig_fn s_old_int = SIG_DFL, s_old_term = SIG_DFL;
static int s_sig_installed;
static PadrecClocks s_sig_clock;   /* the last poll's, for the end's count */

static void record_signal(int sig)
{
    ps_sig_fn old = sig == SIGINT ? s_old_int : s_old_term;
    PadSession *s = __atomic_exchange_n(&s_sig_rec, NULL, __ATOMIC_ACQ_REL);
    FILE *f = s ? __atomic_exchange_n(&s->f, NULL, __ATOMIC_ACQ_REL) : NULL;
    PadSession *p = __atomic_exchange_n(&s_pending, NULL, __ATOMIC_ACQ_REL);

    /* An armed record that never started holds no steps: its empty file
     * would be listed, then refused. Leaked, not freed: no free() here. */
    if (p && p->kind == KIND_REC && p->path) {
#ifdef _WIN32
        utf8_remove(p->path);
#else
        unlink(p->path);
#endif
    }
    if (f) {
        char lines[512], end[32], len[LEN_WIDTH];
        uint32_t rel = rel_now(s, &s_sig_clock);
        int i, fd = fileno(f), w;
        w = pad_recorder_end_line(s->rec, USB_GAMEPAD_MAX, rel, end, sizeof end);
        for (i = 0; i < USB_GAMEPAD_MAX; i++)
            if (pad_recorder_end(&s->rec[i], rel, lines, sizeof lines))
                (void)!ps_write(fd, lines, (unsigned)strlen(lines));
        if (w > 0)
            (void)!ps_write(fd, end, (unsigned)w);
        if (s->len_off > 0 && ps_lseek(fd, s->len_off, SEEK_SET) == s->len_off)
            (void)!ps_write(fd, len, (unsigned)format_len(len, rel));
    }
    signal(sig, old == SIG_ERR ? SIG_DFL : old);
    raise(sig);
}

static void padrec_exit(void)
{
    PadSession *s;
    /* An armed record that never reached the pad thread: its file holds
     * nothing, so it goes rather than stay listed and be refused. The
     * exchange needs no lock. */
    pending_drop(__atomic_exchange_n(&s_pending, NULL, __ATOMIC_ACQ_REL));
    /* No lock once the record has ended: a handler chained after ours may
     * exit() on a thread that holds it, and it is not recursive. */
    if (!__atomic_load_n(&s_sig_rec, __ATOMIC_ACQUIRE))
        return;
    pad_lock();
    s = s_active;
    if (s && s->kind == KIND_REC)
        record_end(s, rel_now(s, &s_sig_clock));
    pad_unlock();
}

static void padrec_exit_hook(const char *why)
{
    (void)why;
    padrec_exit();
}

static void install_exit(void)
{
    if (s_sig_installed)
        return;
    s_sig_installed = 1;
    atexit(padrec_exit);
    recomp_exit_hook_add(padrec_exit_hook);
    s_old_int = signal(SIGINT, record_signal);
    s_old_term = signal(SIGTERM, record_signal);
}

/* ---- the swap ------------------------------------------------------------------ */

static void retire(PadSession *s)
{
    session_free(s_retired);
    s_retired = s;
}

static void end_active(int state, const PadrecClocks *c)
{
    PadSession *s = s_active;
    PadrecStatus st;

    if (!s)
        return;
    if (s->kind == KIND_REC)
        record_end(s, rel_now(s, c));
    status_of(s, state, rel_now(s, c), &st);
    st.end_ms = (uint32_t)GetTickCount();
    status_put(&st);
    __atomic_store_n(&s_active, NULL, __ATOMIC_RELEASE);
    __atomic_store_n(&s_live, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_playing, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_play_pads, 0, __ATOMIC_RELEASE);
    retire(s);
}

static void record_begin(PadSession *s, const PadrecClocks *c)
{
    const char *why, *pick = padrec_clock_pick(c->game_tap, c->flip, &why);
    char *buf = malloc(64 * 1024), open[PAD_ANCHOR_TEXT];
    int i, n;

    s->clock = !strcmp(pick, "poll") ? 2 : 1;
    fprintf(stderr, "[PADREC] record %s: clock %s (%s)\n", s->name, pick, why);
    fflush(stderr);
    pad_header_set_text(s->hdr, "clock", pick);
    pad_header_set_text(s->hdr, "origin", "session");
    padrec_last_open(open, sizeof open);
    if (open[0])
        pad_header_set_text(s->hdr, "start_open", open);
    for (i = 0; i < USB_GAMEPAD_MAX; i++) {
        s->rec[i].pad = i;
        s->rec[i].poll = s->clock == 2;
    }
    n = buf ? pad_header_write(s->hdr, buf, 64 * 1024) : -1;
    if (n > 0) {
        fwrite(buf, 1, (size_t)n, s->f);
        /* The length last, patched at the end: the record's last count. */
        fputs("# xbr-pad: length = ", s->f);
        s->len_off = ftell(s->f);
        fprintf(s->f, "%-*s\n", LEN_WIDTH, "0");
    }
    fprintf(s->f, s->clock == 2
            ? "# pad_record: pad_script steps timed by the title's pad polls"
              " (g<poll>:<buttons>:<hold polls>, sticks lx=<v>, analog depth lt=<v>)\n"
            : "# pad_record: pad_script steps timed by flip"
              " (f<flip>:<buttons>:<hold flips>, sticks lx=<v>)\n");
    fflush(s->f);
    free(buf);
    free(s->hdr);
    s->hdr = NULL;
    __atomic_store_n(&s_sig_rec, s, __ATOMIC_RELEASE);
}

static void activate(PadSession *s, const PadrecClocks *c)
{
    PadrecStatus st;

    s->host0 = GetTickCount();
    s->ms0 = c->ms;
    if (s->kind == KIND_REC)
        record_begin(s, c);
    s->origin = clock_now(s, c);
    if (s->kind == KIND_PLAY) {
        char open[PAD_ANCHOR_TEXT];
        padrec_last_open(open, sizeof open);
        if (s->want_open[0] && open[0] && strcmp(open, s->want_open))
            snprintf(s->warn, sizeof s->warn, "recorded after %s, this run last opened %s",
                     s->want_open, open);
        fprintf(stderr, "[PADREC] play %s: %d step(s), %d anchor(s), %s clock from %u\n",
                s->name, s->nsteps, s->anchors->n,
                s->clock == 2 ? "poll" : s->clock == 1 ? "flip" : "ms",
                (unsigned)s->origin);
        fflush(stderr);
    }
    __atomic_store_n(&s_active, s, __ATOMIC_RELEASE);
    __atomic_store_n(&s_live, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&s_play_pads, s->kind == KIND_PLAY ? s->pads : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_playing, s->kind == KIND_PLAY, __ATOMIC_RELEASE);
    status_of(s, s->kind == KIND_REC ? PADREC_REC : PADREC_PLAY, 0, &st);
    status_put(&st);
}

void padrec_sync(const PadrecClocks *c)
{
    PadSession *s, *p;

    if (!__atomic_load_n(&s_ever, __ATOMIC_ACQUIRE))
        return;
    if (s_synced && s_sync_poll == c->polls)
        return;
    s_synced = 1;
    s_sync_poll = c->polls;
    s_sig_clock = *c;
    pad_lock();
    if (__atomic_exchange_n(&s_stop, 0, __ATOMIC_ACQ_REL))
        end_active(PADREC_STOPPED, c);
    s = s_active;
    if (s && __atomic_load_n(&s->ended, __ATOMIC_ACQUIRE))
        end_active(s->end_state, c);
    if (!__atomic_load_n(&s_hold, __ATOMIC_ACQUIRE)
        && (p = __atomic_exchange_n(&s_pending, NULL, __ATOMIC_ACQ_REL))) {
        __atomic_store_n(&s_pending_pads, 0, __ATOMIC_RELEASE);
        end_active(PADREC_STOPPED, c);
        activate(p, c);
    }
    s = s_active;
    if (s) {
        PadrecStatus st;
        int state = s->kind == KIND_REC ? PADREC_REC : PADREC_PLAY;
        if (s->kind == KIND_PLAY && s->anchors->armed < s->anchors->n) {
            const PadAnchor *an = &s->anchors->a[s->anchors->armed];
            if ((an->logged & 1) && !an->seen) {
                state = PADREC_WAIT;
                snprintf(s->warn, sizeof s->warn, "waiting for %s", an->text);
            }
        }
        status_of(s, state, rel_now(s, c), &st);
        status_put(&st);
    }
    pad_unlock();
}

/* ---- playback and record on the pad thread --------------------------------- */

void padrec_press(int pad, uint8_t *report, const PadrecClocks *c)
{
    PadSession *s = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    uint32_t rel;
    unsigned long t;
    int i, k;

    if (!s || s->kind != KIND_PLAY || pad < 0 || pad >= USB_GAMEPAD_MAX
        || !(s->pads & (1u << pad)) || __atomic_load_n(&s->ended, __ATOMIC_ACQUIRE))
        return;
    /* The host is out of the replay (R3): the menu and the actions read it
     * before this, the title only after. */
    memset(report + 2, 0, 18);
    rel = rel_now(s, c);
    t = c->ms - s->ms0;
    for (i = 0; i < s->nsteps; i++) {
        PadStep *st = &s->steps[i];
        if (st->pad != pad || !pad_step_active_at(st, s->anchors, t, rel))
            continue;
        if (!st->announced) {
            st->announced = 1;
            s->stepped++;
        }
        pad_step_press(st, report);
    }
    if (s->anchors->n) {
        PadStep retry;
        int on;
        char log[512], rlog[160];
        pad_lock();
        pad_anchors_tick(s->anchors, s->steps, s->nsteps, rel, log, sizeof log);
        on = pad_anchor_retry(s->anchors, pad, rel, &retry, rlog, sizeof rlog);
        pad_unlock();
        if (log[0] || rlog[0]) {
            fputs(log, stderr);
            fputs(rlog, stderr);
            fflush(stderr);
        }
        if (on)
            pad_step_press(&retry, report);
    }
    if (pad != 0)
        return;
    /* A step once ended stays ended: the scan goes on from the last one
     * still running, one check a poll (as pad_quit does). */
    for (k = 0; k < s->nsteps; k++) {
        i = (s->from + k) % s->nsteps;
        if (!pad_step_done_at(&s->steps[i], s->anchors, t, rel)) {
            s->from = i;
            return;
        }
    }
    s->end_state = PADREC_ENDED;
    __atomic_store_n(&s->ended, 1, __ATOMIC_RELEASE);
    fprintf(stderr, "[PADREC] play %s ended (%s %u)\n", s->name,
            s->clock == 2 ? "poll" : s->clock == 1 ? "flip" : "ms",
            (unsigned)(s->clock ? rel : (uint32_t)t));
    fflush(stderr);
}

void padrec_feed(int pad, const uint8_t *report, const PadrecClocks *c)
{
    PadSession *s = __atomic_load_n(&s_active, __ATOMIC_ACQUIRE);
    char lines[512];
    uint32_t rel;
    FILE *f;
    int w;

    if (!s || s->kind != KIND_REC || pad < 0 || pad >= USB_GAMEPAD_MAX
        || !__atomic_load_n(&s->f, __ATOMIC_ACQUIRE))
        return;
    rel = rel_now(s, c);
    pad_lock();
    /* Loaded once: record_signal may take s->f on this very thread between
     * a check and a use (as in pad_record); the FILE itself stays open. */
    if ((f = __atomic_load_n(&s->f, __ATOMIC_ACQUIRE)) != NULL) {
        w = pad_recorder_feed(&s->rec[pad], pad_bits_from_report(report), rel, lines,
                              sizeof lines);
        if (w > 0) {
            const char *p;
            fputs(lines, f);
            fflush(f);
            for (p = lines; (p = strchr(p, '\n')); p++)
                s->lines++;
        }
        /* One cap for record and replay (R5): the record stops with room
         * for its held steps and @end, so it plays back whole. */
        if (s->lines >= PAD_STEPS_MAX - 4 * USB_GAMEPAD_MAX - 8) {
            s->full = 1;
            snprintf(s->warn, sizeof s->warn, "recording full");
            record_end(s, rel);
            s->end_state = PADREC_STOPPED;
            __atomic_store_n(&s->ended, 1, __ATOMIC_RELEASE);
        }
    }
    pad_unlock();
}

uint32_t padrec_pads(void)
{
    return __atomic_load_n(&s_play_pads, __ATOMIC_ACQUIRE)
         | __atomic_load_n(&s_pending_pads, __ATOMIC_ACQUIRE);
}

int padrec_any(void)
{
    return __atomic_load_n(&s_ever, __ATOMIC_ACQUIRE);
}

int padrec_playing(void)
{
    return __atomic_load_n(&s_playing, __ATOMIC_ACQUIRE);
}

int padrec_wants_opens(void)
{
    static int anchor = -1;
    int a = __atomic_load_n(&anchor, __ATOMIC_RELAXED);
    if (a < 0) {
        const char *v = recomp_env(RENV_PAD_ANCHOR);
        a = v && *v;
        __atomic_store_n(&anchor, a, __ATOMIC_RELAXED);
    }
    return a || __atomic_load_n(&s_live, __ATOMIC_ACQUIRE);
}

/* Under the pad lock: an open on the record's anchor texts writes its line
 * in order with the steps around it (as usb_gamepad.c's pad_record_open). */
void padrec_file_open(const char *path, const PadrecClocks *c, char *log, size_t logn)
{
    PadSession *s = s_active;
    char line[512];
    const char *name;
    uint32_t at;
    FILE *f;
    int w;

    if (!s || __atomic_load_n(&s->ended, __ATOMIC_ACQUIRE))
        return;
    if (s->kind == KIND_PLAY) {
        if (s->anchors->n)
            pad_anchors_open(s->anchors, path, rel_now(s, c), log, logn);
        return;
    }
    /* Loaded once, as in padrec_feed: record_signal may take it. */
    if (!(f = __atomic_load_n(&s->f, __ATOMIC_ACQUIRE)) || !pad_anchor_wanted(path))
        return;
    name = pad_path_name(path);
    if (s->anchor_name[0] && !strcmp(name, s->anchor_name))
        return;
    w = pad_recorder_anchor(s->rec, USB_GAMEPAD_MAX, path, rel_now(s, c), s->anchor_at,
                            line, sizeof line, &at);
    if (w <= 0) {
        size_t used = strlen(log);
        if (used + 1 < logn)
            snprintf(log + used, logn - used,
                     w ? "  PAD: %s: its anchor line is too long; not recorded\n"
                       : "  PAD: %s cannot be an anchor (its name)\n", path);
        return;
    }
    s->anchor_at = at;
    snprintf(s->anchor_name, sizeof s->anchor_name, "%s", name);
    fputs(line, f);
    fflush(f);
    s->lines++;
}

/* ---- the menu thread ---------------------------------------------------------- */

/* A pending session that never reached the pad thread: a record's file
 * holds nothing yet, so it goes too (an ARMED record cancelled, or one
 * replaced before the menu closed). */
static void pending_drop(PadSession *s)
{
    if (s && s->kind == KIND_REC && s->f && s->path) {
        fclose(s->f);
        s->f = NULL;
        utf8_remove(s->path);
    }
    session_free(s);
}

static void publish(PadSession *s, unsigned flags, uint32_t pads)
{
    PadSession *old;
    PadrecStatus st;

    s->serial = __atomic_add_fetch(&s_serial, 1, __ATOMIC_RELAXED);
    if (flags & PADREC_ARMED) {
        status_of(s, s->kind == KIND_REC ? PADREC_ARMED_REC : PADREC_ARMED_PLAY, 0, &st);
        status_put(&st);
    }
    __atomic_store_n(&s_hold, (flags & PADREC_ARMED) ? 1 : 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_pending_pads, pads, __ATOMIC_RELEASE);
    old = __atomic_exchange_n(&s_pending, s, __ATOMIC_ACQ_REL);
    /* Never seen by the pad thread: it takes a pending session only by
     * exchanging it out, so this one is ours alone. */
    pending_drop(old);
    __atomic_store_n(&s_ever, 1, __ATOMIC_RELEASE);
}

static void name_of(char *out, size_t n, const char *path)
{
    const char *b = pad_path_name(path), *dot = strrchr(b, '.');
    size_t len = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (len >= n)
        len = n - 1;
    memcpy(out, b, len);
    out[len] = 0;
}

static const char *host_os(void)
{
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

int padrec_record_start(const char *path, const PadrecMeta *m, unsigned flags,
                        char *why, size_t whyn)
{
    const char *env = padrec_env_driven();
    PadSession *s;
    char stamp[32];
    time_t now = time(NULL);
    struct tm tm;

    if (env) {
        snprintf(why, whyn, "driven by %s", env);
        return -1;
    }
    s = calloc(1, sizeof *s);
    if (!s || !(s->hdr = calloc(1, sizeof *s->hdr))) {
        session_free(s);
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    s->kind = KIND_REC;
    s->f = utf8_fopen(path, "wb");
    if ((s->path = malloc(strlen(path) + 1)) != NULL)
        memcpy(s->path, path, strlen(path) + 1);
    if (!s->f || !s->path) {
        session_free(s);
        snprintf(why, whyn, "cannot write %s", pad_path_name(path));
        return -1;
    }
    name_of(s->name, sizeof s->name, path);
    pad_header_init(s->hdr);
    if (m) {
        if (m->game && *m->game)
            pad_header_set_text(s->hdr, "game", m->game);
        if (m->title_id) {
            char id[16];
            snprintf(id, sizeof id, "%08x", (unsigned)m->title_id);
            pad_header_set_text(s->hdr, "title_id", id);
        }
        if (m->title && *m->title)
            pad_header_set_text(s->hdr, "title", m->title);
        if (m->toolkit && *m->toolkit)
            pad_header_set_text(s->hdr, "toolkit", m->toolkit);
        if (m->game_build && *m->game_build)
            pad_header_set_text(s->hdr, "game_build", m->game_build);
        if (m->start_point && *m->start_point)
            pad_header_set_text(s->hdr, "start_point", m->start_point);
    }
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    strftime(stamp, sizeof stamp, "%Y-%m-%dT%H:%M:%SZ", &tm);
    pad_header_set_text(s->hdr, "created", stamp);
    pad_header_set_text(s->hdr, "host_os", host_os());
    pad_header_set_text(s->hdr, "name", m && m->name && *m->name ? m->name : s->name);
    if (m && m->notes && *m->notes)
        pad_header_set_text(s->hdr, "notes", m->notes);
    pad_header_set_int(s->hdr, "deterministic", xbox_GuestTimeVirtual() ? 1 : 0);
    install_exit();
    publish(s, flags, 0);
    return 0;
}

/* The last playback's paths, for padrec_again. */
static char s_last_path[1024];

int padrec_play(const char *const *paths, int n, unsigned flags, char *why, size_t whyn)
{
    const char *env = padrec_env_driven();
    PadSession *s;
    PadHeader *h;
    PadStep *steps;
    char *text;
    int rc, i, anchors;

    if (env) {
        snprintf(why, whyn, "driven by %s", env);
        return -1;
    }
    if (n != 1 || !paths || !paths[0]) {
        /* Setups and chains come with phase 3 (R6). */
        snprintf(why, whyn, n > 1 ? "chains come later" : "no recording");
        return -1;
    }
    if (!(text = read_text(paths[0], why, whyn)))
        return -1;
    h = malloc(sizeof *h);
    if (!h) {
        free(text);
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    rc = pad_header_read(text, h);
    if (rc < 0 || strcmp(pad_header_origin(h), "session")) {
        if (rc < 0)
            snprintf(why, whyn, "%s: %s", pad_path_name(paths[0]), pad_header_error(rc));
        else
            /* Counted from boot: it plays only from the start of a run. */
            snprintf(why, whyn, "%s was recorded from boot (RECOMP_PAD_SCRIPT)",
                     pad_path_name(paths[0]));
        free(h);
        free(text);
        return -1;
    }
    if ((anchors = pad_script_anchor_lines(text)) > PAD_ANCHOR_MAX) {
        snprintf(why, whyn, "%d anchors, over %d", anchors, PAD_ANCHOR_MAX);
        free(h);
        free(text);
        return -1;
    }
    s = calloc(1, sizeof *s);
    steps = malloc((size_t)(PAD_STEPS_MAX + 1) * sizeof *steps);
    if (!s || !steps || !(s->anchors = calloc(1, sizeof *s->anchors))) {
        if (s)
            free(s->anchors);
        free(s);
        free(steps);
        free(h);
        free(text);
        snprintf(why, whyn, "out of memory");
        return -1;
    }
    s->kind = KIND_PLAY;
    s->nsteps = pad_script_parse_anchored(text, steps, 0, PAD_STEPS_MAX + 1, s->anchors);
    free(text);
    if (s->nsteps > PAD_STEPS_MAX || s->nsteps <= 0) {
        snprintf(why, whyn, s->nsteps > 0 ? "over %d steps" : "no steps", PAD_STEPS_MAX);
        free(steps);
        free(h);
        session_free(s);
        return -1;
    }
    s->steps = realloc(steps, (size_t)s->nsteps * sizeof *steps);
    if (!s->steps)
        s->steps = steps;
    s->clock = s->anchors->clock == 2 ? 2 : s->anchors->clock == 1 ? 1 : 0;
    for (i = 0; i < s->nsteps; i++)
        s->pads |= 1u << (s->steps[i].pad & 3);
    for (i = 0; i < s->anchors->n; i++)
        if (s->anchors->a[i].period)
            s->pads |= 1u << (s->anchors->a[i].retry.pad & 3);
    s->total = (uint32_t)pad_header_get_int(h, "length", 0);
    {
        const char *nm = pad_header_get(h, "name"), *so = pad_header_get(h, "start_open");
        if (nm && *nm)
            snprintf(s->name, sizeof s->name, "%s", nm);
        else
            name_of(s->name, sizeof s->name, paths[0]);
        if (so)
            snprintf(s->want_open, sizeof s->want_open, "%s", so);
    }
    free(h);
    snprintf(s_last_path, sizeof s_last_path, "%s", paths[0]);
    publish(s, flags, s->pads);
    return 0;
}

int padrec_again(unsigned flags, char *why, size_t whyn)
{
    char path[sizeof s_last_path];
    const char *p = path;

    if (!s_last_path[0]) {
        snprintf(why, whyn, "nothing played yet");
        return -1;
    }
    memcpy(path, s_last_path, sizeof path);
    return padrec_play(&p, 1, flags, why, whyn);
}

void padrec_stop(void)
{
    PadSession *p = __atomic_exchange_n(&s_pending, NULL, __ATOMIC_ACQ_REL);

    __atomic_store_n(&s_pending_pads, 0, __ATOMIC_RELEASE);
    if (p) {
        /* An ARMED session that never started: STOPPED at once. */
        PadrecStatus st;
        status_of(p, PADREC_STOPPED, 0, &st);
        st.end_ms = (uint32_t)GetTickCount();
        if (!__atomic_load_n(&s_live, __ATOMIC_ACQUIRE))
            status_put(&st);
        pending_drop(p);
    }
    __atomic_store_n(&s_hold, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
}

void padrec_record_stop(void)
{
    padrec_stop();
}

void padrec_release(void)
{
    __atomic_store_n(&s_hold, 0, __ATOMIC_RELEASE);
}

void padrec_reset_for_test(void)
{
    PadrecStatus st;
    pad_lock();
    if (s_active && s_active->kind == KIND_REC)
        record_end(s_active, 0);
    session_free(s_active);
    session_free(s_retired);
    session_free(__atomic_exchange_n(&s_pending, NULL, __ATOMIC_ACQ_REL));
    s_active = s_retired = NULL;
    s_sig_rec = NULL;
    s_live = s_playing = 0;
    s_play_pads = s_pending_pads = 0;
    s_stop = s_hold = 0;
    s_synced = 0;
    s_last_path[0] = 0;
    pad_unlock();
    memset(&st, 0, sizeof st);
    status_put(&st);
}
