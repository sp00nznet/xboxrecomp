/*
 * pad_session: a record and a playback started in the game, through the
 * calls usb_gamepad.c makes each poll (see CMakeLists.txt).
 */
#include "pad_session.h"
#include "recomp_env.h"
#include "recomp_exit_hook.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supplied by recompiled game code in a real build; nothing here calls one. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define REC_PATH "pad_session_rec.pad"
#define ARMED_PATH "pad_session_armed.pad"
#define BOOT_PATH "pad_session_boot.pad"
#define REC_AT 1000u
#define PLAY_AT 5000u
#define A_POLLS 5u      /* A held from the record's first poll */
#define REC_POLLS 10u

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static PadrecClocks at(uint32_t poll)
{
    PadrecClocks c;
    memset(&c, 0, sizeof c);
    c.polls = poll;
    c.ms = (unsigned long)poll * 16u;
    c.game_tap = 1;
    return c;
}

static int state(void)
{
    PadrecStatus st;
    padrec_status(&st);
    return st.state;
}

static char *slurp(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *b;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    b = malloc((size_t)n + 1);
    n = (long)fread(b, 1, (size_t)n, f);
    b[n] = 0;
    fclose(f);
    return b;
}

static int count_of(const char *s, const char *what)
{
    int n = 0;
    for (const char *p = s; (p = strstr(p, what)); p += strlen(what))
        n++;
    return n;
}

/* One poll of port 0: the sync, the press (host bits in), the title's read
 * and the record. Returns the A byte the title got. */
static int poll0(uint32_t poll, int host_a, int host_b)
{
    PadrecClocks c = at(poll);
    uint8_t r[20];
    memset(r, 0, sizeof r);
    r[1] = 20;
    if (host_a)
        r[4] = 0xFF;
    if (host_b)
        r[5] = 0xFF;
    padrec_sync(&c);
    padrec_press(0, r, &c);
    padrec_feed(0, r, &c);
    return r[4];
}

static void test_clock_pick(void)
{
    const char *why;
    printf("clock pick\n");
    check(!strcmp(padrec_clock_pick(1, 0, &why), "poll"), "auto on the game's tap: poll");
    check(!strcmp(padrec_clock_pick(0, 7, &why), "flip"), "auto on the USB model with flips: flip");
    check(!strcmp(padrec_clock_pick(0, 0, &why), "poll"), "auto with no flips counted: poll");
    padrec_set_clock_key("flip");
    check(!strcmp(padrec_clock_pick(1, 0, &why), "flip") && !strcmp(why, "padrec.clock"),
          "padrec.clock = flip wins over the tap");
    padrec_set_clock_key("auto");
    check(!strcmp(padrec_clock_pick(1, 0, &why), "poll"), "auto again");
}

static void test_record(void)
{
    PadrecMeta m;
    char why[160];
    char *txt;
    uint32_t p;
    int rc;

    printf("record from poll %u\n", REC_AT);
    remove(REC_PATH);
    memset(&m, 0, sizeof m);
    m.title = "Test";
    m.title_id = 0x4d530004;
    m.start_point = "1-1\r\nstart";
    rc = padrec_record_start(REC_PATH, &m, 0, why, sizeof why);
    check(rc == 0, "record starts");
    check(state() != PADREC_REC, "not live before the next port-0 poll");
    for (p = REC_AT; p < REC_AT + REC_POLLS; p++)
        poll0(p, p < REC_AT + A_POLLS, 0);
    check(state() == PADREC_REC, "live after the swap");
    padrec_record_stop();
    check(state() == PADREC_REC, "a stop acts at the next poll");
    poll0(p, 0, 0);
    check(state() == PADREC_STOPPED, "stopped at the next poll");
    padrec_record_stop();
    poll0(p + 1, 0, 0);
    txt = slurp(REC_PATH);
    check(txt != NULL, "the file is there");
    if (!txt)
        return;
    check(strstr(txt, "# xbr-export: pad 1") != NULL, "the header's marker");
    check(strstr(txt, "origin = \"session\"") != NULL, "origin session");
    check(strstr(txt, "clock = \"poll\"") != NULL, "clock poll (the game's tap)");
    check(strstr(txt, "\ng0:") != NULL, "the first step at g0, not g1000");
    check(strstr(txt, "g1000") == NULL, "no boot counts");
    check(count_of(txt, "@end") == 1, "one @end after two stops");
    check(strstr(txt, "length = 10") != NULL, "length patched to the last count");
    check(strstr(txt, "start_point = \"1-1start\"") != NULL, "CR and LF dropped from the text");
    free(txt);
}

