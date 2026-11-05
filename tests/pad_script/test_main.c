/* pad_script: the step grammar (ms and f<flip>), step timing, the press, and
 * the recorder, whose lines must parse back to the presses it saw. */
#include "pad_script.h"

#include <stdio.h>
#include <string.h>

static int fails;
#define CHECK(c) do { if (!(c)) { fails++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void report(uint8_t *r, uint8_t dig, uint8_t ana, int16_t lx, int16_t ly)
{
    int j;
    memset(r, 0, 20);
    r[1] = 20;
    r[2] = dig;
    for (j = 0; j < 8; j++)
        if (ana & (1u << j))
            r[4 + j] = 0xFF;
    memcpy(&r[12], &lx, 2);
    memcpy(&r[14], &ly, 2);
}

static void test_parse(void)
{
    PadStep s[16];
    int n = pad_script_parse("8000:start:300, 9000:a\n# a comment, a:b\n"
                             "f120:lup+a:30 f200:x f300:p2-b:4f 010:down",
                             s, 0, 16);
    CHECK(n == 6);
    CHECK(!s[0].by_flip && s[0].at == 8000 && s[0].hold == 300 && s[0].digital == 0x10);
    CHECK(!s[1].by_flip && s[1].at == 9000 && s[1].hold == 150 && s[1].analog == 0x01);
    CHECK(s[2].by_flip && s[2].at == 120 && s[2].hold == 30);
    CHECK(s[2].stick == 0x04 && s[2].analog == 0x01);
    CHECK(s[3].by_flip && s[3].hold == PAD_HOLD_FLIPS_DEFAULT && s[3].analog == 0x04);
    CHECK(s[4].by_flip && s[4].pad == 1 && s[4].hold == 4 && s[4].analog == 0x02);
    /* Decimal: a leading zero is not octal. */
    CHECK(!s[5].by_flip && s[5].at == 10 && s[5].digital == 0x02);
    n = pad_script_parse("f010:a:08", s, 0, 16);
    CHECK(n == 1 && s[0].at == 10 && s[0].hold == 8);
    /* Hex is not a time any more. */
    CHECK(pad_script_parse("0x10:down", s, 0, 16) == 0);

    /* Sticks at a value. */
    n = pad_script_parse("f5:lx=-12000+ry=300+a:2", s, 0, 16);
    CHECK(n == 1 && s[0].axis_set == 0x09 && s[0].axis[0] == -12000
          && s[0].axis[3] == 300 && s[0].analog == 0x01);
    n = pad_script_parse("f5:lx=40000:2", s, 0, 16);
    CHECK(n == 1 && s[0].axis_set == 0);

    /* Appends after len, stops at max. */
    n = pad_script_parse("f1:a f2:b f3:x", s, 14, 16);
    CHECK(n == 16 && s[15].analog == 0x02);
    /* An entry without a time ends the parse. */
    CHECK(pad_script_parse("f1:a, start", s, 0, 16) == 1);
    /* A sign or a blank is no time (strtoul would wrap "-1"), and a hold
     * needs its number: each ends the parse. */
    CHECK(pad_script_parse("f-1:a", s, 0, 16) == 0);
    CHECK(pad_script_parse("f1:a f +5:b", s, 0, 16) == 1);
    CHECK(pad_script_parse("-5:a", s, 0, 16) == 0);
    CHECK(pad_script_parse("f1:a f10:a: f20:b", s, 0, 16) == 1);
    CHECK(pad_script_parse("f10:a:-3", s, 0, 16) == 0);
}

static void test_timing(void)
{
    PadStep s[3];
    pad_script_parse("f100:a:10, 500:b:100, f50:x:4", s, 0, 3);
    /* A flip step ignores wall time, a ms step ignores flips. */
    CHECK(!pad_step_active(&s[0], 99999, 99));
    CHECK(pad_step_active(&s[0], 0, 100));
    CHECK(pad_step_active(&s[0], 0, 109));
    CHECK(!pad_step_active(&s[0], 0, 110) && pad_step_done(&s[0], 0, 110));
    CHECK(!pad_step_done(&s[0], 99999, 105));
    CHECK(pad_step_active(&s[1], 550, 0));
    CHECK(!pad_step_active(&s[1], 499, 100000));
    CHECK(pad_step_done(&s[1], 600, 0));
    /* A live line whose flip has passed is pressed at once, for its hold. */
    CHECK(pad_step_done(&s[2], 0, 80));
    pad_step_not_before(&s[2], 80);
    CHECK(pad_step_active(&s[2], 0, 80) && pad_step_active(&s[2], 0, 83));
    CHECK(pad_step_done(&s[2], 0, 84));
    /* One not yet due keeps its flip, and a ms step is left alone. */
    pad_step_not_before(&s[0], 20);
    CHECK(s[0].at == 100);
    pad_step_not_before(&s[1], 100000);
    CHECK(s[1].at == 500);
}

static void test_truncation(void)
{
    static PadStep s[4];
    CHECK(pad_script_parse("f1:a f2:a f3:a f4:a f5:a f6:a", s, 0, 4) == 4);
    CHECK(s[3].at == 4);
}

static void test_press(void)
{
    PadStep s[2];
    uint8_t r[20];
    int16_t lx, ly, ry;
    pad_script_parse("f1:start+lt+lleft+rdown", s, 0, 1);
    report(r, 0x01, 0, 0, 0);
    pad_step_press(&s[0], r);
    memcpy(&lx, &r[12], 2);
    memcpy(&ly, &r[14], 2);
    memcpy(&ry, &r[18], 2);
    CHECK(r[2] == 0x11);
    CHECK(r[4 + 6] == 0xFF && r[4] == 0);
    CHECK(lx == -32767 && ly == 0 && ry == -32767);

    pad_script_parse("f1:lx=-1234+ly=20000", s, 0, 1);
    report(r, 0, 0, 500, 500);
    memcpy(&r[16], &(int16_t){77}, 2);
    pad_step_press(&s[0], r);
    memcpy(&lx, &r[12], 2);
    memcpy(&ly, &r[14], 2);
    memcpy(&ry, &r[16], 2);
    CHECK(lx == -1234 && ly == 20000 && ry == 77);
}

