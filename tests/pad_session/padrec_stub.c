/*
 * padrec_stub.c - the menu side of pad_session.h for the enhancement
 * layer's tests (enhance_menu, mod_status, the mod tests), which build the
 * layer from its sources and no pad. A record creates its file, a playback
 * checks it exists; the state moves as the real one does on the menu thread
 * (ARMED until padrec_release, then REC or PLAY), with no pad thread.
 * padrec_stub_set puts a status for the page's rows; padrec_stub_env makes
 * an env key drive the pad. A record's file holds a session header and
 * its @end, so the page's Info finds it playable; the last playback
 * follows a rename or delete. The title check is pad_session's (its test).
 */
#include "pad_session.h"

#include <stdio.h>
#include <string.h>

static PadrecStatus s_st;
static const char *s_env;
static char s_last[1024];
static char s_meta_name[PADREC_NAME_MAX];
/* The last chain's files and flags, and the record at its tail, for the
 * page's tests (padrec_stub_chain). */
static char s_chain[PADREC_CHAIN_MAX][1024];
static int s_chain_trust[PADREC_CHAIN_MAX];
static int s_nchain;
static unsigned s_chain_flags;
static char s_chain_rec[1024];
static int (*s_req)(const char *key);
static char s_unmet[96];

/* The real check reads the header (pad_header.c, which not every test
 * links): here the first key of a `requires = ["..."]` line is enough for
 * the page's "Turn on". 0, or -1 with the key in s_unmet. */
static int stub_requires(const char *path)
{
    char buf[4096], *p, *q;
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!(p = strstr(buf, "requires = [\"")))
        return 0;
    p += strlen("requires = [\"");
    if (!(q = strchr(p, '"')) || (size_t)(q - p) >= sizeof s_unmet)
        return 0;
    snprintf(s_unmet, sizeof s_unmet, "%.*s", (int)(q - p), p);
    if (s_req && s_req(s_unmet)) {
        s_unmet[0] = 0;
        return 0;
    }
    return -1;
}

/* The header name the last record was given, to check it is the stem. */
const char *padrec_stub_meta_name(void)
{
    return s_meta_name;
}

void padrec_stub_set(const PadrecStatus *st)
{
    s_st = *st;
}

void padrec_stub_env(const char *key)
{
    s_env = key;
}

void padrec_stub_reset(void)
{
    memset(&s_st, 0, sizeof s_st);
    s_env = NULL;
    s_last[0] = 0;
    s_nchain = 0;
    s_chain_rec[0] = 0;
    s_unmet[0] = 0;
}

void padrec_set_title(uint32_t title_id, const char *title)
{
    (void)title_id;
    (void)title;
}

void padrec_path_moved(const char *from, const char *to)
{
    if (from && !strcmp(s_last, from))
        snprintf(s_last, sizeof s_last, "%s", to ? to : "");
}

static void start(const char *path, int kind, unsigned flags)
{
    const char *b = path, *p, *dot;
    size_t n;
    for (p = path; *p; p++)
        if (*p == '/' || *p == '\\')
            b = p + 1;
    dot = strrchr(b, '.');
    n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (n >= sizeof s_st.name)
        n = sizeof s_st.name - 1;
    memset(&s_st, 0, sizeof s_st);
    memcpy(s_st.name, b, n);
    s_st.serial++;
    s_st.clock = 2;
    if (kind == PADREC_PLAY)
        s_st.nsteps = 1;
    s_st.state = (flags & PADREC_ARMED) ? (kind == PADREC_REC ? PADREC_ARMED_REC
                                                               : PADREC_ARMED_PLAY)
                                        : kind;
}

int padrec_any(void) { return s_st.serial != 0; }
int padrec_playing(void) { return s_st.state == PADREC_PLAY || s_st.state == PADREC_WAIT; }
const char *padrec_env_driven(void) { return s_env; }
void padrec_set_clock_key(const char *v) { (void)v; }
void padrec_status(PadrecStatus *out) { *out = s_st; }

int padrec_record_start(const char *path, const PadrecMeta *meta, unsigned flags, char *why,
                        size_t whyn)
{
    FILE *f;
    snprintf(s_meta_name, sizeof s_meta_name, "%s", meta && meta->name ? meta->name : "");
    if (s_env) {
        snprintf(why, whyn, "driven by %s", s_env);
        return -1;
    }
    f = fopen(path, "wb");
    if (!f) {
        snprintf(why, whyn, "cannot write");
        return -1;
    }
    fputs("# xbr-export: pad 1\n# xbr-pad: origin = \"session\"\nf1:@end\n", f);
    fclose(f);
    start(path, PADREC_REC, flags);
    return 0;
}


