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
#define OTHER_PATH "pad_session_other.pad"
#define MOVED_PATH "pad_session_moved.pad"
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

/* Another title's file is refused (copied into recordings/ by hand); the
 * last playback follows a rename and is gone after a delete. */
static void test_title_and_moves(void)
{
    const char *one[1] = { OTHER_PATH };
    char why[160];
    FILE *f = fopen(OTHER_PATH, "wb");
    printf("another title, rename, delete\n");
    if (f) {
        fputs("# xbr-export: pad 1\n# xbr-pad: title = \"BLiNX\"\n"
              "# xbr-pad: title_id = \"4d530018\"\n# xbr-pad: origin = \"session\"\n"
              "f10:A:5\nf20:@end\n", f);
        fclose(f);
    }
    padrec_set_title(0x4d530065, "BLiNX 2");
    why[0] = 0;
    check(padrec_play(one, 1, 0, why, sizeof why) < 0
              && strstr(why, "a recording of BLiNX (4d530018), not of BLiNX 2 (4d530065)"),
          "another title's file is refused");
    padrec_set_title(0x4d530018, "BLiNX");
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "its own title plays it");
    padrec_stop();
    poll0(7000, 0, 0);
    remove(MOVED_PATH);
    check(rename(OTHER_PATH, MOVED_PATH) == 0, "renamed on disk");
    padrec_path_moved(OTHER_PATH, MOVED_PATH);
    check(padrec_again(0, why, sizeof why) == 0, "play again after a rename");
    padrec_stop();
    poll0(7001, 0, 0);
    padrec_path_moved(MOVED_PATH, NULL);
    why[0] = 0;
    check(padrec_again(0, why, sizeof why) < 0 && strstr(why, "nothing played"),
          "nothing to play again after a delete");
    padrec_set_title(0, NULL);
}

/* ---- chains, requires, actions (pad-recording-ui R5) -------------------- */

#define SEG1_PATH "pad_session_seg1.pad"
#define SEG2_PATH "pad_session_seg2.pad"
#define BIG1_PATH "pad_session_big1.pad"
#define BIG2_PATH "pad_session_big2.pad"
#define REQ_PATH "pad_session_req.pad"
#define ACT_PATH "pad_session_act.pad"
#define TAIL_PATH "pad_session_tail.pad"

static void put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

#define SESSION_HDR "# xbr-export: pad 1\n# xbr-pad: origin = \"session\"\n"

/* One port-0 poll with the flip at half the poll, so a flip file and a
 * poll file both move. The A and B bytes the title got. */
static int poll_fb(uint32_t poll, int *b)
{
    PadrecClocks c = at(poll);
    uint8_t r[20];
    memset(r, 0, sizeof r);
    r[1] = 20;
    c.flip = poll / 2;
    padrec_sync(&c);
    padrec_press(0, r, &c);
    padrec_feed(0, r, &c);
    if (b)
        *b = r[5];
    return r[4];
}

static void test_chain(void)
{
    const char *two[2] = { SEG1_PATH, SEG2_PATH };
    PadrecStatus st;
    char why[160];
    uint32_t p, a_last = 0, b_first = 0;
    int b, seg_at_b = 0;

    printf("a chain: a flip file, then a poll file\n");
    put(SEG1_PATH, SESSION_HDR "f0:A:2\nf3:@end\n");
    put(SEG2_PATH, SESSION_HDR "g0:B:2\ng4:@end\n");
    check(padrec_play(two, 2, 0, why, sizeof why) == 0, "the chain plays");
    for (p = 8000; p < 8040 && state() != PADREC_ENDED; p++) {
        int a = poll_fb(p, &b);
        if (a)
            a_last = p;
        if (b && !b_first) {
            b_first = p;
            padrec_status(&st);
            seg_at_b = st.seg * 10 + st.segs;
        }
    }
    check(a_last >= 8000 && a_last < 8006, "the first file's A, from its own first flip");
    check(b_first > a_last, "the second file's B after the first ended");
    check(seg_at_b == 22, "the strip reads 2/2 during the second");
    check(state() == PADREC_ENDED, "the chain ends with its last file");
    padrec_status(&st);
    check(st.seg == 2 && st.segs == 2, "ended on 2/2");
}

static void test_chain_refusals(void)
{
    const char *boot2[2] = { SEG1_PATH, BOOT_PATH };
    const char *boot1[2] = { BOOT_PATH, SEG1_PATH };
    const char *big[2] = { BIG1_PATH, BIG2_PATH };
    char why[160], *text;
    size_t at_ = 0, cap = (size_t)70000 * 16 + 128;
    int i;

    printf("chain refusals\n");
    put(BOOT_PATH, "f10:A:5\n");
    why[0] = 0;
    check(padrec_play(boot2, 2, 0, why, sizeof why) < 0 && strstr(why, "only first"),
          "a boot file second in a chain is refused");
    check(padrec_play(boot1, 2, PADREC_BOOT, why, sizeof why) == 0,
          "a boot file first in a chain started at boot plays");
    padrec_stop();
    poll0(8100, 0, 0);
    text = malloc(cap);
    at_ = (size_t)snprintf(text, cap, SESSION_HDR);
    for (i = 0; i < 70000; i++)
        at_ += (size_t)snprintf(text + at_, cap - at_, "g%d:A:1\n", i * 2);
    put(BIG1_PATH, text);
    put(BIG2_PATH, text);
    free(text);
    why[0] = 0;
    check(padrec_play(big, 2, 0, why, sizeof why) < 0 && strstr(why, "the chain is over"),
          "a chain over the step cap is refused before it plays");
    check(state() != PADREC_PLAY && state() != PADREC_ARMED_PLAY, "nothing plays");
    remove(BIG1_PATH);
    remove(BIG2_PATH);
}