static void test_bits(void)
{
    uint8_t r[20];
    PadBits b;
    char name[64];
    report(r, 0x10, 0, 20000, -20000);
    r[4] = 30;      /* resting on A: not pressed */
    r[4 + 7] = 31;  /* RT just past the threshold */
    b = pad_bits_from_report(r);
    CHECK(b.digital == 0x10 && b.analog == 0x80);
    CHECK(b.ax[0] == 20000 && b.ax[1] == -20000 && b.ax[2] == 0);
    pad_bits_name(b, name, sizeof name);
    CHECK(strcmp(name, "start+rt") == 0);
}

static PadBits bits(uint8_t dig, uint8_t ana, int16_t lx, int16_t ly)
{
    PadBits b;
    memset(&b, 0, sizeof b);
    b.digital = dig;
    b.analog = ana;
    b.ax[0] = lx;
    b.ax[1] = ly;
    return b;
}

static void test_recorder(void)
{
    PadRecorder rec;
    char out[512];
    memset(&rec, 0, sizeof rec);

    /* A flip's state is written when the next flip's report arrives. */
    CHECK(!pad_recorder_feed(&rec, bits(0, 0, 0, 0), 10, out, sizeof out));
    CHECK(!pad_recorder_feed(&rec, bits(0, 1, 0, 0), 20, out, sizeof out));
    CHECK(!pad_recorder_feed(&rec, bits(0, 1, 0, 0), 21, out, sizeof out));
    CHECK(!pad_recorder_feed(&rec, bits(0, 1, 0, 32767), 25, out, sizeof out));
    /* The stick moving under A does not split A's step. */
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 32767), 40, out, sizeof out) == 0);
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 0), 41, out, sizeof out) == 1);
    CHECK(strcmp(out, "f20:a:20\n") == 0);
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 0), 42, out, sizeof out) == 1);
    CHECK(strcmp(out, "f25:ly=32767:16\n") == 0);

    /* Reports within one flip merge: a tap is kept, the stick is the last. */
    CHECK(!pad_recorder_feed(&rec, bits(0, 1, 9000, 0), 50, out, sizeof out));
    CHECK(!pad_recorder_feed(&rec, bits(0, 0, 9100, 0), 50, out, sizeof out));
    CHECK(!pad_recorder_feed(&rec, bits(0, 0, 9200, 0), 50, out, sizeof out));
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 0), 51, out, sizeof out) == 0);
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 0), 52, out, sizeof out) == 2);
    CHECK(strcmp(out, "f50:a:1\nf50:lx=9200:1\n") == 0);

    /* Jitter inside the band is one step; reaching full throw is a new one. */
    pad_recorder_feed(&rec, bits(0, 0, 10000, 0), 60, out, sizeof out);
    pad_recorder_feed(&rec, bits(0, 0, 10100, 0), 61, out, sizeof out);
    pad_recorder_feed(&rec, bits(0, 0, 9950, 0), 62, out, sizeof out);
    CHECK(!pad_recorder_feed(&rec, bits(0, 0, 32767, 0), 63, out, sizeof out));
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 32767, 0), 64, out, sizeof out) == 1);
    CHECK(strcmp(out, "f60:lx=10000:3\n") == 0);
    /* At the end, what is still held is written. */
    CHECK(pad_recorder_end(&rec, 70, out, sizeof out) == 1);
    CHECK(strcmp(out, "f63:lx=32767:7\n") == 0);
    CHECK(pad_recorder_end(&rec, 80, out, sizeof out) == 0);

    memset(&rec, 0, sizeof rec);
    rec.pad = 1;
    pad_recorder_feed(&rec, bits(0, 1, 0, 0), 60, out, sizeof out);
    pad_recorder_feed(&rec, bits(0, 0, 0, 0), 63, out, sizeof out);
    CHECK(pad_recorder_feed(&rec, bits(0, 0, 0, 0), 64, out, sizeof out) == 1);
    CHECK(strcmp(out, "f60:p2-a:3\n") == 0);
}

/* Record a pad sequence, replay the lines, and the replayed reports match:
 * buttons, a stick ramping up the way the keyboard's does, a half-throw
 * walk, and a button held across the ramp. */
static void test_round_trip(void)
{
    static const struct { uint8_t dig, ana; int16_t lx, ly; } seq[] = {
        {0, 0, 0, 0}, {0x10, 0, 0, 0}, {0x10, 0, 0, 0}, {0, 0, 0, 0},
        {0, 0x01, 0, 4096}, {0, 0x01, 0, 12288}, {0, 0x01, 0, 24576},
        {0, 0x01, 0, 32767}, {0, 0, 0, 32767},
        {0, 0x40, -16384, 0}, {0x01, 0x40, -16384, 0}, {0x01, 0, -16384, 9000},
        {0, 0, 0, 0}, {0, 0, 0, 0},
    };
    enum { N = sizeof seq / sizeof seq[0] };
    PadRecorder rec;
    PadStep steps[64];
    char text[2048] = "", out[512];
    int i, n;

    memset(&rec, 0, sizeof rec);
    for (i = 0; i < N; i++) {
        uint8_t r[20];
        report(r, seq[i].dig, seq[i].ana, seq[i].lx, seq[i].ly);
        if (pad_recorder_feed(&rec, pad_bits_from_report(r), (uint32_t)(100 + i),
                              out, sizeof out))
            strcat(text, out);
    }
    if (pad_recorder_end(&rec, 100 + N, out, sizeof out))
        strcat(text, out);
    n = pad_script_parse(text, steps, 0, 64);
    CHECK(n > 0);
    for (i = 0; i < N; i++) {
        uint8_t want[20], got[20];
        int k;
        report(want, seq[i].dig, seq[i].ana, seq[i].lx, seq[i].ly);
        report(got, 0, 0, 0, 0);
        for (k = 0; k < n; k++)
            if (pad_step_active(&steps[k], 0, (uint32_t)(100 + i)))
                pad_step_press(&steps[k], got);
        if (memcmp(want, got, 20) != 0) {
            fails++;
            fprintf(stderr, "FAIL round trip at flip %d\n%s", 100 + i, text);
        }
    }
}