int padrec_stub_chain(int i, const char **path, int *trusted, unsigned *flags,
                      const char **rec)
{
    if (path)
        *path = i >= 0 && i < s_nchain ? s_chain[i] : NULL;
    if (trusted)
        *trusted = i >= 0 && i < s_nchain ? s_chain_trust[i] : 0;
    if (flags)
        *flags = s_chain_flags;
    if (rec)
        *rec = s_chain_rec;
    return s_nchain;
}

int padrec_play_chain(const PadrecSeg *segs, int n, unsigned flags, const char *rec_path,
                      const PadrecMeta *meta, char *why, size_t whyn)
{
    FILE *f;
    int i;
    if (s_env) {
        snprintf(why, whyn, "driven by %s", s_env);
        return -1;
    }
    if (n < 1 || n > PADREC_CHAIN_MAX) {
        snprintf(why, whyn, "cannot read");
        return -1;
    }
    s_unmet[0] = 0;
    for (i = 0; i < n; i++) {
        if (!(f = fopen(segs[i].path, "rb"))) {
            snprintf(why, whyn, "cannot read");
            return -1;
        }
        fclose(f);
        if (stub_requires(segs[i].path) < 0) {
            snprintf(why, whyn, "needs %s on", s_unmet);
            return PADREC_E_REQUIRES;
        }
    }
    s_nchain = n;
    s_chain_flags = flags;
    for (i = 0; i < n; i++) {
        snprintf(s_chain[i], sizeof s_chain[i], "%s", segs[i].path);
        s_chain_trust[i] = segs[i].trusted;
    }
    snprintf(s_chain_rec, sizeof s_chain_rec, "%s", rec_path ? rec_path : "");
    if (rec_path) {
        snprintf(s_meta_name, sizeof s_meta_name, "%s", meta && meta->name ? meta->name : "");
        if ((f = fopen(rec_path, "wb"))) {
            fputs("# xbr-export: pad 1\n# xbr-pad: origin = \"session\"\nf1:@end\n", f);
            fclose(f);
        }
    }
    snprintf(s_last, sizeof s_last, "%s", segs[n - 1].path);
    start(segs[0].path, PADREC_PLAY, flags);
    s_st.seg = 1;
    s_st.segs = n + (rec_path != NULL);
    return 0;
}

int padrec_play(const char *const *paths, int n, unsigned flags, char *why, size_t whyn)
{
    PadrecSeg segs[PADREC_CHAIN_MAX];
    if (n < 1 || n > PADREC_CHAIN_MAX || !paths) {
        snprintf(why, whyn, "cannot read");
        return -1;
    }
    for (int i = 0; i < n; i++) {
        segs[i].path = paths[i];
        segs[i].trusted = (flags & PADREC_TRUST) != 0;
        segs[i].allowed = NULL;
    }
    return padrec_play_chain(segs, n, flags, NULL, NULL, why, whyn);
}

void padrec_set_requires_hook(int (*on)(const char *key))
{
    s_req = on;
}

const char *padrec_unmet(void)
{
    return s_unmet;
}

/* The real one reads the header; the page's tests need only "none". */
int padrec_requires(const char *text, char *unmet, size_t n)
{
    (void)text;
    if (unmet && n)
        unmet[0] = 0;
    return 0;
}

void padrec_set_quit(void (*fn)(void))
{
    (void)fn;
}

void padrec_action_fired(const char *name)
{
    (void)name;
}

void padrec_set_action_hook(int (*fire)(const char *name, char *why, size_t n))
{
    (void)fire;
}

int padrec_again(unsigned flags, char *why, size_t whyn)
{
    const char *p = s_last;
    if (!s_last[0]) {
        snprintf(why, whyn, "nothing played yet");
        return -1;
    }
    return padrec_play(&p, 1, flags, why, whyn);
}

void padrec_stop(void)
{
    if (s_st.state != PADREC_IDLE)
        s_st.state = PADREC_STOPPED;
}

void padrec_record_stop(void)
{
    padrec_stop();
}

void padrec_release(void)
{
    if (s_st.state == PADREC_ARMED_REC)
        s_st.state = PADREC_REC;
    else if (s_st.state == PADREC_ARMED_PLAY)
        s_st.state = PADREC_PLAY;
}
