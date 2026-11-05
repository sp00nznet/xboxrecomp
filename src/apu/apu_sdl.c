/**
 * SDL3 Audio Output Backend (POSIX hosts)
 *
 * The xa2_* contract of apu_xaudio2.h on macOS and Linux: the APU frame
 * thread hands over one EP period (256 stereo frames) at a time, and the
 * device queue paces it (mcpx_apu_pace_step reads xa2_queued_samples).
 * An SDL audio stream bound to the default device is the queue, fed with
 * SDL_PutAudioStreamData (push, not a callback: the pacer owns the timing);
 * the device thread pulls one device buffer at a time and pads a short pull
 * with silence, which is why the pacing marks keep at least one device
 * buffer queued.
 *
 * Headless runs (RECOMP_HEADLESS=1) use SDL's "dummy" driver unless
 * SDL_AUDIO_DRIVER is already set, so nothing reaches the speakers.
 * SDL_AUDIO_DRIVER=coreaudio (or pulseaudio, pipewire) with RECOMP_HEADLESS=1
 * is the way to hear a headless run. The dummy driver's own drain is a sleep
 * loop that runs 1-7% off 48 kHz and moves with load, and the
 * pacing would make that the guest's audio clock; on it the queue is a
 * virtual device clocked by CLOCK_MONOTONIC instead (apu_vclock.h), and the
 * SDL stream stays paused and empty.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "apu_xaudio2.h"
#include "apu_vclock.h"
#include "recomp_env.h"

#if !defined(_WIN32)

#include <SDL3/SDL.h>
#include <time.h>
#include "platform/sdl_env.h"

#define SDLA_SAMPLE_RATE   48000
#define SDLA_CHANNELS      2
/* Device buffer size and count: the same defaults (apu_xaudio2.h) and knobs
 * as the XAudio2 backend, with no Wine case. The count is a pacing mark
 * here, not a slot limit: the stream is unbounded. */
#define SDLA_BUF_SAMPLES_MIN     128
#define SDLA_BUF_SAMPLES_MAX     4096
#define SDLA_NUM_BUFS_MIN        2
#define SDLA_NUM_BUFS_MAX        8

static SDL_AudioStream *g_dev;
static int       g_buf_samples = XA2_BUF_SAMPLES_DEFAULT;
static int       g_num_bufs    = XA2_NUM_BUFS_DEFAULT;
static int       g_initialized;
static int       g_audio_opened;    /* this backend brought SDL audio up */
static int       g_frames_written;  /* periods handed to the queue */
static uint64_t  g_samples_queued;  /* samples handed to the queue, ever */
static unsigned  g_underruns;
static int       g_dry;
static int64_t   g_underrun_logged_ms;
/* The dummy driver: the virtual device replaces SDL's queue. Only the APU
 * frame thread submits and queries, so it needs no lock. */
static int        g_virtual;
static apu_vclock g_vclock;

static int env_int(recomp_env_id id, int def, int lo, int hi)
{
    int v = recomp_env_int(id, def);
    if (v < lo) v = lo;
    if (v > hi) v = hi;
    return v;
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int64_t now_ms(void)
{
    return now_ns() / 1000000;
}

/* Frames queued and not yet played, from SDL or the virtual device. */
static uint64_t queued_frames(void)
{
    if (!g_virtual)
        return (uint64_t)SDL_GetAudioStreamQueued(g_dev) / (SDLA_CHANNELS * sizeof(int16_t));
    return apu_vclock_queued(&g_vclock, now_ns());
}

int xa2_init(void)
{
    const SDL_AudioSpec want = { SDL_AUDIO_S16, SDLA_CHANNELS, SDLA_SAMPLE_RATE };
    SDL_AudioSpec have;
    const char *driver;
    char frames_hint[16];
    int frames = 0;

    if (g_initialized) return 1;
    /* Before SDL reads its environment: the old driver names still work. */
    xbox_sdl_env_compat();

    g_buf_samples = env_int(RENV_AUDIO_BUF_SAMPLES, XA2_BUF_SAMPLES_DEFAULT,
                            SDLA_BUF_SAMPLES_MIN, SDLA_BUF_SAMPLES_MAX);
    g_num_bufs = env_int(RENV_AUDIO_BUF_COUNT, XA2_NUM_BUFS_DEFAULT,
                         SDLA_NUM_BUFS_MIN, SDLA_NUM_BUFS_MAX);

    /* Headless: no speakers unless the caller names a driver. Must be set
     * before the audio subsystem starts; 0 keeps an existing value.
     * SDL_AUDIO_DRIVER is SDL's own variable, not one of ours. */
    if (recomp_env_on(RENV_HEADLESS))
        SDL_setenv_unsafe("SDL_AUDIO_DRIVER", "dummy", 0);

    /* Audio pulls in SDL's event subsystem, which would trap SIGINT and
     * SIGTERM into SDL_EVENT_QUIT; nothing pumps events in a headless run, so
     * Ctrl-C would be swallowed. Keep the process defaults. */
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_WasInit(SDL_INIT_AUDIO)) {
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            fprintf(stderr, "[SDL3] audio init failed: %s\n", SDL_GetError());
            return 0;
        }
        g_audio_opened = 1;
    }

    /* The device buffer is a hint in SDL3, read when the device opens. */
    snprintf(frames_hint, sizeof frames_hint, "%d", g_buf_samples);
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, frames_hint);

    /* The stream's input is the mixer's 48 kHz s16 stereo, so the queued
     * byte count stays in that format; SDL converts for the device (often
     * F32) on its side. The bound device starts paused. */
    g_dev = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &want, NULL, NULL);
    if (!g_dev) {
        fprintf(stderr, "[SDL3] SDL_OpenAudioDeviceStream failed: %s\n", SDL_GetError());
        if (g_audio_opened) { SDL_QuitSubSystem(SDL_INIT_AUDIO); g_audio_opened = 0; }
        return 0;
    }
    if (!SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(g_dev), &have, &frames))
    {
        have.freq = 0;
        frames = 0;
    }
    if (frames > 0 && frames != g_buf_samples) {
        fprintf(stderr, "[SDL3] device buffer is %d samples (asked %d)\n",
                frames, g_buf_samples);
        g_buf_samples = frames;
        /* The queue marks and the pacer's stall window assume buffers no
         * larger than the env limit. A driver that grants more is still
         * fed, against marks sized for the limit, and may underrun. */
        if (g_buf_samples > SDLA_BUF_SAMPLES_MAX)
            g_buf_samples = SDLA_BUF_SAMPLES_MAX;
    }

    g_frames_written = 0;
    g_samples_queued = 0;
    g_dry = 0;
    driver = SDL_GetCurrentAudioDriver();
    g_virtual = driver && !strcmp(driver, "dummy");
    /* The dummy device stays paused: the virtual one plays instead. A
     * device that will not resume never drains, and pacing by its queue
     * depth would stall the guest with no word why, so it goes silent on
     * the virtual clock instead. */
    if (!g_virtual && !SDL_ResumeAudioStreamDevice(g_dev)) {
        fprintf(stderr, "[SDL3] audio device resume failed: %s; silent on the virtual clock\n",
                SDL_GetError());
        g_virtual = 1;
    }
    if (g_virtual)
        apu_vclock_init(&g_vclock, SDLA_SAMPLE_RATE);
    g_initialized = 1;

    /* The device rate tells whether SDL resamples: its resampler holds
     * look-ahead frames that SDL_GetAudioStreamQueued still counts, so the
     * queue marks read high off 48 kHz. */
    fprintf(stderr, "[SDL3] audio initialized: driver %s (%d Hz stereo 16-bit, device %d Hz, %d x %d-sample buffers, %.1f ms queued max)%s\n",
            driver ? driver : "?", SDLA_SAMPLE_RATE, have.freq, g_num_bufs, g_buf_samples,
            1000.0 * g_num_bufs * g_buf_samples / SDLA_SAMPLE_RATE,
            g_virtual ? " [silent: no device output, virtual 48 kHz clock]" : "");
    return 1;
}

