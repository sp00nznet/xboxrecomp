/*
 * The presenter's window-close teardown leaves the APU's audio stream alone.
 *
 * A thread stands in for the APU frame thread: it queries the queue depth and
 * hands over a period whenever the queue is below two of them, as the pacer
 * does. The main thread brings video up, as the presenter does, then tears
 * it down with XBOX_PRESENT_SDL_QUIT while that thread runs. The stream must
 * outlive the teardown: audio stays initialised, and the thread keeps
 * feeding a queue that keeps draining.
 *
 * With "sdl_quit" as the argument the main thread calls SDL_Quit instead,
 * the presenter's old teardown; that run is expected to crash (by hand, not
 * a ctest).
 */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>
#include "apu_xaudio2.h"
#include "present_sdl_quit.h"

#define PERIOD 256

static volatile int s_stop;
static volatile long s_submits;

static void nap_ms(int ms)
{
    struct timespec ts = { 0, (long)ms * 1000000L };
    nanosleep(&ts, NULL);
}

static void *frame_thread(void *arg)
{
    static int16_t silence[PERIOD * 2];
    (void)arg;
    while (!s_stop) {
        if (xa2_queued_samples() < 2 * PERIOD && xa2_submit_samples(silence, PERIOD))
            s_submits++;
        nap_ms(1);
    }
    return NULL;
}

int main(int argc, char **argv)
{
    int old = argc > 1 && !strcmp(argv[1], "sdl_quit");
    const char *driver;
    pthread_t th;
    long before, after;
    int fail = 0;

    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        printf("SKIP: no SDL video (%s)\n", SDL_GetError());
        return 77;
    }
    if (!xa2_init()) {
        printf("SKIP: no SDL audio\n");
        return 77;
    }
    driver = SDL_GetCurrentAudioDriver();
    if (!driver || strcmp(driver, "disk")) {
        /* The dummy driver plays on a virtual clock and never queries the
         * stream, which is how the crash hid from every headless run. */
        printf("SKIP: audio driver is %s, not disk\n", driver ? driver : "?");
        xa2_shutdown();
        return 77;
    }

    pthread_create(&th, NULL, frame_thread, NULL);
    nap_ms(200);
    before = s_submits;
    if (old)
        SDL_Quit();
    else
        SDL_QuitSubSystem(XBOX_PRESENT_SDL_QUIT);
    nap_ms(200);
    after = s_submits;
    s_stop = 1;
    pthread_join(th, NULL);

    if (before == 0) {
        printf("FAIL: nothing submitted before the teardown\n");
        fail = 1;
    }
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        printf("FAIL: the teardown took audio down\n");
        fail = 1;
    }
    /* 200 ms at 48 kHz drains about 37 periods; the queue only refills if it
     * drained, so a stalled or freed stream shows as no new submits. */
    if (after - before < 10) {
        printf("FAIL: %ld periods submitted after the teardown, want 10 or more\n",
               after - before);
        fail = 1;
    }
    xa2_shutdown();
    SDL_Quit();
    if (!fail)
        printf("PASS: %ld periods before the teardown, %ld after\n", before, after - before);
    return fail;
}