/* Anchors: f<flip>:@open:<text>[:<buttons>/<period>]. */
static void test_anchor_parse(void)
{
    static PadAnchors a;
    PadStep s[16];
    int n;

    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored("f10:a:5\nf40:@open:menu.xpr\n"
                                  "f45:b:2 f20:@open:intro.sfd:a/61\n"
                                  "f20:start f60:p2-x f40:y",
                                  s, 0, 16, &a);
    CHECK(n == 5);
    /* Sorted by flip, the retry parsed. */
    CHECK(a.n == 2 && a.a[0].at == 20 && a.a[1].at == 40);
    CHECK(strcmp(a.a[0].text, "intro.sfd") == 0);
    CHECK(a.a[0].period == 61 && a.a[0].retry.analog == 0x01
          && a.a[0].retry.hold == 30);
    CHECK(a.a[1].period == 0);
    /* Each step takes the last anchor at or before its start: ties after. */
    CHECK(s[0].anchor == 0);            /* f10 */
    CHECK(s[1].anchor == 2);            /* f45 */
    CHECK(s[2].anchor == 1);            /* f20, on the anchor's flip */
    CHECK(s[3].anchor == 2 && s[3].pad == 1);
    CHECK(s[4].anchor == 2);            /* f40 */

    /* A retry for another pad, and with two buttons. */
    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored("f5:@open:x:p2-start+a/30", s, 0, 16, &a);
    CHECK(n == 0 && a.n == 1 && a.a[0].retry.pad == 1
          && a.a[0].retry.digital == 0x10 && a.a[0].retry.analog == 0x01);

    /* Bad anchors end the parse, as a bad step does. */
    memset(&a, 0, sizeof a);
    CHECK(pad_script_parse_anchored("f1:a 100:@open:x f2:b", s, 0, 16, &a) == 1
          && a.n == 0);
    CHECK(pad_script_parse_anchored("f1:a f2:@open: f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@close:x f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:a f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:a/1 f3:b", s, 0, 16, &a) == 1);
    /* Past the cap, a retry pressing nothing or not a button, a pad on the
     * anchor rather than its retry. */
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:a/301 f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:zz/10 f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:/10 f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:@open:x:lx=100/10 f3:b", s, 0, 16, &a) == 1);
    CHECK(pad_script_parse_anchored("f1:a f2:p2-@open:x f3:b", s, 0, 16, &a) == 1);
    CHECK(a.n == 0);
    CHECK(pad_script_parse_anchored("f2:@open:x:a/300", s, 0, 16, &a) == 0 && a.n == 1);
    /* Without an anchor table the entry is skipped, the steps kept. */
    n = pad_script_parse("f1:a f2:@open:x:a/10 f3:b", s, 0, 16);
    CHECK(n == 2 && s[1].at == 3 && s[0].anchor == 0 && s[1].anchor == 0);
}

static void test_anchor_match(void)
{
    static PadAnchors a;
    PadStep s[4];

    /* A bare name is the start of the file name: "stop.sfd" is not op's. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:op.sfd f20:@open:movie\\end", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "d:\\movie\\stop.sfd", 5, NULL, 0) == -1);
    CHECK(pad_anchors_open(&a, "d:\\op\\x.sfd", 5, NULL, 0) == -1);
    CHECK(pad_anchors_open(&a, "d:\\movie\\OP.SFD", 6, NULL, 0) == 0);
    /* With a directory in it, anywhere in the path. */
    CHECK(pad_anchors_open(&a, "d:\\movie\\ending.sfd", 7, NULL, 0) == 1 && a.armed == 2);
    /* A prefix of the name takes it. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:intro", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "d:\\Intro.sfd", 5, NULL, 0) == 0);
}

static void test_anchor_clock(void)
{
    static PadAnchors a;
    PadStep s[4], r;
    int k;

    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:a:3 f100:@open:movie f110:b:4 "
                              "f200:@open:menu:a/20 f205:x:2",
                              s, 0, 4, &a);
    /* Before its open, an anchored step neither presses nor is done. */
    CHECK(pad_step_active_at(&s[0], &a, 0, 10));
    CHECK(!pad_step_active_at(&s[1], &a, 0, 110));
    CHECK(!pad_step_done_at(&s[1], &a, 0, 100000));
    /* Only the armed anchor takes an open while it is not late, and in
     * any case. */
    CHECK(pad_anchors_open(&a, "d:\\menu.xpr", 90, NULL, 0) == -1);
    CHECK(pad_anchors_open(&a, "d:\\MOVIE\\Movie.sfd", 150, NULL, 0) == 0);
    CHECK(a.armed == 1);
    /* 50 flips late: the step lands 10 flips after the open. */
    CHECK(!pad_step_active_at(&s[1], &a, 0, 159));
    CHECK(pad_step_active_at(&s[1], &a, 0, 160));
    CHECK(pad_step_active_at(&s[1], &a, 0, 163));
    CHECK(pad_step_done_at(&s[1], &a, 0, 164));
    /* The menu is due at flip 250 on the movie's clock. Not late before. */
    CHECK(!pad_anchor_retry(&a, 0, 249, &r, NULL, 0));
    /* Late: taps A for 10 flips every 20, from 10 flips after it is due. */
    CHECK(!pad_anchor_retry(&a, 0, 250, &r, NULL, 0));
    CHECK(!pad_anchor_retry(&a, 0, 259, &r, NULL, 0));
    CHECK(pad_anchor_retry(&a, 0, 260, &r, NULL, 0) && r.analog == 0x01);
    CHECK(pad_anchor_retry(&a, 0, 269, &r, NULL, 0));
    CHECK(!pad_anchor_retry(&a, 0, 270, &r, NULL, 0));
    CHECK(pad_anchor_retry(&a, 0, 280, &r, NULL, 0));
    CHECK(!pad_anchor_retry(&a, 1, 280, &r, NULL, 0));   /* pad 1's own */
    /* An open early on its clock pulls the next steps in. */
    k = pad_anchors_open(&a, "z:\\menu", 282, NULL, 0);
    CHECK(k == 1 && !pad_anchor_retry(&a, 0, 290, &r, NULL, 0));
    CHECK(pad_step_active_at(&s[2], &a, 0, 287) && !pad_step_active_at(&s[2], &a, 0, 286));
    /* A step on no anchor is on the raw count, as before. */
    CHECK(s[0].anchor == 0 && pad_step_done_at(&s[0], &a, 0, 13));
    /* Every anchor seen: no more opens taken. */
    CHECK(pad_anchors_open(&a, "movie", 300, NULL, 0) == -1);
}

