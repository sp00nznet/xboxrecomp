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

int padrec_play(const char *const *paths, int n, unsigned flags, char *why, size_t whyn)
{
    FILE *f;
    if (n != 1 || !(f = fopen(paths[0], "rb"))) {
        snprintf(why, whyn, "cannot read");
        return -1;
    }
    fclose(f);
    snprintf(s_last, sizeof s_last, "%s", paths[0]);
    start(paths[0], PADREC_PLAY, flags);
    return 0;
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