void xa2_shutdown(void)
{
    if (g_dev) {
        SDL_PauseAudioStreamDevice(g_dev);
        SDL_ClearAudioStream(g_dev);
        SDL_DestroyAudioStream(g_dev);
        g_dev = NULL;
    }
    if (g_audio_opened) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        g_audio_opened = 0;
    }
    if (g_initialized)
        fprintf(stderr, "[SDL3] shut down (%d periods written, %u underruns)\n",
                g_frames_written, g_underruns);
    g_initialized = 0;
}

int xa2_is_active(void)
{
    return g_initialized;
}

/* Hand one EP period to the queue. SDL copies the samples, so the caller's
 * buffer is free on return. Nothing is ever refused by depth: the stream is
 * unbounded and pacing by queue depth holds it at the marks. */
int xa2_submit_samples(const int16_t *samples, int num_samples)
{
    if (!g_initialized || !g_dev || num_samples <= 0) return 0;
    if (g_virtual)
        apu_vclock_submit(&g_vclock, (uint32_t)num_samples, now_ns());
    else if (!SDL_PutAudioStreamData(g_dev, samples,
                                     num_samples * SDLA_CHANNELS * (int)sizeof(int16_t))) {
        fprintf(stderr, "[SDL3] SDL_PutAudioStreamData failed: %s\n", SDL_GetError());
        return 0;
    }
    g_frames_written++;
    g_samples_queued += (uint64_t)num_samples;
    return 1;
}

int xa2_get_buffer_size(void)
{
    return g_buf_samples;
}

int xa2_get_buffer_count(void)
{
    return g_num_bufs;
}

/* Samples queued and not yet pulled by the device thread. The device holds
 * up to one more buffer in flight, which this does not count, the same way
 * XAudio2's BuffersQueued does not count the one playing. On the dummy
 * driver the virtual clock drains continuously instead, so nothing is in
 * flight and this is exactly what is left to play. Also the underrun
 * detector: an empty queue after audio has started means the device's last
 * pull was short and padded with silence; counted once per dry spell and
 * reported at most once a second. A drain announced by xa2_expect_drain
 * (the frame thread pausing) is not one. */
int xa2_queued_samples(void)
{
    uint64_t frames;

    if (!g_initialized || !g_dev) return 0;
    frames = queued_frames();
    if (frames == 0 && g_frames_written > 0) {
        if (!g_dry) {
            int64_t now = now_ms();
            g_dry = 1;
            g_underruns++;
            if (now - g_underrun_logged_ms >= 1000) {
                g_underrun_logged_ms = now;
                fprintf(stderr, "[SDL3] underrun x%u\n", g_underruns);
            }
        }
    } else {
        g_dry = 0;
    }
    return (int)frames;
}

void xa2_expect_drain(void)
{
    g_dry = 1;
}

unsigned xa2_underruns(void)
{
    return g_underruns;
}

/* What the device thread has pulled off the queue; it pulls one device
 * buffer per callback, so this leads the speaker by up to one buffer. On
 * the dummy driver it is the virtual clock's play position, which drains
 * continuously and so has no such lead. */
uint64_t xa2_samples_played(void)
{
    uint64_t queued;

    if (!g_initialized || !g_dev) return 0;
    queued = queued_frames();
    return g_samples_queued > queued ? g_samples_queued - queued : 0;
}

#endif /* !_WIN32 */