static void test_anchor_give_up(void)
{
    static PadAnchors a;
    PadStep s[4], r;
    char log[512];

    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:movie f110:b:4 "
                              "f3000:@open:menu:a/20 f3005:x:2",
                              s, 0, 4, &a);
    /* The first is due on the raw count: given up 1800 flips late, its
     * steps on that count, as without anchors. */
    CHECK(pad_anchors_tick(&a, s, 2, 100 + PAD_ANCHOR_GIVE_UP_FLIPS - 1, NULL, 0) == 0
          && a.armed == 0);
    CHECK(pad_anchors_tick(&a, s, 2, 100 + PAD_ANCHOR_GIVE_UP_FLIPS, log, sizeof log) == 1
          && a.armed == 1);
    CHECK(strstr(log, "gave up on @open:movie") && strstr(log, "1 step(s) skipped"));
    CHECK(a.a[0].seen && a.a[0].seen_flip == 100);
    CHECK(pad_step_done_at(&s[0], &a, 0, 115));
    /* The next, due at 3000 on that clock, taps from 3010 every 20, for
     * PAD_RETRY_MAX_TAPS taps, then only waits. */
    CHECK(!pad_anchor_retry(&a, 0, 3009, &r, NULL, 0));
    CHECK(pad_anchor_retry(&a, 0, 3010, &r, NULL, 0));
    CHECK(pad_anchor_retry(&a, 0, 3010 + 20 * (PAD_RETRY_MAX_TAPS - 1), &r, NULL, 0));
    CHECK(!pad_anchor_retry(&a, 0, 3010 + 20 * PAD_RETRY_MAX_TAPS, &r, NULL, 0));
    CHECK(!pad_anchor_retry(&a, 0, 3010 + 20 * (PAD_RETRY_MAX_TAPS + 5), &r, NULL, 0));
    CHECK(pad_anchors_tick(&a, s, 2, 3000 + PAD_ANCHOR_GIVE_UP_FLIPS, NULL, 0) == 1
          && a.armed == 2);
    CHECK(pad_anchors_tick(&a, s, 2, 100000, NULL, 0) == 0);
    /* An open after it gave up is not taken. */
    CHECK(pad_anchors_open(&a, "menu", 5000, NULL, 0) == -1);

    /* A later anchor the restored clock has passed goes with it, silently:
     * its retry does not tap, nor wait 1800 flips more. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:movie f110:b:4 "
                              "f200:@open:menu:a/20 f205:x:2",
                              s, 0, 4, &a);
    CHECK(pad_anchors_tick(&a, s, 2, 100 + PAD_ANCHOR_GIVE_UP_FLIPS, log, sizeof log) == 2
          && a.armed == 2);
    CHECK(strstr(log, ", and 1 later anchor(s) it passed; 2 step(s) skipped"));
    CHECK(a.a[1].seen_flip == 200);

    /* A record's @end under a given-up anchor presses nothing: not a
     * skipped step. */
    memset(&a, 0, sizeof a);
    CHECK(pad_script_parse_anchored("f100:@open:movie f110:b:4 f120:@end",
                                    s, 0, 4, &a) == 2 && s[1].end && s[1].anchor);
    CHECK(pad_anchors_tick(&a, s, 2, 100 + PAD_ANCHOR_GIVE_UP_FLIPS, log, sizeof log) == 1
          && strstr(log, "; 1 step(s) skipped"));
    CHECK(!pad_anchor_retry(&a, 0, 100 + PAD_ANCHOR_GIVE_UP_FLIPS + 10, &r, NULL, 0));

    /* Several overdue at once all go, each read on the give-up's clock. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:a f20:@open:b f30:x", s, 0, 4, &a);
    CHECK(pad_anchors_tick(&a, s, 1, 5000, NULL, 0) == 2 && a.armed == 2);
    CHECK(a.a[1].seen_flip == 5000 - (5000 - 20));
}

static void test_anchor_skip_ahead(void)
{
    static PadAnchors a;
    PadStep s[4];
    char log[512];

    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:a f20:x:2 f50:@open:b f60:y:2 "
                              "f2000:@open:c",
                              s, 0, 4, &a);
    /* Not late yet: the order holds, b's open is not taken over a. */
    CHECK(pad_anchors_open(&a, "b", 5, NULL, 0) == -1 && a.armed == 0);
    /* Late, a later anchor's open gives up the ones before it. Its steps
     * passed: they are skipped, not pressed at once. */
    CHECK(pad_anchors_open(&a, "d:\\b.bin", 30, log, sizeof log) == 1 && a.armed == 2);
    CHECK(strstr(log, "gave up on 1 anchor(s) from @open:a")
          && strstr(log, "@open:b at flip 30"));
    /* The pad thread counts them, at its next tick. */
    CHECK(pad_anchors_tick(&a, s, 2, 31, log, sizeof log) == 0
          && strstr(log, "1 step(s) skipped by that give-up"));
    CHECK(pad_anchors_tick(&a, s, 2, 32, log, sizeof log) == 0 && !log[0]);
    CHECK(a.a[0].seen && pad_step_done_at(&s[0], &a, 0, 30));
    CHECK(!pad_step_active_at(&s[1], &a, 0, 39) && pad_step_active_at(&s[1], &a, 0, 40));
    /* Only within the give-up's wait of record: c is too far past b. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:a f2000:@open:c", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "c", 100, NULL, 0) == -1 && a.armed == 0);

    /* a, b, a: an open of a nearer the a seen than its next anchor is a
     * re-open (the recorder keeps one anchor for opens in a row), late b
     * or not. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:movie.sfd f590:start f600:@open:menu.xpr "
                              "f700:a f1500:@open:movie.sfd",
                              s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "d:\\movie.sfd", 100, NULL, 0) == 0);
    CHECK(pad_anchors_open(&a, "d:\\movie.sfd", 650, NULL, 0) == -1 && a.armed == 1);
    CHECK(pad_anchors_open(&a, "d:\\menu.xpr", 660, NULL, 0) == 1);
    CHECK(pad_anchors_open(&a, "d:\\movie.sfd", 1500, NULL, 0) == 2);
    /* Nearer a's next anchor it is that one's open, b's missed. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:a.bin f150:x f200:@open:b.bin f250:y "
                              "f300:@open:a.bin f310:start",
                              s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "d:\\a.bin", 100, NULL, 0) == 0);
    CHECK(pad_anchors_open(&a, "d:\\a.bin", 199, NULL, 0) == -1 && a.armed == 1);
    CHECK(pad_anchors_open(&a, "d:\\a.bin", 300, log, sizeof log) == 2 && a.armed == 3);
    CHECK(strstr(log, "gave up on 1 anchor(s) from @open:b.bin")
          && strstr(log, "(+0 flips against the record)"));

    /* Two opens on one record flip, come the other way while on time: the
     * second's open takes it, and the first's after it is a re-open. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:x.bin f100:@open:y.bin f110:a", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "y.bin", 98, NULL, 0) == 1 && a.armed == 2);
    CHECK(pad_anchors_open(&a, "x.bin", 99, NULL, 0) == -1);
    CHECK(pad_step_active_at(&s[0], &a, 0, 108) && !pad_step_active_at(&s[0], &a, 0, 107));
    /* A later record flip still waits its turn while on time. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:x.bin f101:@open:y.bin", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "y.bin", 98, NULL, 0) == -1 && a.armed == 0);
    /* x, y, x on one record flip, replayed x, x, y: the second x is the
     * third anchor, not a re-open; nothing was skipped. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f100:@open:x.bin f100:@open:y.bin f100:@open:x.bin "
                              "f110:a",
                              s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "x.bin", 100, NULL, 0) == 0);
    CHECK(pad_anchors_open(&a, "x.bin", 100, log, sizeof log) == 2 && a.armed == 3);
    CHECK(strstr(log, "@open:x.bin taken before @open:y.bin (same record flip)")
          && !strstr(log, "skipped"));
    CHECK(pad_step_active_at(&s[0], &a, 0, 110) && !pad_step_active_at(&s[0], &a, 0, 109));
}

static void test_anchor_log(void)
{
    static PadAnchors a;
    PadStep s[4], r;
    char log[512], small[40];
    size_t n;
    int i;

    /* The retry's lines go to the buffer, for the caller to print after
     * its lock. */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:a:a/20", s, 0, 4, &a);
    CHECK(!pad_anchor_retry(&a, 0, 10, &r, log, sizeof log)
          && strstr(log, "waiting on @open:a from flip 10"));
    CHECK(!pad_anchor_retry(&a, 0, 11, &r, log, sizeof log) && !log[0]);
    /* A full buffer still ends in a newline. */
    memset(&a, 0, sizeof a);
    a.n = 6;
    for (i = 0; i < a.n; i++) {
        memset(a.a[i].text, 'x', PAD_ANCHOR_TEXT - 1);
        a.a[i].at = 10 + (unsigned long)i * 3000;
    }
    CHECK(pad_anchors_tick(&a, s, 0, 100000, small, sizeof small) == 6);
    n = strlen(small);
    CHECK(n == sizeof small - 1 && small[n - 1] == '\n');
    /* A line that fills the buffer exactly still marks the one dropped:
     * a give-up's line, then the match's. */
    {
        char big[512], fit[512];
        size_t first = 0;
        int round;

        for (round = 0; round < 2; round++) {
            memset(&a, 0, sizeof a);
            pad_script_parse_anchored("f10:@open:a f50:@open:b", s, 0, 4, &a);
            if (!round) {
                CHECK(pad_anchors_open(&a, "b", 60, big, sizeof big) == 1);
                first = (size_t)(strchr(big, '\n') - big) + 1;
            } else {
                CHECK(pad_anchors_open(&a, "b", 60, fit, first + 1) == 1);
            }
        }
        n = strlen(fit);
        CHECK(first < strlen(big) && n == first && !strcmp(fit + n - 4, "...\n"));
    }
}

