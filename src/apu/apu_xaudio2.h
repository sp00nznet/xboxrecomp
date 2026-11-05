/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>
#include <string.h>

/* The device backend behind these functions: XAudio2 on Windows
 * (apu_xaudio2.c), SDL3 on POSIX hosts (apu_sdl.c). Same contract. */
#if defined(_WIN32)
#define XA2_BACKEND_NAME "XAudio2"
#else
#define XA2_BACKEND_NAME "SDL3"
#endif

/* Device buffer size and count, shared by both backends; overridable with
 * RECOMP_AUDIO_BUF_SAMPLES and RECOMP_AUDIO_BUF_COUNT. 512 x 4 is 10.7 ms a
 * buffer and 43 ms queued at the high mark. Under Wine the APU frame thread
 * can be preempted past that while other threads are busy (a scene load),
 * and the voice runs dry: the default there is deeper. */
#define XA2_BUF_SAMPLES_DEFAULT 512
#define XA2_NUM_BUFS_DEFAULT    4
#define XA2_NUM_BUFS_WINE       6

static inline int xa2_default_buf_count(int under_wine)
{
    return under_wine ? XA2_NUM_BUFS_WINE : XA2_NUM_BUFS_DEFAULT;
}

/* RECOMP_AUDIO_THREAD_PRIORITY for the APU frame thread. time_critical only
 * with tc_allowed (the audio_thread_tc debug key): the frame thread runs the
 * title's DirectSound ISR, and with no priority inheritance on the locks it
 * shares, that level can starve the game's own threads. *bad is set when
 * the value is not one of the names (or time_critical without the key). */
enum { XA2_PRIO_NORMAL, XA2_PRIO_HIGHEST, XA2_PRIO_TIME_CRITICAL };

static inline int xa2_parse_priority(const char *v, int tc_allowed, int *bad)
{
    *bad = 0;
    if (!v || !*v || !strcmp(v, "normal")) return XA2_PRIO_NORMAL;
    if (!strcmp(v, "highest")) return XA2_PRIO_HIGHEST;
    if (!strcmp(v, "time_critical")) {
        if (tc_allowed) return XA2_PRIO_TIME_CRITICAL;
        *bad = 1;
        return XA2_PRIO_HIGHEST;
    }
    *bad = 1;
    return XA2_PRIO_NORMAL;
}

static inline const char *xa2_priority_name(int prio)
{
    return prio == XA2_PRIO_TIME_CRITICAL ? "time_critical"
         : prio == XA2_PRIO_HIGHEST       ? "highest" : "normal";
}

/* Initialize the device backend. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Number of device buffers the voice may hold. */
int xa2_get_buffer_count(void);

/* Samples handed over and not yet played (queued buffers plus the partly
 * filled one). Also counts underruns: see xa2_underruns. */
int xa2_queued_samples(void);

/* The queue is about to drain on purpose (the frame thread is pausing):
 * the dry spell that follows is not an underrun. */
void xa2_expect_drain(void);

/* Pacing marks in samples for count buffers of buf_samples, fed one period
 * (period samples) at a time. A period is submitted only while queued <
 * high, so high is capped where that period could fill a buffer the voice
 * has no slot for: queued + period must stay below (count + 1) buffers
 * (count on the voice, one staging). Exact in samples, so buffer sizes that
 * are not multiples of the period work too. low is half of high. */
static inline void xa2_pacing_marks(int buf_samples, int count, int period,
                                    int *low, int *high)
{
    int h = buf_samples * count;
    int cap = buf_samples * (count + 1) - period;
    if (h > cap) h = cap;
    if (h < 1) h = 1;
    *high = h;
    *low = h / 2;
}

/* Times the voice ran dry after audio started ([XA2] underrun x%u). */
unsigned xa2_underruns(void);

/* Samples the device has consumed since init: XAudio2's SamplesPlayed, or
 * on SDL3 everything queued minus what is still queued. Read against the
 * wall clock this is the host-side starvation measure: while the device
 * plays, it advances 48000 a second; whatever it falls behind is silence
 * the device inserted (an underrun the WAV tap, which records submissions,
 * cannot see). */
uint64_t xa2_samples_played(void);

#endif /* APU_XAUDIO2_H */