static void test_play(void)
{
    const char *one[1] = { REC_PATH };
    char why[160];
    int a_ok = 1, b_out = 1;
    uint32_t p;

    printf("playback from poll %u\n", PLAY_AT);
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "the record plays");
    check(!padrec_playing(), "not playing before the next port-0 poll");
    for (p = PLAY_AT; p < PLAY_AT + A_POLLS; p++) {
        /* The host holds B: the replay zeroes it (R3). */
        uint8_t r[20];
        PadrecClocks c = at(p);
        memset(r, 0, sizeof r);
        r[5] = 0xFF;
        padrec_sync(&c);
        padrec_press(0, r, &c);
        if (!r[4])
            a_ok = 0;
        if (r[5])
            b_out = 0;
    }
    check(padrec_playing(), "playing after the swap");
    check(a_ok, "A pressed from poll 5000 for the record's 5 polls");
    check(b_out, "the host's B never reaches the title");
    check(poll0(p, 0, 0) == 0, "A released after its hold");
    for (p++; p < PLAY_AT + 40 && state() != PADREC_ENDED; p++)
        poll0(p, 0, 0);
    check(state() == PADREC_ENDED, "ended at the record's @end");
    check(!padrec_playing(), "the pad is the host's again");
}

static void test_again(void)
{
    char why[160];
    check(padrec_again(PADREC_ARMED, why, sizeof why) == 0, "play again, ARMED");
    check(state() == PADREC_ARMED_PLAY, "ARMED_PLAY while the menu is open");
    poll0(6000, 0, 0);
    poll0(6001, 0, 0);
    check(!padrec_playing(), "an ARMED session waits through polls");
    padrec_release();
    check(poll0(6002, 0, 0) != 0, "released: A pressed at the next poll");
    padrec_stop();
    poll0(6003, 0, 0);
    check(state() == PADREC_STOPPED && !padrec_playing(), "stop ends the playback");
}

static void test_armed_cancel(void)
{
    char why[160];
    FILE *f;
    printf("an ARMED record cancelled\n");
    remove(ARMED_PATH);
    check(padrec_record_start(ARMED_PATH, NULL, PADREC_ARMED, why, sizeof why) == 0,
          "an ARMED record starts");
    check(state() == PADREC_ARMED_REC, "ARMED_REC");
    padrec_stop();
    check(state() == PADREC_STOPPED, "stopped at once");
    f = fopen(ARMED_PATH, "rb");
    check(f == NULL, "its empty file is gone");
    if (f)
        fclose(f);
}

/* The game quits with a record still armed: it never started, so its file
 * holds no steps and goes, not stays listed to be refused. */
static void test_armed_exit(void)
{
    char why[160];
    FILE *f;
    printf("an ARMED record at exit\n");
    remove(ARMED_PATH);
    check(padrec_record_start(ARMED_PATH, NULL, PADREC_ARMED, why, sizeof why) == 0,
          "an ARMED record starts");
    f = fopen(ARMED_PATH, "rb");
    check(f != NULL, "its file is made at once");
    if (f)
        fclose(f);
    recomp_exit_hook_run("test");
    f = fopen(ARMED_PATH, "rb");
    check(f == NULL, "the exit removes its file");
    if (f)
        fclose(f);
}

static void test_refusals(void)
{
    const char *one[1] = { BOOT_PATH };
    char why[160];
    FILE *f = fopen(BOOT_PATH, "wb");
    printf("refusals\n");
    if (f) {
        fputs("f10:A:5\n", f);
        fclose(f);
    }
    why[0] = 0;
    check(padrec_play(one, 1, 0, why, sizeof why) < 0 && why[0],
          "a file with no header (an env record) is refused");
    recomp_env_set(RENV_PAD_PRESS, "a@100");
    check(padrec_env_driven() && !strcmp(padrec_env_driven(), "RECOMP_PAD_PRESS"),
          "RECOMP_PAD_PRESS drives the pad");
    why[0] = 0;
    check(padrec_record_start(ARMED_PATH, NULL, 0, why, sizeof why) < 0
              && strstr(why, "RECOMP_PAD_PRESS"),
          "a record is refused while it does");
    recomp_env_set(RENV_PAD_PRESS, NULL);
    check(padrec_env_driven() == NULL, "no env key");
}

int main(void)
{
    recomp_env_init();
    test_clock_pick();
    test_record();
    test_play();
    test_again();
    test_armed_cancel();
    test_armed_exit();
    test_refusals();
    padrec_reset_for_test();
    remove(REC_PATH);
    remove(ARMED_PATH);
    remove(BOOT_PATH);
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