static void test_anchor_early_limit(void)
{
    static PadAnchors a;
    PadStep s[4];

    /* A step under the anchor before keeps its clock when the next open
     * comes early: y, after x in the record, presses before it (Limits). */
    memset(&a, 0, sizeof a);
    pad_script_parse_anchored("f10:@open:a f25:x:2 f30:@open:b f35:y:2", s, 0, 4, &a);
    CHECK(pad_anchors_open(&a, "a", 10, NULL, 0) == 0);
    CHECK(pad_anchors_open(&a, "b", 15, NULL, 0) == 1);
    CHECK(pad_step_active_at(&s[1], &a, 0, 20) && !pad_step_active_at(&s[0], &a, 0, 20));
    CHECK(pad_step_active_at(&s[0], &a, 0, 25));
}

static void test_recorder_anchor(void)
{
    PadRecorder rec[2];
    char out[256];
    uint32_t at = 0;
    int f;

    memset(rec, 0, sizeof rec);
    rec[1].pad = 1;
    /* A tapped every 10 flips for 4, from flip 10. */
    for (f = 0; f < 36; f++) {
        pad_recorder_feed(&rec[0], bits(0, (f % 10) < 4 && f >= 10 ? 1 : 0, 0, 0),
                          (uint32_t)f, out, sizeof out);
        pad_recorder_feed(&rec[1], bits(0, 0, 0, 0), (uint32_t)f, out, sizeof out);
    }
    /* The open comes at flip 36, 6 after the last A started. */
    CHECK(pad_recorder_anchor(rec, 2, "d:\\movie\\intro.sfd", 36, 0, out,
                              sizeof out, &at) > 0);
    CHECK(strcmp(out, "# retry a/10: pressed at f10 f20 f30, the open 6 flips"
                      " after the last\nf36:@open:intro.sfd:a/10\n") == 0 && at == 36);
    /* Presses before the previous anchor are not its retry. */
    CHECK(pad_recorder_anchor(rec, 2, "x.bin", 36, 11, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "f36:@open:x.bin\n") == 0);
    /* An open later than one spacing after the last is no retry. */
    CHECK(pad_recorder_anchor(rec, 2, "x.bin", 43, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "# no retry a: pressed at f10 f20 f30 (gaps 10 10), the open 13"
                      " flips after the last\nf43:@open:x.bin\n") == 0);
    /* Presses long before the open are a menu walk: no note. */
    CHECK(pad_recorder_anchor(rec, 2, "x.bin", 61, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "f61:@open:x.bin\n") == 0);
    /* A press polled earlier in the open's flip stays before it: the anchor
     * goes a flip on. The press under gathering counts for the retry. */
    pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), 36, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), 37, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 0x01, 0, 0), 40, out, sizeof out);
    CHECK(pad_recorder_anchor(rec, 2, "menu", 40, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "# retry a/10: pressed at f20 f30 f40, the open 0 flips"
                      " after the last\nf41:@open:menu:a/10\n") == 0 && at == 41);

    /* Pad 2's retry is named for it. */
    memset(rec, 0, sizeof rec);
    rec[1].pad = 1;
    for (f = 0; f < 6; f++)
        pad_recorder_feed(&rec[1], bits(f & 1 ? 0 : 0x10, 0, 0, 0),
                          (uint32_t)(100 + f * 8), out, sizeof out);
    CHECK(pad_recorder_anchor(rec, 2, "a", 140, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "# retry p2-start/16: pressed at f100 f116 f132, the open 8"
                      " flips after the last\nf140:@open:a:p2-start/16\n") == 0);

    /* Menu choices are no retry: two presses, uneven ones, other buttons. */
    memset(rec, 0, sizeof rec);
    pad_recorder_feed(&rec[0], bits(0, 1, 0, 0), 10, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), 12, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 1, 0, 0), 30, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), 32, out, sizeof out);
    CHECK(pad_recorder_anchor(rec, 1, "a", 40, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "f40:@open:a\n") == 0);
    pad_recorder_feed(&rec[0], bits(0, 1, 0, 0), 80, out, sizeof out);
    pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), 82, out, sizeof out);
    CHECK(pad_recorder_anchor(rec, 1, "a", 90, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "# no retry a: pressed at f10 f30 f80 (gaps 20 50), the open 10"
                      " flips after the last\nf90:@open:a\n") == 0);
    memset(rec, 0, sizeof rec);
    for (f = 0; f < 3; f++) {
        pad_recorder_feed(&rec[0], bits(f == 1 ? 0x01 : 0, f == 1 ? 0 : 1, 0, 0),
                          (uint32_t)(10 + f * 10), out, sizeof out);
        pad_recorder_feed(&rec[0], bits(0, 0, 0, 0), (uint32_t)(12 + f * 10), out,
                          sizeof out);
    }
    CHECK(pad_recorder_anchor(rec, 1, "a", 35, 0, out, sizeof out, &at) > 0);
    CHECK(strcmp(out, "f35:@open:a\n") == 0);
    /* Nor presses past the cap apart. */
    memset(rec, 0, sizeof rec);
    for (f = 0; f < 3; f++) {
        pad_recorder_feed(&rec[0], bits(0, 1, 0, 0),
                          (uint32_t)(1 + f * (PAD_RETRY_MAX_FLIPS + 1)), out, sizeof out);
        pad_recorder_feed(&rec[0], bits(0, 0, 0, 0),
                          (uint32_t)(2 + f * (PAD_RETRY_MAX_FLIPS + 1)), out, sizeof out);
    }
    CHECK(pad_recorder_anchor(rec, 1, "a", 2 * PAD_RETRY_MAX_FLIPS + 10, 0, out,
                              sizeof out, &at) > 0);
    CHECK(strncmp(out, "# no retry a:", 13) == 0 && !strchr(out, '/'));
    /* A line past the buffer is -1, not a bad name. */
    CHECK(pad_recorder_anchor(rec, 1, "a", 2 * PAD_RETRY_MAX_FLIPS + 10, 0, out, 20, &at)
          == -1);
    /* Names that cannot be a text. */
    CHECK(pad_recorder_anchor(rec, 1, "d:\\my file.bin", 1, 0, out, sizeof out, &at) == 0);
    CHECK(pad_recorder_anchor(rec, 1, "d:\\dir\\", 1, 0, out, sizeof out, &at) == 0);
    CHECK(pad_recorder_anchor(rec, 1, "a,b", 1, 0, out, sizeof out, &at) == 0);
    CHECK(strcmp(pad_path_name("d:\\movie\\x.sfd"), "x.sfd") == 0);
    CHECK(strcmp(pad_path_name("x.sfd"), "x.sfd") == 0);
}

