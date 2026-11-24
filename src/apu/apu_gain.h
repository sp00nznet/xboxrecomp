/**
 * The master volume: one gain on the final stereo mix, just before the host
 * audio device and the audio_wav dump.
 *
 * Titles mix at very different loudness (JSRF about -18 LUFS-I, BLiNX 2
 * about -27), so a player wants one control that works in every game. It
 * sits after the DSP JIT, the stub and the software mixer have all written
 * the period, so every path through mcpx_apu_monitor_frame gets it once.
 *
 * The level is a percent, 0 to 100, and the gain is its square: 50% is
 * -12 dB, 25% is -24 dB, which spreads the slider over the range the ear
 * hears instead of crowding the audible change into the bottom fifth. At
 * 100% (the default) apu_master_gain_apply returns without touching the
 * samples, so stock output stays bit-identical.
 *
 * Set from any thread (the menu, the start); the APU frame thread reads it.
 * One aligned int, so a period sees either the old level or the new one.
 */
#ifndef APU_GAIN_H
#define APU_GAIN_H

#include <stdint.h>

#define APU_MASTER_VOLUME_MAX 100

/* Clamps pct to 0..APU_MASTER_VOLUME_MAX; returns the level kept. */
int  apu_master_volume_set(int pct);
int  apu_master_volume(void);
/* The gain in Q15 (32768 is unity) for a level. */
int32_t apu_master_gain_q15(int pct);
/* Scales n interleaved samples in place, saturating; nothing at 100%. */
void apu_master_gain_apply(int16_t *s, int n);

#endif