static int s_req_on;
static int req_hook(const char *key)
{
    return s_req_on && !strcmp(key, "mods.test.enable");
}

static void test_requires(void)
{
    const char *one[1] = { REQ_PATH };
    char why[160];

    printf("requires\n");
    put(REQ_PATH, SESSION_HDR "# xbr-pad: requires = [\"mods.test.enable\"]\ng0:A:1\ng3:@end\n");
    padrec_set_requires_hook(req_hook);
    s_req_on = 0;
    why[0] = 0;
    check(padrec_play(one, 1, 0, why, sizeof why) == PADREC_E_REQUIRES
              && strstr(why, "needs mods.test.enable on"),
          "an unmet requires is refused, naming the key");
    check(!strcmp(padrec_unmet(), "mods.test.enable"), "padrec_unmet names it");
    s_req_on = 1;
    check(padrec_play(one, 1, 0, why, sizeof why) == 0 && !padrec_unmet()[0],
          "it plays once the key is on");
    padrec_stop();
    poll0(8200, 0, 0);
    padrec_set_requires_hook(NULL);
}

static int s_fires;
static int fire_hook(const char *name, char *why, size_t n)
{
    (void)why;
    (void)n;
    if (!strcmp(name, "test.act"))
        s_fires++;
    return 0;
}

static void test_actions(void)
{
    const char *one[1] = { ACT_PATH };
    PadrecMeta m;
    PadrecSeg seg;
    char why[160], *txt;
    uint32_t p;

    printf("!<action> steps and play-then-record\n");
    put(ACT_PATH, SESSION_HDR "g1:!test.act\ng4:@end\n");
    padrec_set_action_hook(fire_hook);
    s_fires = 0;
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "an untrusted file plays");
    for (p = 8300; p < 8320 && state() != PADREC_ENDED; p++)
        poll0(p, 0, 0);
    check(s_fires == 0, "its action does not fire");
    check(padrec_play(one, 1, PADREC_TRUST, why, sizeof why) == 0, "a trusted one plays");
    poll0(8320, 0, 0);
    for (p = 8321; p < 8340 && state() != PADREC_ENDED; p++)
        poll0(p, 0, 0);
    check(s_fires == 1, "its action fires once");

    /* Play the file, then record: the record starts when it ends, counted
     * from its own first poll, and writes the setup's name and a fire. */
    remove(TAIL_PATH);
    memset(&m, 0, sizeof m);
    m.setup = "pad_session_act";
    seg.path = ACT_PATH;
    seg.trusted = 1;
    seg.allowed = NULL;
    s_fires = 0;
    check(padrec_play_chain(&seg, 1, 0, TAIL_PATH, &m, why, sizeof why) == 0,
          "play then record starts");
    for (p = 8400; p < 8420 && state() != PADREC_REC; p++)
        poll0(p, 0, 0);
    check(state() == PADREC_REC && s_fires == 1, "the record runs after the setup");
    poll0(p++, 1, 0);
    poll0(p++, 1, 0);
    padrec_action_fired("test.act");
    poll0(p++, 0, 0);
    padrec_record_stop();
    poll0(p++, 0, 0);
    poll0(p++, 0, 0);
    txt = slurp(TAIL_PATH);
    check(txt != NULL, "the record's file is there");
    if (txt) {
        check(strstr(txt, "setup = \"pad_session_act\"") != NULL, "its header names the setup");
        check(strstr(txt, "\ng1:a:2") != NULL,
              "its first press near g0, not the setup's counts");
        check(strstr(txt, ":!test.act\n") != NULL, "the fire is a !test.act line");
        check(count_of(txt, "@end") == 1, "one @end");
        free(txt);
    }

    /* Confirmed, then the file changed (R6): what plays, and what Play
     * again plays, is only what the player saw. */
    padrec_stop();
    poll0(p++, 0, 0);
    put(ACT_PATH, SESSION_HDR "g1:!test.act\ng4:@end\n");
    seg.allowed = "other.act";
    check(padrec_play_chain(&seg, 1, 0, NULL, NULL, why, sizeof why) < 0 &&
              strstr(why, "!test.act was not confirmed"),
          "an action outside the confirmed list is refused");
    seg.allowed = "other.act, test.act";
    check(padrec_play_chain(&seg, 1, 0, NULL, NULL, why, sizeof why) == 0,
          "the confirmed list plays");
    for (; p < 8600 && state() != PADREC_ENDED; p++)
        poll0(p, 0, 0);
    put(ACT_PATH, SESSION_HDR "g1:!test.act\ng2:!evil.act\ng4:@end\n");
    check(padrec_again(0, why, sizeof why) < 0 && strstr(why, "!evil.act was not confirmed"),
          "Play again re-checks the file it confirmed");
    put(ACT_PATH, SESSION_HDR "g1:!test.act\ng4:@end\n");
    check(padrec_again(0, why, sizeof why) == 0, "Play again plays it unchanged");
    padrec_stop();
    poll0(p++, 0, 0);
    padrec_set_action_hook(NULL);
    remove(TAIL_PATH);
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
    test_title_and_moves();
    test_chain();
    test_chain_refusals();
    test_requires();
    test_actions();
    padrec_reset_for_test();
    remove(REC_PATH);
    remove(ARMED_PATH);
    remove(BOOT_PATH);
    remove(OTHER_PATH);
    remove(MOVED_PATH);
    remove(SEG1_PATH);
    remove(SEG2_PATH);
    remove(REQ_PATH);
    remove(ACT_PATH);
    printf("%s (%d failure%s)\n", failures ? "FAIL" : "PASS", failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