/* The poll clock: g<poll> steps, one clock a script, analog
 * depth, and a record that gives back every poll's state exactly. */
static void test_poll_clock(void)
{
    static PadAnchors a;
    PadStep s[8];
    uint8_t r[20];
    int n;

    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored("g10:a:3 g20:@open:stage g25:lt=97+rt:2 f30:b",
                                  s, 0, 8, &a);
    /* The f step is on the other clock: refused, the rest kept. */
    CHECK(n == 2 && a.clock == 2 && a.n == 1);
    CHECK(s[0].by_flip == 2 && s[0].at == 10 && s[0].hold == 3);
    CHECK(s[1].analog == 0xC0 && s[1].aval[6] == 97 && s[1].aval[7] == 0);
    CHECK(s[1].anchor == 1);
    report(r, 0, 0, 0, 0);
    pad_step_press(&s[1], r);
    CHECK(r[4 + 6] == 97 && r[4 + 7] == 0xFF);
    /* Depth out of range, or on a button with none, is not a button. */
    memset(&a, 0, sizeof a);
    CHECK(pad_script_parse("g1:lt=0", s, 0, 8) == 1 && !s[0].analog);
    CHECK(pad_script_parse("g1:lt=256", s, 0, 8) == 1 && !s[0].analog);
    CHECK(pad_script_parse("g1:start=9", s, 0, 8) == 1 && !s[0].digital);
    CHECK(pad_script_parse("g1:a=255", s, 0, 8) == 1 && s[0].analog == 0x01
          && s[0].aval[0] == 0);
}

