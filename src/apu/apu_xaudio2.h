/**
 * XAudio2 Audio Output Backend
 */
#ifndef APU_XAUDIO2_H
#define APU_XAUDIO2_H

#include <stdint.h>

/* Initialize XAudio2. Returns 1 on success, 0 on failure. */
int xa2_init(void);

/* Shut down XAudio2 and release all resources. */
void xa2_shutdown(void);

/* Returns 1 if XAudio2 is active. */
int xa2_is_active(void);

/* Submit interleaved stereo 16-bit samples. Returns 1 if accepted. */
int xa2_submit_samples(const int16_t *samples, int num_samples);

/* Get the preferred buffer size in samples. */
int xa2_get_buffer_size(void);

/* Blocks submitted and not yet played (the device's own clock drains it). */
int xa2_queued(void);

/* Output health since start, for a title's vitals monitor. An underrun is a
 * submit that found nothing queued (playback ran dry: an audible gap); a drop
 * is a rendered block discarded because the queue was full (a skip). Both are
 * heard as crackle. Clipped counts samples at full scale (distortion). */
typedef struct {
    uint64_t submitted, dropped, underruns, clipped, samples;
    int peak;                   /* largest |sample| since the last call */
} Xa2Stats;
void xa2_get_stats(Xa2Stats *out);

/* Master volume, 0.0 .. 1.0 (default 0.5), applied to every sample before
 * output, followed by a limiter that keeps the signal below -6 dBFS: nothing
 * the guest or the mixers produce can reach full scale in a headset. */
void xa2_set_master_volume(float v);

/* The same volume + limiter, for any other output path (waveOut). in and out
 * may be the same buffer. */
void apu_output_safety(const int16_t *in, int16_t *out, int n);

#endif /* APU_XAUDIO2_H */
