/* mcpx_apu_dsp_frame must mix the bins down the way the GP stages them: the
 * speaker bins at unity, the crosstalk bins at half, the I3DL2 send not at
 * all, the other effect sends at unity. See README.md for the measurements
 * behind each step.
 *
 * Run with: ctest --test-dir <build>   (three arms; see CMakeLists.txt) */
#include "apu_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

/* One signal per class of bin, each at a level no other sum can fake:
 *   bins 0/1  speaker pair (2D music)                    0.25
 *   bins 6/7  crosstalk pair (JSRF's 3D voices)          0.25
 *   bin 10    I3DL2 reverb send (the same voices again)  0.125
 *   bin 17    a title's own effect send (Burnout 3's)    0.0625
 * Bin 10 is even, so it can only ever reach the left channel; bin 17 is odd,
 * so it reaches only the right. */
#define SPEAKER 0.25f
#define XTLK    0.25f
#define SEND    0.125f
#define FXSEND  0.0625f

static int near(int got, float want)
{
    int w = (int)(want * 32767.0f);
    return got >= w - 2 && got <= w + 2;
}

int main(void)
{
    const char *mode_env = getenv("RECOMP_TEST_MIXDOWN_EXPECT");
    const int mode = mode_env ? atoi(mode_env) : 1;
    float want_l, want_r;
    const char *what;

    /* Zeroed is deliberately enough: ram_ptr NULL makes dsp_ack_frame return
     * before it touches guest memory, and monitor.point 0 is MON_AC97, which
     * is not MON_VP, so the mixdown runs. */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int i;

    if (!d) { fprintf(stderr, "out of memory\n"); return 2; }

    for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        mixbins[0][i] = mixbins[1][i] = SPEAKER;
        mixbins[6][i] = mixbins[7][i] = XTLK;
        mixbins[10][i] = SEND;
        mixbins[17][i] = FXSEND;
    }

    switch (mode) {
    case 0:
        /* The negative control for the two wider modes: the signal above
         * bin 1 must really be lost, or neither of them proves anything. */
        want_l = SPEAKER;
        want_r = SPEAKER;
        what = "two bins: only the speaker pair";
        break;
    case 2:
        /* Everything at unity, the mixdown that made JSRF's character sounds
         * 14-15 dB louder than its music. */
        want_l = SPEAKER + XTLK + SEND;
        want_r = SPEAKER + XTLK + FXSEND;
        what = "unity: every bin, the sends dry";
        break;
    default:
        want_l = SPEAKER + 0.5f * XTLK;
        want_r = SPEAKER + 0.5f * XTLK + FXSEND;
        what = "staged: crosstalk at half, I3DL2 dropped, bin 17 kept";
        break;
    }

    mcpx_apu_dsp_frame(d, mixbins);

    for (i = 0; i < NUM_SAMPLES_PER_FRAME; ++i) {
        int l = d->monitor.frame_buf[i][0], r = d->monitor.frame_buf[i][1];
        if (!near(l, want_l) || !near(r, want_r)) {
            fprintf(stderr,
                    "FAIL (mode %d, %s): sample %d is L=%d R=%d, want "
                    "L=%d R=%d\n", mode, what, i, l, r,
                    (int)(want_l * 32767.0f), (int)(want_r * 32767.0f));
            free(d);
            return 1;
        }
    }
    printf("PASS (mode %d): %s.\n", mode, what);
    free(d);
    return 0;
}