static void test_poll_round_trip(void)
{
    /* What one jittery hand gives a title in successive polls: small stick
     * moves inside the flip clock's band, a light trigger, a press under
     * the trigger threshold. The flip record would round them away. */
    static const struct { uint8_t a0, lt; int16_t lx, ly; } seq[] = {
        {0, 0, 2330, -1568}, {0, 0, 2400, -1568}, {0, 0, 2410, -1500},
        {0, 97, 2410, -1500}, {0, 158, 2410, -1500}, {0, 255, 15565, 31489},
        {12, 255, 16367, 31151}, {200, 0, 16367, 31151}, {0, 0, 0, 0},
        {0, 2, 0, 0}, {0, 0, -12402, -8703},
    };
    enum { N = sizeof seq / sizeof seq[0] };
    PadRecorder rec;
    static PadAnchors a;
    PadStep steps[64];
    char text[4096] = "", out[512];
    int i, n;

    memset(&rec, 0, sizeof rec);
    rec.poll = 1;
    for (i = 0; i < N; i++) {
        uint8_t r[20];
        report(r, 0, 0, seq[i].lx, seq[i].ly);
        r[4] = seq[i].a0;
        r[4 + 6] = seq[i].lt;
        if (pad_recorder_feed(&rec, pad_bits_from_report(r), (uint32_t)(50 + i),
                              out, sizeof out))
            strcat(text, out);
    }
    if (pad_recorder_end(&rec, 50 + N, out, sizeof out))
        strcat(text, out);
    CHECK(text[0] == 'g' && !strchr(text, 'f'));
    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored(text, steps, 0, 64, &a);
    CHECK(n > 0 && a.clock == 2);
    for (i = 0; i < N; i++) {
        uint8_t want[20], got[20];
        int k;
        report(want, 0, 0, seq[i].lx, seq[i].ly);
        want[4] = seq[i].a0;
        want[4 + 6] = seq[i].lt;
        report(got, 0, 0, 0, 0);
        for (k = 0; k < n; k++)
            if (pad_step_active_at(&steps[k], &a, 0, (uint32_t)(50 + i)))
                pad_step_press(&steps[k], got);
        if (memcmp(want, got, 20) != 0) {
            fails++;
            fprintf(stderr, "FAIL poll round trip at poll %d\n%s", 50 + i, text);
        }
    }
}

/* An anchor on the poll clock is written g<poll>:@open. */
static void test_poll_anchor(void)
{
    PadRecorder rec[1];
    char line[512];
    uint32_t at = 0;
    uint8_t r[20];

    memset(rec, 0, sizeof rec);
    rec[0].poll = 1;
    report(r, 0, 0, 0, 0);
    pad_recorder_feed(&rec[0], pad_bits_from_report(r), 7, line, sizeof line);
    CHECK(pad_recorder_anchor(rec, 1, "d:\\adx\\jingle_stagestart.adx", 8, 0,
                              line, sizeof line, &at) > 0);
    CHECK(at == 8 && strcmp(line, "g8:@open:jingle_stagestart.adx\n") == 0);
}

/* The record's end, @end: a step that presses nothing, done on its own
 * count, written past every pad's last report. */
static void test_end_marker(void)
{
    static PadAnchors a;
    PadStep s[8];
    PadRecorder rec[2];
    char line[64], out[256];
    uint8_t r[20];
    int n;

    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored("g10:a:4,g40:@end", s, 0, 8, &a);
    CHECK(n == 2 && a.clock == 2 && s[1].end && s[1].hold == 0 && s[1].at == 40);
    CHECK(!pad_step_active(&s[1], 0, 40) && !pad_step_done(&s[1], 0, 39)
          && pad_step_done(&s[1], 0, 40));
    report(r, 0, 0, 0, 0);
    pad_step_press(&s[1], r);
    CHECK(r[2] == 0 && r[4] == 0);
    /* One clock, as for any entry; no pad prefix; a word, not a prefix. */
    memset(&a, 0, sizeof a);
    CHECK(pad_script_parse_anchored("g10:a,f40:@end", s, 0, 8, &a) == 1);
    CHECK(pad_script_parse("f40:p2-@end", s, 0, 8) == 0);
    CHECK(pad_script_parse("40:@end", s, 0, 8) == 0);
    memset(&a, 0, sizeof a);
    CHECK(pad_script_parse_anchored("f40:@ending:x", s, 0, 8, &a) == 0);

    /* Nothing reported: nothing recorded, no end. */
    memset(rec, 0, sizeof rec);
    rec[0].poll = rec[1].poll = 1;
    rec[1].pad = 1;
    CHECK(pad_recorder_end_line(rec, 2, 50, line, sizeof line) == 0 && !line[0]);
    /* The end goes past the last report, or at the count given. */
    pad_recorder_feed(&rec[0], bits(0, 1, 0, 0), 60, out, sizeof out);
    CHECK(pad_recorder_end_line(rec, 2, 50, line, sizeof line) > 0
          && strcmp(line, "g61:@end\n") == 0);
    CHECK(pad_recorder_end_line(rec, 2, 90, line, sizeof line) > 0
          && strcmp(line, "g90:@end\n") == 0);
    CHECK(pad_recorder_end_line(rec, 2, 90, line, 5) == 0 && !line[0]);
    /* A step that does not fit leaves no cut line behind: the recorder
     * formats without snprintf (signal-safe) and writes a line whole. */
    {
        PadRecorder one;
        memset(&one, 0, sizeof one);
        one.poll = 1;
        pad_recorder_feed(&one, bits(1, 0, 0, 0), 10, out, sizeof out);
        pad_recorder_feed(&one, bits(0, 0, 0, 0), 14, out, sizeof out);
        CHECK(pad_recorder_feed(&one, bits(0, 0, 0, 0), 15, line, 6) == 0
              && !line[0]);
        memset(&one, 0, sizeof one);
        one.poll = 1;
        one.pad = 2;
        pad_recorder_feed(&one, bits(1, 0, 0, 0), 4000000000u, out, sizeof out);
        CHECK(pad_recorder_end(&one, 4000000007u, out, sizeof out) == 1
              && strcmp(out, "g4000000000:p3-up:7\n") == 0);
        /* A negative axis, and a poll-clock depth, exactly. */
        memset(&one, 0, sizeof one);
        pad_recorder_feed(&one, bits(0, 0, -32768, 0), 10, out, sizeof out);
        CHECK(pad_recorder_end(&one, 12, out, sizeof out) == 1
              && strcmp(out, "f10:lx=-32768:2\n") == 0);
        {
            PadBits lt = bits(0, 0, 0, 0);
            lt.av[6] = 97;   /* lt, a light press */
            memset(&one, 0, sizeof one);
            one.poll = 1;
            pad_recorder_feed(&one, lt, 10, out, sizeof out);
            CHECK(pad_recorder_end(&one, 13, out, sizeof out) == 1
                  && strcmp(out, "g10:lt=97:3\n") == 0);
        }
    }
    rec[0].poll = 0;
    CHECK(pad_recorder_end_line(rec, 1, 90, line, sizeof line) > 0
          && strcmp(line, "f90:@end\n") == 0);
}

