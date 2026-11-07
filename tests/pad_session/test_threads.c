/*
 * pad_session across its three threads (pad-recording-ui R7), through the
 * calls the game's tap makes: a pad thread polling port 0, a game thread
 * opening files (anchors), and the menu thread starting, arming, releasing
 * and stopping records and playbacks under them. Built once more with
 * -DPAD_SESSION_TSAN=ON (tests/posix_ctest.sh), where a race is a report.
 *
 * The game's opens are pad_anchor files, so records write @open lines and
 * playbacks hold on them while the opener runs.
 *
 * Also the record cap (R5): a record that reaches PAD_STEPS_MAX stops
 * itself with room for its @end, and plays back whole.
 */
#include "pad_session.h"
#include "pad_script.h"
#include "usb_gamepad.h"
#include "xinput_xbox.h"
#include "recomp_env.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define CYCLES 40
#define FULL_PATH "pad_session_full.pad"
#define LOG_PATH "pad_session_threads.log"

static int failures;
static int s_quit;
static uint32_t s_polled;

static void check(int ok, const char *what)
{
    printf("  %-64s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok)
        failures++;
}

static int state(void)
{
    PadrecStatus st;
    padrec_status(&st);
    return st.state;
}

static void *pad_thread(void *arg)
{
    XBOX_GAMEPAD g;
    uint32_t i = 0;
    (void)arg;
    while (!__atomic_load_n(&s_quit, __ATOMIC_ACQUIRE)) {
        memset(&g, 0, sizeof g);
        if ((i / 3) & 1)
            g.bAnalogButtons[XBOX_BUTTON_A] = 0xFF;
        usb_gamepad_game_press(0, &g);
        usb_gamepad_game_record(0, &g);
        __atomic_store_n(&s_polled, ++i, __ATOMIC_RELEASE);
        sched_yield();
    }
    return NULL;
}

/* The game's file opens, and the walker's flips beside them. x.bin and
 * y.bin are pad_anchor files (main): a record writes an @open line for
 * each change between them, and its playback holds its steps on them, so
 * these opens race the swap and the pad thread's anchor reads. One every
 * four flips keeps a record well under PAD_ANCHOR_MAX. */
static void *game_thread(void *arg)
{
    uint32_t flip = 0;
    (void)arg;
    while (!__atomic_load_n(&s_quit, __ATOMIC_ACQUIRE)) {
        usb_gamepad_flip(++flip);
        if ((flip & 7) == 0)
            usb_gamepad_file_open("d:\\data\\x.bin", 0);
        else if ((flip & 7) == 4)
            usb_gamepad_file_open("d:\\data\\y.bin", 0);
        usb_gamepad_file_open("d:\\data\\z.bin", 0);
        sched_yield();
    }
    return NULL;
}

/* n more polls on the pad thread, or a 5 s timeout: 0 then. */
static int wait_polls(uint32_t n)
{
    uint32_t want = __atomic_load_n(&s_polled, __ATOMIC_ACQUIRE) + n;
    time_t t0 = time(NULL);
    while (__atomic_load_n(&s_polled, __ATOMIC_ACQUIRE) < want) {
        if (time(NULL) - t0 > 5)
            return 0;
        sched_yield();
    }
    return 1;
}

static int has_anchor(const char *path)
{
    char buf[4096];
    size_t n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    n = fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, "@open") != NULL;
}

static int ends_with_end(const char *path)
{
    char buf[64];
    long n;
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, n > 32 ? n - 32 : 0, SEEK_SET);
    n = (long)fread(buf, 1, sizeof buf - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, "@end") != NULL;
}

