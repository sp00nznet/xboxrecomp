/*
 * Opens from two game threads at once: every one is matched, in order.
 * An anchor matched twice or lost would leave the last anchor unseen and
 * the step after it waiting for the give-up. Its own binary, since the
 * runtime reads RECOMP_DEBUG once per process.
 */
#include "usb_gamepad.h"
#include "xinput_xbox.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define SCRIPT_PATH "usb_pad_anchor_threads.txt"
#define ANCHORS 200
#define OPENS_EACH (ANCHORS / 2)

static int failures;
/* No pthread_barrier on macOS: both spin on a count to start together. */
static int s_ready;

static void check(int ok, const char *what)
{
    printf("  %-60s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void *opener(void *arg)
{
    int i;
    (void)arg;
    __atomic_add_fetch(&s_ready, 1, __ATOMIC_ACQ_REL);
    while (__atomic_load_n(&s_ready, __ATOMIC_ACQUIRE) < 2)
        ;
    for (i = 0; i < OPENS_EACH; i++)
        usb_gamepad_file_open("d:\\data\\x.bin", 0);
    return NULL;
}

int main(void)
{
    pthread_t th[2];
    XBOX_GAMEPAD g;
    FILE *f;
    uint32_t fl;
    int i, first = -1;

    /* Anchors at flips 10..209, all on x.bin, then B at 300: on the last
     * one's clock, 91 flips after the opens at flip 0. */
    f = fopen(SCRIPT_PATH, "w");
    if (!f)
        return 1;
    for (i = 0; i < ANCHORS; i++)
        fprintf(f, "f%d:@open:x.bin\n", 10 + i);
    fprintf(f, "f300:b:2\n");
    fclose(f);
    setenv("RECOMP_DEBUG", "pad_script=@" SCRIPT_PATH, 1);
    printf("usb_pad_anchor_threads\n");

    usb_gamepad_flip(0);
    for (i = 0; i < 2; i++)
        pthread_create(&th[i], NULL, opener, NULL);
    for (i = 0; i < 2; i++)
        pthread_join(th[i], NULL);

    for (fl = 0; fl < 200 && first < 0; fl++) {
        usb_gamepad_flip(fl);
        memset(&g, 0, sizeof g);
        usb_gamepad_game_press(0, &g);
        if (g.bAnalogButtons[1] == 0xFF)
            first = (int)fl;
    }
    check(first == 91, "every open matched once: B on the last anchor's clock");
    remove(SCRIPT_PATH);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