/* On the poll clock a trigger's depth moving under a held finger is a new
 * step but not a new press: it must not fill the retry's history. */
static void test_poll_depth_not_a_press(void)
{
    PadRecorder rec;
    char out[512];
    uint8_t r[20];
    uint32_t t = 10;
    int i;

    memset(&rec, 0, sizeof rec);
    rec.poll = 1;
    report(r, 0, 0, 0, 0);
    pad_recorder_feed(&rec, pad_bits_from_report(r), t++, out, sizeof out);
    r[4 + 6] = 90;                       /* lt pressed lightly: one press */
    pad_recorder_feed(&rec, pad_bits_from_report(r), t++, out, sizeof out);
    for (i = 0; i < 6; i++) {            /* its depth wavers */
        r[4 + 6] = (uint8_t)(91 + i);
        pad_recorder_feed(&rec, pad_bits_from_report(r), t++, out, sizeof out);
    }
    r[4 + 6] = 0;
    pad_recorder_feed(&rec, pad_bits_from_report(r), t++, out, sizeof out);
    pad_recorder_feed(&rec, pad_bits_from_report(r), t++, out, sizeof out);
    CHECK(rec.presses == 1 && rec.press_at[0] == 11);
}

/* The recorded files the replays depend on keep parsing: the pad_record
 * header, flip (f) and poll (g) clocks, anchors, comment lines the user adds,
 * steps written out of order (a stick step is written when it ends) and
 * @end. Each excerpt is shaped like one of the BLiNX 2 clip records; every
 * line that is not a comment is a step or an anchor. */
static int count_entries(const char *text)
{
    int n = 0, start = 1;
    for (const char *c = text; *c; c++) {
        if (start && *c != '#' && *c != '\n')
            n++;
        start = *c == '\n';
    }
    return n;
}

static void check_record(const char *text, int clock, int anchors)
{
    static PadAnchors a;
    static PadStep s[PAD_SCRIPT_MAX];
    int n;

    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored(text, s, 0, PAD_SCRIPT_MAX, &a);
    CHECK(n + a.n == count_entries(text));
    CHECK(a.clock == clock);
    if (anchors >= 0)
        CHECK(a.n == anchors);
    for (int i = 0; i < n; i++)
        CHECK(s[i].by_flip == clock);
}

static void test_record_formats(void)
{
    static PadStep s[8];
    static PadAnchors a;
    int n;

    /* clip: flip clock, no anchors. */
    check_record("# pad_record: pad_script steps timed by flip (f<flip>:<buttons>"
                 ":<hold flips>, sticks lx=<v>)\n"
                 "f116:a:9\nf168:a:8\n"
                 "f114:lx=3107+ly=-1766+rx=-1037+ry=-2720:376\n"
                 "f490:lx=3107+ly=-2167+rx=-1037+ry=-2775:2\n", 1, 0);
    /* clip2: flip clock with anchors, out of order. */
    check_record("# pad_record: pad_script steps timed by flip (f<flip>:<buttons>"
                 ":<hold flips>, sticks lx=<v>)\n"
                 "f0:@open:logo_mgs.sfd\n"
                 "f0:lx=2330+ly=-1568+rx=-254+ry=-2069:33\n"
                 "f32:start:9\nf83:a:13\n"
                 "f187:@open:blinx2_opening.sfd\n"
                 "f305:a:8\n"
                 "f50:lx=2330+ly=-1568+rx=-136+ry=-2997:267\n", 1, 2);
    /* clip3: poll clock, a comment between steps, sticks, @end. */
    check_record("# pad_record: pad_script steps timed by the title's pad polls "
                 "(g<poll>:<buttons>:<hold polls>, sticks lx=<v>, analog depth "
                 "lt=<v>)\n"
                 "g2:@open:logo_mgs.sfd\n"
                 "g0:ly=-1+rx=25108+ry=29163:1\n"
                 "g56:start:8\n"
                 "# no retry start: pressed at f56 f121 f166 (gaps 65 45)\n"
                 "g211:@open:blinx2_opening.sfd\n"
                 "g369:@open:song_TITLE.adx\n"
                 "g131:lx=2460+ly=-1114+rx=-445+ry=-2197:287\n"
                 "g4381:start:6\ng4428:@end\n", 2, 3);

    /* An out-of-order stick step counts from the anchor its start is past,
     * not the one written before it. */
    memset(&a, 0, sizeof a);
    n = pad_script_parse_anchored("g2:@open:x.sfd\ng211:@open:y.sfd\n"
                                  "g250:a:2\ng131:lx=100:200\n", s, 0, 8, &a);
    CHECK(n == 2 && a.n == 2 && s[0].anchor == 2 && s[1].anchor == 1);
}

/* Each argument is a pad file to check the same way (the recorded clips are
 * game data and stay out of the tree): its first timed line sets the clock. */
static void check_files(int argc, char **argv)
{
    static char buf[4 << 20];
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        size_t len;
        const char *c;
        if (!f) {
            fprintf(stderr, "pad_script: cannot open %s\n", argv[i]);
            fails++;
            continue;
        }
        len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;
        for (c = buf; *c == '#' && strchr(c, '\n'); c = strchr(c, '\n') + 1)
            ;
        int before = fails;
        /* g: guest vblanks, f: flips, a leading digit: milliseconds. */
        check_record(buf, *c == 'g' ? 2 : (*c >= '0' && *c <= '9') ? 0 : 1, -1);
        printf("pad_script: %s %s\n", argv[i], fails == before ? "ok" : "FAILED");
    }
}

int main(int argc, char **argv)
{
    test_parse();
    test_timing();
    test_truncation();
    test_press();
    test_bits();
    test_recorder();
    test_round_trip();
    test_anchor_parse();
    test_anchor_match();
    test_anchor_clock();
    test_anchor_give_up();
    test_anchor_skip_ahead();
    test_anchor_early_limit();
    test_anchor_log();
    test_recorder_anchor();
    test_poll_clock();
    test_poll_round_trip();
    test_poll_anchor();
    test_end_marker();
    test_poll_depth_not_a_press();
    test_record_formats();
    check_files(argc, argv);
    if (fails) {
        fprintf(stderr, "pad_script: %d failure(s)\n", fails);
        return 1;
    }
    printf("pad_script: all passed\n");
    return 0;
}