static void test_threads(void)
{
    pthread_t pad, game;
    char path[64], why[160];
    const char *one[1];
    int i, polled = 1, ended = 1, started = 1, anchored = 0, wrapped = 0, err;

    printf("three threads\n");
    /* The anchor logs go to a file, read after: an open read on clocks
     * older than the swap's origin showed as a give-up at flip 4294967295. */
    fflush(stderr);
    err = dup(2);
    if (freopen(LOG_PATH, "w", stderr) == NULL)
        err = -1;
    pthread_create(&pad, NULL, pad_thread, NULL);
    pthread_create(&game, NULL, game_thread, NULL);
    for (i = 0; i < CYCLES; i++) {
        unsigned armed = (i & 1) ? PADREC_ARMED : 0;
        PadrecStatus st;
        snprintf(path, sizeof path, "pad_session_thr%02d.pad", i);
        remove(path);
        started &= padrec_record_start(path, NULL, armed, why, sizeof why) == 0;
        polled &= wait_polls(2);
        if (armed)
            padrec_release();
        polled &= wait_polls(20);
        padrec_status(&st);
        padrec_record_stop();
        polled &= wait_polls(2);
        ended &= ends_with_end(path);
        anchored += has_anchor(path);

        one[0] = path;
        started &= padrec_play(one, 1, armed, why, sizeof why) == 0;
        if (armed)
            padrec_release();
        polled &= wait_polls(5 + (uint32_t)(i % 7));
        padrec_status(&st);
        padrec_stop();
        polled &= wait_polls(2);
        ended &= !padrec_playing();
        remove(path);
    }
    __atomic_store_n(&s_quit, 1, __ATOMIC_RELEASE);
    pthread_join(pad, NULL);
    pthread_join(game, NULL);
    fflush(stderr);
    if (err >= 0) {
        dup2(err, 2);
        close(err);
    }
    {
        FILE *f = fopen(LOG_PATH, "rb");
        char line[600];
        while (f && fgets(line, sizeof line, f))
            if (strstr(line, "flip 42949"))
                wrapped = 1;
        if (f)
            fclose(f);
        remove(LOG_PATH);
    }
    check(!wrapped, "no open is read before its session's origin");
    check(started, "every record and playback started");
    check(polled, "the pad thread kept polling");
    check(ended, "every record ended with @end, every playback stopped");
    check(anchored > 0, "records wrote anchors, so playbacks held on them");
    if (!anchored)
        printf("    no record had an @open line\n");
}

static void test_full(void)
{
    XBOX_GAMEPAD g;
    PadrecStatus st;
    const char *one[1] = { FULL_PATH };
    char why[160];
    uint32_t i;

    printf("the record cap\n");
    remove(FULL_PATH);
    check(padrec_record_start(FULL_PATH, NULL, 0, why, sizeof why) == 0, "a record starts");
    /* The status is the last session's until the next poll swaps this in.
     * A toggled every poll: a step (a:1) each two, so the cap comes at
     * about 2 * PAD_STEPS_MAX polls. */
    for (i = 0; i < 3u * PAD_STEPS_MAX && (i < 2 || state() != PADREC_STOPPED); i++) {
        memset(&g, 0, sizeof g);
        if (i & 1)
            g.bAnalogButtons[XBOX_BUTTON_A] = 0xFF;
        usb_gamepad_game_press(0, &g);
        usb_gamepad_game_record(0, &g);
    }
    padrec_status(&st);
    check(st.state == PADREC_STOPPED && st.full, "it stops itself, full");
    check(strstr(st.warn, "full") != NULL, "and says so");
    check(ends_with_end(FULL_PATH), "its last line is @end");
    why[0] = 0;
    check(padrec_play(one, 1, 0, why, sizeof why) == 0, "it plays back whole (under the cap)");
    if (why[0])
        printf("    why: %s\n", why);
    padrec_stop();
    memset(&g, 0, sizeof g);
    usb_gamepad_game_press(0, &g);
    remove(FULL_PATH);
}

int main(void)
{
    /* Read once per process: anchors on for the records and playbacks. */
    setenv("RECOMP_DEBUG", "pad_anchor=x.bin|y.bin", 1);
    recomp_env_init();
    printf("pad_session_threads\n");
    test_threads();
    test_full();
    padrec_reset_for_test();
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
