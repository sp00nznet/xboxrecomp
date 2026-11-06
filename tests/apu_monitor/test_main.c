/* mcpx_apu_monitor_frame must hand the device what the DSP stage produced.
 *
 * It used to clear frame_buf and render only the test tone and the software
 * mixer into it, so every sample the VP and DSP made was thrown away before
 * the device saw it, and it rendered a 1024-sample device buffer for every
 * 256-sample EP period. Here the device is a fake that records what it is
 * given, and the frame thread's sequence (dsp_frame then monitor_frame, eight
 * times per period) is driven by hand. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define PERIOD 256
#define SUB    NUM_SAMPLES_PER_FRAME      /* 32 */

extern volatile int g_audio_muted;

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ----
 * It records what it is given and, for the pacing case, models a queue that
 * a consumption clock drains (dev_*). */
static int16_t got[PERIOD * 8][2];
static int got_samples, got_calls;

static int      dev_model;          /* pacing case: check sequence numbers  */
static uint64_t dev_tail, dev_head; /* samples submitted / consumed         */
static int      dev_seq_errors;

int  xa2_init(void)          { return 1; }
void xa2_shutdown(void)      {}
int  xa2_is_active(void)     { return 1; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void) { return 0; }
uint64_t xa2_samples_played(void) { return dev_head; }
int  xa2_queued_samples(void) { return (int)(dev_tail - dev_head); }
void xa2_expect_drain(void)  {}
int  xa2_submit_samples(const int16_t *s, int n)
{
    got_calls++;
    if (dev_model) {
        /* Sample k carries its sequence number: left = k & 0x7FFF,
         * right = k >> 15. Anything dropped or doubled breaks the run. */
        for (int i = 0; i < n; i++) {
            uint64_t k = dev_tail + (uint64_t)i;
            if ((uint16_t)s[2 * i] != (k & 0x7FFF)
                    || (uint16_t)s[2 * i + 1] != (uint16_t)(k >> 15))
                dev_seq_errors++;
        }
        dev_tail += (uint64_t)n;
        return 1;
    }
    if (got_samples + n <= PERIOD * 8)
        memcpy(got[got_samples], s, (size_t)n * 2 * sizeof(int16_t));
    got_samples += n;
    return 1;
}

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

/* What dsp_frame writes for bin value v: (int16_t)(v * 32767). Bin 0 is
 * left and bin 1 right in both mixdown modes. */
static float left_of(int k, int i)  { return (float)(k * SUB + i) / 4096.0f; }
static float right_of(int k, int i) { return -(float)(k * SUB + i) / 8192.0f; }

/* One EP period the way the frame thread runs it. */
static void run_period(MCPXAPUState *d)
{
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    int calls0 = got_calls;
    for (int k = 0; k < 8; k++) {
        memset(mixbins, 0, sizeof(mixbins));
        for (int i = 0; i < SUB; i++) {
            mixbins[0][i] = left_of(k, i);
            mixbins[1][i] = right_of(k, i);
        }
        d->ep_frame_div = k;
        mcpx_apu_dsp_frame(d, mixbins);
        mcpx_apu_monitor_frame(d);
        if (k < 7)
            CHECK("nothing submitted mid-period", got_calls == calls0);
    }
}

static int16_t want_l(int s) { return (int16_t)(left_of(s / SUB, s % SUB) * 32767.0f); }
static int16_t want_r(int s) { return (int16_t)(right_of(s / SUB, s % SUB) * 32767.0f); }

static int16_t clamp16(int v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

/* ---- the voice position trace ----
 * One looping 16-bit mono voice in fake guest RAM, run through the real VP
 * for 120 frames. stderr is captured to a file. With RECOMP_APU_TRACE on
 * (the apu_monitor ctest) two "[APU] voice 0 pos=" lines must appear, with
 * the position advancing between them; unset (apu_monitor_quiet) or =0
 * (apu_monitor_off), none. */
extern uint8_t *g_apu_ram_ptr;



/* What the ctest set, from its command line: on (the switches =1), unset,
 * or off (=0). The expectations come from here and never from the switch
 * reader, so reading =0 as on fails the off run instead of agreeing with
 * itself. It also names the capture files, one per ctest, so they can run
 * in parallel. */
static const char *g_mode = "unset";
static int g_expect_on;

static void wr32(uint32_t addr, uint32_t v) { memcpy(g_apu_ram_ptr + addr, &v, 4); }

static void voice_trace_case(void)
{
    enum { VPV = 0x100000, SGE = 0x200000, PCM = 0x300000, EBO = 8000 };
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    char line[256];
    unsigned pos[4], len[4], n = 0, v;
    char path[64];
    FILE *f;

    g_apu_ram_ptr = (uint8_t *)calloc(1, 64u << 20);
    if (!d || !g_apu_ram_ptr) { failures++; return; }

    d->regs[NV_PAPU_VPVADDR]   = VPV;
    d->regs[NV_PAPU_VPSGEADDR] = SGE;
    d->regs[NV_PAPU_TVL2D] = 0;              /* voice 0 heads the 2D list */
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    for (int page = 0; page < 8; page++)     /* SGE: page k -> PCM + k*4K */
        wr32(SGE + page * 8, PCM + page * 4096);
    for (int i = 0; i < EBO; i++) {          /* a ramp, so it is not silence */
        int16_t smp = (int16_t)(i * 4);
        memcpy(g_apu_ram_ptr + PCM + i * 2, &smp, 2);
    }
    wr32(VPV + NV_PAVS_VOICE_CFG_FMT,
         (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28)   /* S16          */
         | (1u << 30)                                    /* 2-byte cont. */
         | NV_PAVS_VOICE_CFG_FMT_LOOP);
    wr32(VPV + NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    wr32(VPV + NV_PAVS_VOICE_PAR_NEXT, EBO);
    wr32(VPV + NV_PAVS_VOICE_TAR_PITCH_LINK, 0xFFFF);    /* end of list  */

    snprintf(path, sizeof(path), "apu_monitor_trace_%s.txt",
             g_mode);
    fflush(stderr);
    if (!freopen(path, "w", stderr)) { failures++; return; }
    for (int fr = 0; fr < 120; fr++) {
        memset(mixbins, 0, sizeof(mixbins));
        mcpx_apu_vp_frame(d, mixbins);
    }
    fflush(stderr);

    f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f))
        if (n < 4 && sscanf(line, "[APU] voice %u pos=%u/%u", &v, &pos[n], &len[n]) == 3
                && v == 0)
            n++;
    if (f) fclose(f);
    printf("voice trace: %u lines", n);
    for (unsigned i = 0; i < n; i++) printf(" %u/%u", pos[i], len[i]);
    printf("\n");
    if (!g_expect_on) {
        CHECK("no trace lines with RECOMP_APU_TRACE unset or 0", n == 0);
    } else {
        CHECK("two trace lines in 120 frames", n == 2);
        CHECK("position advances", n == 2 && pos[1] > pos[0]);
        CHECK("length is the EBO", n >= 1 && len[0] == EBO);
    }
    free(g_apu_ram_ptr);
    g_apu_ram_ptr = NULL;
    free(d);
}

/* ---- the ring monitor ----
 * One looping 16-bit mono voice of 8 KB (eight 1 KB blocks), as a title
 * streaming music into a ring would set it up. For six laps a writer
 * refills the block two ahead of the cursor each time the cursor enters a
 * block: every block plays new data each lap, so nothing is stale. Then
 * the writer stops, and from the next lap each block replays its last lap.
 * With RECOMP_APU_RING set, phase one must log no stale block and phase two
 * at least a lap of them; unset or 0, no [APU-RING] line at all. */
static unsigned count_lines(const char *path, const char *prefix)
{
    char line[256];
    unsigned n = 0;
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, prefix, strlen(prefix)))
            n++;
    if (f) fclose(f);
    return n;
}

static void ring_case(void)
{
    enum { VPV = 0x100000, SGE = 0x200000, PCM = 0x300000,
           EBO = 4095, BLK_SAMPLES = 512, NBLK = 8, LAP_FRAMES = 128 };
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    int ring = g_expect_on;
    char pa[64], pb[64];
    unsigned cur = ~0u, fills = 0, stale_a, stale_b, any;

    g_apu_ram_ptr = (uint8_t *)calloc(1, 64u << 20);
    if (!d || !g_apu_ram_ptr) { failures++; return; }
    snprintf(pa, sizeof(pa), "apu_monitor_ring_%s_a.txt", g_mode);
    snprintf(pb, sizeof(pb), "apu_monitor_ring_%s_b.txt", g_mode);

    d->regs[NV_PAPU_VPVADDR]   = VPV;
    d->regs[NV_PAPU_VPSGEADDR] = SGE;
    d->regs[NV_PAPU_TVL2D] = 0;
    d->regs[NV_PAPU_TVL3D] = 0xFFFF;
    d->regs[NV_PAPU_TVLMP] = 0xFFFF;
    for (int page = 0; page < 2; page++)
        wr32(SGE + page * 8, PCM + page * 4096);
    for (int i = 0; i <= EBO; i++) {
        int16_t smp = (int16_t)(1000 + i);
        memcpy(g_apu_ram_ptr + PCM + i * 2, &smp, 2);
    }
    wr32(VPV + NV_PAVS_VOICE_CFG_FMT,
         (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) | (1u << 30)
         | NV_PAVS_VOICE_CFG_FMT_LOOP);
    wr32(VPV + NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
    wr32(VPV + NV_PAVS_VOICE_PAR_NEXT, EBO);
    wr32(VPV + NV_PAVS_VOICE_TAR_PITCH_LINK, 0xFFFF);

    for (int phase = 0; phase < 2; phase++) {
        fflush(stderr);
        if (!freopen(phase ? pb : pa, "w", stderr)) { failures++; return; }
        for (int fr = 0; fr < (phase ? 3 : 6) * LAP_FRAMES; fr++) {
            uint32_t cbo;
            memcpy(&cbo, g_apu_ram_ptr + VPV + NV_PAVS_VOICE_PAR_OFFSET, 4);
            cbo &= NV_PAVS_VOICE_PAR_OFFSET_CBO;
            if (!phase && cbo / BLK_SAMPLES != cur) {
                uint32_t b = (cbo / BLK_SAMPLES + 2) % NBLK;
                cur = cbo / BLK_SAMPLES;
                fills++;
                for (int i = 0; i < BLK_SAMPLES; i++) {
                    int16_t smp = (int16_t)(fills * 61 + i * 3 + 1);
                    memcpy(g_apu_ram_ptr + PCM + (b * BLK_SAMPLES + i) * 2, &smp, 2);
                }
            }
            memset(mixbins, 0, sizeof(mixbins));
            mcpx_apu_vp_frame(d, mixbins);
        }
        fflush(stderr);
    }

    stale_a = count_lines(pa, "[APU-RING] stale");
    stale_b = count_lines(pb, "[APU-RING] stale");
    any = count_lines(pa, "[APU-RING]") + count_lines(pb, "[APU-RING]");
    printf("ring: %u fills, %u stale while written, %u after the writer stopped\n",
           fills, stale_a, stale_b);
    if (!ring) {
        CHECK("no ring lines with RECOMP_APU_RING unset or 0", any == 0);
    } else {
        CHECK("writer kept up: no stale block", stale_a == 0);
        CHECK("writer stopped: a lap of stale blocks", stale_b >= NBLK);
    }
    free(g_apu_ram_ptr);
    g_apu_ram_ptr = NULL;
    free(d);
}

/* ---- host starvation accounting ----
 * apu_starve_step against a device that takes 512-sample buffers whole (as
 * XAudio2 and Wine do) on its own clock, which stops for each gap: the
 * producer is late, nothing is submitted and the device plays silence.
 * Submits come once an EP period. The same 100 ms of inserted silence as
 * one gap, or as twenty 5 ms gaps 100 ms or 1 s apart, must report 100 ms
 * to within two buffers: one of jitter band, and up to one more below the
 * threshold that has not been reported yet (before the fix the twenty
 * gaps reported nothing). A gap leaves the device dry (the backend's
 * underrun count rises) at the next submit. Ten minutes of device clock
 * drift at +-150 ppm reports nothing, nor does a device clock 3% fast or
 * slow that never runs dry (SDL's dummy driver runs about 3% slow); a 2 s
 * gap reports 2 s, unless it is a drain the frame thread announced (a
 * rebase, with the dry spell pre-marked), which reports nothing. */
typedef struct { int64_t start_us, len_us; } StarveGap;

static double starve_run(const StarveGap *gaps, int ngaps, int64_t run_us,
                         double ppm, int drain, unsigned *reports)
{
    enum { BUF = 512 };
    ApuStarve s;
    double total = 0;
    int rebase = 1, dry = 0;

    *reports = 0;
    for (int64_t k = 0;; k++) {
        int64_t now = k * 256 * 1000000 / 48000, stopped = 0;
        int in_gap = 0;
        uint64_t played;
        int64_t lost;

        if (now >= run_us)
            break;
        for (int i = 0; i < ngaps; i++) {
            int64_t into = now - gaps[i].start_us;
            if (into > 0)
                stopped += into < gaps[i].len_us ? into : gaps[i].len_us;
            if (into >= 0 && into < gaps[i].len_us)
                in_gap = 1;
        }
        /* The device pulls a buffer as it starts playing it. */
        played = ((uint64_t)((double)(now - stopped) * (1.0 + ppm * 1e-6)
                             * 48 / 1000) / BUF + 1) * BUF;
        if (in_gap) {
            rebase |= drain;
            dry = !drain;
            continue;
        }
        if (rebase) {
            apu_starve_rebase(&s, now, played);
            rebase = 0;
        }
        lost = apu_starve_step(&s, now, played, BUF, dry);
        dry = 0;
        if (lost) {
            (*reports)++;
            total += lost / 48.0;
        }
    }
    return total;
}

static void starve_case(void)
{
    StarveGap one = { 2000000, 100000 }, many[20], slow[20], drain = { 2000000, 2000000 };
    const double tol = 2 * 512 / 48.0 + 0.5;
    unsigned r_one, r_many, r_slow, r_fast, r_lag, r_fast3, r_lag3, r_stop, r_drain, r_slowdry;
    double t_one, t_many, t_slow, t_fast, t_lag, t_fast3, t_lag3, t_stop, t_drain, t_slowdry;

    for (int i = 0; i < 20; i++) {
        many[i].start_us = 2000000 + i * 100000; many[i].len_us = 5000;
        slow[i].start_us = 2000000 + i * 1000000; slow[i].len_us = 5000;
    }
    t_one  = starve_run(&one, 1, 5000000, 0, 0, &r_one);
    t_many = starve_run(many, 20, 6000000, 0, 0, &r_many);
    t_slow = starve_run(slow, 20, 24000000, 0, 0, &r_slow);
    t_fast = starve_run(NULL, 0, 600000000, 150, 0, &r_fast);
    t_lag  = starve_run(NULL, 0, 600000000, -150, 0, &r_lag);
    t_fast3 = starve_run(NULL, 0, 60000000, 30000, 0, &r_fast3);
    t_lag3 = starve_run(NULL, 0, 60000000, -30000, 0, &r_lag3);
    t_stop = starve_run(&drain, 1, 6000000, 0, 0, &r_stop);
    t_drain = starve_run(&drain, 1, 6000000, 0, 1, &r_drain);
    /* The known limit, printed rather than checked: on a clock 3% slow each
     * dry spell adds up to about a buffer of drift to its report (true
     * total 100 ms). Real devices sit well inside the 200 ppm allowance. */
    t_slowdry = starve_run(slow, 20, 24000000, -30000, 0, &r_slowdry);
    printf("starve limit: 20 x 5 ms 1 s apart on a 3%% slow clock %.1f ms (%u)\n",
           t_slowdry, r_slowdry);
    printf("starve: one gap %.1f ms (%u), 20 x 5 ms %.1f ms (%u), 1 s apart %.1f ms (%u),"
           " drift +150 ppm %.1f ms (%u), -150 ppm %.1f ms (%u), +3%% %.1f ms (%u),"
           " -3%% %.1f ms (%u), 2 s gap %.1f ms (%u), drained %.1f ms (%u)\n",
           t_one, r_one, t_many, r_many, t_slow, r_slow, t_fast, r_fast,
           t_lag, r_lag, t_fast3, r_fast3, t_lag3, r_lag3, t_stop, r_stop,
           t_drain, r_drain);
    CHECK("one 100 ms gap reports 100 ms", fabs(t_one - 100) <= tol);
    CHECK("twenty 5 ms gaps report 100 ms", fabs(t_many - 100) <= tol);
    CHECK("twenty 5 ms gaps 1 s apart report 100 ms", fabs(t_slow - 100) <= tol);
    CHECK("small gaps add up over several reports", r_many >= 2);
    CHECK("no report from a fast device clock", r_fast == 0);
    CHECK("no report from a slow device clock", r_lag == 0);
    CHECK("no report from a 3% fast clock that never runs dry", r_fast3 == 0);
    CHECK("no report from a 3% slow clock that never runs dry", r_lag3 == 0);
    CHECK("a 2 s gap reports 2 s", fabs(t_stop - 2000) <= tol);
    CHECK("an announced drain reports nothing", r_drain == 0);
}

/* ---- pacing by queue depth ----
 * The frame thread's decision (mcpx_apu_pace_step) against the fake queue,
 * on a simulated clock: the device consumes 48 samples per millisecond
 * except during a stall. Rendering takes no simulated time, which is the
 * worst case for running ahead. */
static uint64_t sim_consumed_at(int64_t now_us, int64_t stall_from,
                                int64_t stall_us)
{
    int64_t t = now_us;
    if (t > stall_from)
        t = (t < stall_from + stall_us) ? stall_from : t - stall_us;
    return (uint64_t)(t * 48 / 1000);
}

static void pacing_case(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    const int low = 4, high = 8;             /* granules: 512 x 4 marks */
    const int64_t stall_from = 3000000, stall_us = 100000;
    int64_t now = 0;
    uint64_t seq = 0, underruns = 0;
    int periods = 0, minq = 1 << 30, maxq = 0, max_q_stall = 0;
    int submits_in_stall = 0, waits = 0;

    if (!d) { failures++; return; }
    d->monitor.queued_bytes_low  = low  * PERIOD * 4;
    d->monitor.queued_bytes_high = high * PERIOD * 4;
    dev_model = 1; dev_tail = dev_head = 0; dev_seq_errors = 0;

    while (periods < 1000) {
        uint64_t c = sim_consumed_at(now, stall_from, stall_us);
        int q;
        if (c > dev_tail) {                  /* ran dry: plays silence */
            underruns++;
            dev_head = dev_tail;
        } else {
            dev_head = c > dev_head ? c : dev_head;
        }
        q = xa2_queued_samples() / PERIOD;
        if (now > 200000 && !(now >= stall_from && now < stall_from + stall_us + 200000)) {
            if (q < minq) minq = q;
            if (q > maxq) maxq = q;
        }
        if (now >= stall_from && now < stall_from + stall_us && q > max_q_stall)
            max_q_stall = q;

        d->ep_frame_div = 0;
        int64_t w = mcpx_apu_pace_step(d, now);
        if (w > 0) { now += w; waits++; continue; }

        for (int i = 0; i < PERIOD; i++) {
            uint64_t k = seq + (uint64_t)i;
            d->monitor.frame_buf[i][0] = (int16_t)(k & 0x7FFF);
            d->monitor.frame_buf[i][1] = (int16_t)(uint16_t)(k >> 15);
        }
        d->ep_frame_div = 7;
        mcpx_apu_monitor_frame(d);
        mcpx_apu_pace_rendered(d);
        seq += PERIOD;
        periods++;
        if (now >= stall_from && now < stall_from + stall_us)
            submits_in_stall++;
        /* Rendering starts only below high, so one period takes the queue
         * at most PERIOD - 1 samples past it (this fake drains sample by
         * sample; a real voice reports whole buffers). */
        if (xa2_queued_samples() > high * PERIOD + PERIOD - 1) {
            printf("FAIL: queue %d samples > high %d + one period after a submit\n",
                   xa2_queued_samples(), high * PERIOD);
            failures++;
            break;
        }
    }
    printf("pacing: 1000 periods in %.3f s simulated, queue %d..%d steady, "
           "%d at most in the stall, %llu underruns, %d submits during the stall\n",
           now / 1e6, minq, maxq, max_q_stall, (unsigned long long)underruns,
           submits_in_stall);
    CHECK("steady queue stays within [low, high]", minq >= low && maxq <= high);
    CHECK("stall stops submission at high", max_q_stall <= high);
    CHECK("no underrun after start", underruns <= 1);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0
          && dev_tail == 1000ull * PERIOD);
    CHECK("runs at the device rate (1000 periods ~ 5.33 s)",
          now > 5200000 + stall_us - 100000 && now < 5500000 + stall_us);
    dev_model = 0;
    free(d);
}

/* ---- the frame loop around the pacer ----
 * One frame-thread pass at a period boundary, as mcpx_apu_frame_thread runs
 * it: throttle (pace_step until it says go), then, unless the front end is
 * trapped, render and submit a period and charge it (pace_rendered). A
 * trapped pass renders nothing, charges nothing and waits a millisecond. Sample k of the stream carries k (as the fake checks)
 * counted from what the device was given, so dropped periods are not gaps. */
struct loop_stats {
    int periods, submits, underruns, minq, maxq;
    int trapped_passes, trapped_charges;
};

static void consume(int64_t now, int64_t stall_from, int64_t stall_us,
                    struct loop_stats *st)
{
    uint64_t c = sim_consumed_at(now, stall_from, stall_us);
    if (c > dev_tail) {                      /* ran dry: plays silence */
        st->underruns++;
        dev_head = dev_tail;
    } else {
        dev_head = c > dev_head ? c : dev_head;
    }
}

static int64_t loop_pass(MCPXAPUState *d, int64_t now, int64_t stall_from,
                         int64_t stall_us, struct loop_stats *st)
{
    bool trapped = (d->regs[NV_PAPU_FECTL] & NV_PAPU_FECTL_FEMETHMODE_TRAPPED) != 0;
    int64_t sched = d->next_frame_time_us;

    d->ep_frame_div = 0;
    for (;;) {                               /* throttle */
        consume(now, stall_from, stall_us, st);
        int64_t w = mcpx_apu_pace_step(d, now);
        if (w <= 0) break;
        now += w;
    }
    if (trapped) {
        st->trapped_passes++;
        if (d->next_frame_time_us != sched)
            st->trapped_charges++;
        return now + 1000;
    }
    int tail0 = (int)dev_tail;
    for (int i = 0; i < PERIOD; i++) {
        uint64_t k = dev_tail + (uint64_t)i;
        d->monitor.frame_buf[i][0] = (int16_t)(k & 0x7FFF);
        d->monitor.frame_buf[i][1] = (int16_t)(uint16_t)(k >> 15);
    }
    d->ep_frame_div = 7;
    mcpx_apu_monitor_frame(d);
    mcpx_apu_pace_rendered(d);
    st->periods++;
    if ((int)dev_tail != tail0)
        st->submits++;
    return now;
}

static MCPXAPUState *loop_device(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    if (!d) return NULL;
    d->monitor.queued_bytes_low  = 4 * PERIOD * 4;   /* granules, as above */
    d->monitor.queued_bytes_high = 8 * PERIOD * 4;
    /* XCNTMODE on, front end free running. */
    d->regs[NV_PAPU_SECTL] = 1u << 3;
    dev_model = 1; dev_tail = dev_head = 0; dev_seq_errors = 0;
    return d;
}

/* A trap that starts on a period boundary must not spend pacing credit:
 * the trapped loop passes once a millisecond, and each pass used to advance
 * the schedule a whole period, so the lead the queue had built drained
 * after the trap. Here a 10 ms trap begins at a boundary every 60 ms. */
static void trapped_case(void)
{
    MCPXAPUState *d = loop_device();
    struct loop_stats st = { 0, 0, 0, 1 << 30, 0, 0, 0 };
    int64_t now = 0, trap_end = 0, next_trap = 500000;

    if (!d) { failures++; return; }
    while (now < 4000000) {
        if (!trap_end && now >= next_trap) {
            d->regs[NV_PAPU_FECTL] |= NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            trap_end = now + 10000;
        } else if (trap_end && now >= trap_end) {
            d->regs[NV_PAPU_FECTL] &= ~NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
            trap_end = 0;
            next_trap = now + 60000;
        }
        now = loop_pass(d, now, 1LL << 60, 0, &st);
        if (now > 300000) {
            int q = xa2_queued_samples() / PERIOD;
            if (q < st.minq) st.minq = q;
            if (q > st.maxq) st.maxq = q;
        }
    }
    printf("trapped: %d periods, %d trapped passes, %d charged, queue %d..%d,"
           " %d underruns\n", st.periods, st.trapped_passes,
           st.trapped_charges, st.minq, st.maxq, st.underruns);
    CHECK("traps happened", st.trapped_passes > 100);
    CHECK("a trapped pass spends no pacing credit", st.trapped_charges == 0);
    CHECK("queue never drains across a trap", st.underruns <= 1 && st.minq >= 1);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0);
    dev_model = 0;
    free(d);
}

/* A device that stops consuming for good must not stop the APU clock: after
 * the queue has sat at high for 200 ms the host clock paces, periods are
 * dropped rather than queued, and when the device drains again the queue
 * paces once more. Here the device stops at 2 s and resumes at 3 s. */
static void stall_fallback_case(void)
{
    MCPXAPUState *d = loop_device();
    struct loop_stats st = { 0, 0, 0, 1 << 30, 0, 0, 0 };
    const int64_t stall_from = 2000000, stall_us = 1000000;
    int64_t now = 0;
    int in_stall_periods = 0, in_stall_submits = 0, after_underruns = 0;
    int periods0, submits0, underruns0, max_q = 0;

    if (!d) { failures++; return; }
    while (now < 5000000) {
        periods0 = st.periods; submits0 = st.submits; underruns0 = st.underruns;
        int64_t t = now;
        now = loop_pass(d, now, stall_from, stall_us, &st);
        if (t >= stall_from + 250000 && now < stall_from + stall_us) {
            in_stall_periods += st.periods - periods0;
            in_stall_submits += st.submits - submits0;
        }
        if (t >= stall_from + stall_us)
            after_underruns += st.underruns - underruns0;
        if (t >= stall_from + stall_us + 300000) {
            int q = xa2_queued_samples() / PERIOD;
            if (q < st.minq) st.minq = q;
            if (q > st.maxq) st.maxq = q;
        }
        if (xa2_queued_samples() / PERIOD > max_q)
            max_q = xa2_queued_samples() / PERIOD;
    }
    printf("stall fallback: %d periods rendered and %d submitted in the last"
           " 750 ms of a 1 s stall; after it queue %d..%d, %d underruns;"
           " queue %d at most\n", in_stall_periods, in_stall_submits,
           st.minq, st.maxq, after_underruns, max_q);
    CHECK("stalled device: the clock keeps running (~140 periods)",
          in_stall_periods > 120 && in_stall_periods < 160);
    CHECK("stalled device: periods are dropped, not queued", in_stall_submits == 0);
    CHECK("queue never grows past high + one period", max_q <= 9);
    CHECK("device back: the queue paces again", st.minq >= 4 && st.maxq <= 8);
    CHECK("device back: no underrun", after_underruns == 0);
    CHECK("no sample dropped or duplicated", dev_seq_errors == 0);
    dev_model = 0;
    free(d);
}

/* ---- the frame decision ----
 * DirectSound's SSL service routine stops the sequencer and starts it again
 * (SECTL 0x07, then 0x0F a few hundred microseconds later). A frame that
 * reads SECTL inside that gap must not be played as a silent slice: it is
 * held, like a trapped front end. A sequencer that stays off past the hold,
 * or that never rendered, gets lightweight frames as before. */
#define SECTL_ON  0x0Fu
#define SECTL_OFF 0x07u

static void frame_kind_case(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    int light = 0, held = 0, render = 0;
    int64_t t;

    if (!d) { failures++; return; }
    d->regs[NV_PAPU_SECTL] = SECTL_OFF;
    CHECK("off before the VP ever rendered: lightweight",
          mcpx_apu_frame_kind(d, 0) == APU_FRAME_LIGHT);
    d->regs[NV_PAPU_SECTL] = SECTL_ON;
    CHECK("sequencer on: render", mcpx_apu_frame_kind(d, 100) == APU_FRAME_RENDER);

    d->regs[NV_PAPU_SECTL] = SECTL_OFF;
    CHECK("off after rendering: held", mcpx_apu_frame_kind(d, 1000) == APU_FRAME_HELD);
    CHECK("still off 300 us later: held", mcpx_apu_frame_kind(d, 1300) == APU_FRAME_HELD);
    d->regs[NV_PAPU_SECTL] = SECTL_ON;
    CHECK("on again: render", mcpx_apu_frame_kind(d, 1400) == APU_FRAME_RENDER);

    d->regs[NV_PAPU_SECTL] = SECTL_OFF;
    CHECK("a new stop is held from its own start",
          mcpx_apu_frame_kind(d, 10000) == APU_FRAME_HELD &&
          mcpx_apu_frame_kind(d, 14999) == APU_FRAME_HELD);
    CHECK("off past the hold: lightweight",
          mcpx_apu_frame_kind(d, 15000) == APU_FRAME_LIGHT);
    CHECK("and stays lightweight, not held again",
          mcpx_apu_frame_kind(d, 15100) == APU_FRAME_LIGHT &&
          mcpx_apu_frame_kind(d, 90000) == APU_FRAME_LIGHT);

    d->regs[NV_PAPU_SECTL] = SECTL_ON;
    d->regs[NV_PAPU_FECTL] = NV_PAPU_FECTL_FEMETHMODE_TRAPPED;
    CHECK("front end trapped: trapped", mcpx_apu_frame_kind(d, 100000) == APU_FRAME_TRAPPED);
    d->regs[NV_PAPU_FECTL] = 0;
    CHECK("front end free: render", mcpx_apu_frame_kind(d, 100100) == APU_FRAME_RENDER);

    /* Ten seconds of the opening scene's pattern: a frame every 0.67 ms, the
     * sequencer off for 300 us every 43 ms, and a held frame asking again
     * every 100 us. No frame is played as a lightweight (silent) slice. */
    for (t = 200000; t < 10200000; ) {
        int64_t phase = (t - 200000) % 43000;
        d->regs[NV_PAPU_SECTL] = phase >= 21000 && phase < 21300 ? SECTL_OFF : SECTL_ON;
        int k = mcpx_apu_frame_kind(d, t);
        if (k == APU_FRAME_RENDER) { render++; t += 667; }
        else if (k == APU_FRAME_HELD) { held++; t += 100; }
        else { light++; t += 667; }
    }
    printf("frame kind: 10 s with SSL stops: %d rendered, %d held passes,"
           " %d lightweight\n", render, held, light);
    CHECK("SSL stops: the slice waits instead of playing silent", light == 0);
    CHECK("SSL stops: frames were held", held > 0);
    CHECK("SSL stops: the frames all render", render > 14900);
    free(d);
}

/* ---- the switches ----
 * The reader against what the ctest set: on with =1, off unset or =0. */
static void switch_case(void)
{
    CHECK("RECOMP_APU_TRACE reads as the ctest set it",
          apu_env_on("RECOMP_APU_TRACE") == g_expect_on);
    CHECK("RECOMP_AUDIO_HOST reads as the ctest set it",
          apu_env_on("RECOMP_AUDIO_HOST") == g_expect_on);
    CHECK("RECOMP_APU_RING reads as the ctest set it",
          apu_env_on("RECOMP_APU_RING") == g_expect_on);
}

int main(int argc, char **argv)
{
    /* Zeroed: ram_ptr NULL keeps dsp_ack_frame out of guest memory, and
     * monitor.point 0 is MON_AC97, so dsp_frame writes frame_buf. */
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    int ok, i;
    if (!d) return 2;
    if (argc != 2 || (strcmp(argv[1], "on") && strcmp(argv[1], "off")
                      && strcmp(argv[1], "unset"))) {
        fprintf(stderr, "usage: apu_monitor_test on|off|unset\n");
        return 2;
    }
    g_mode = argv[1];
    g_expect_on = !strcmp(g_mode, "on");
    printf("apu_monitor: running\n");

    /* 1. The DSP's period reaches the device unchanged, once, 256 samples. */
    got_calls = got_samples = 0;
    run_period(d);
    CHECK("one submit per period", got_calls == 1);
    CHECK("exactly 256 samples", got_samples == PERIOD);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (got[i][0] != want_l(i) || got[i][1] != want_r(i)) ok = 0;
    CHECK("DSP frame reaches the device unchanged", ok);
    CHECK("signal is not silence", got[PERIOD - 1][0] != 0 && got[PERIOD - 1][1] != 0);
    for (ok = 1, i = 0; i < PERIOD; i++)
        if (d->monitor.frame_buf[i][0] || d->monitor.frame_buf[i][1]) ok = 0;
    CHECK("frame_buf starts the next period silent", ok);

    /* 2. With a software-mixer voice playing, the device gets the sum. */
    {
        static int16_t pcm[64][2];
        int slot = apu_mixer_alloc_voice();
        APUMixerVoice *v = apu_mixer_get_voice(slot);
        CHECK("mixer voice allocated", slot >= 0 && v);
        for (i = 0; i < 64; i++) { pcm[i][0] = 1000; pcm[i][1] = -2000; }
        v->pcm_data = &pcm[0][0];
        v->pcm_bytes = sizeof(pcm);
        v->num_channels = 2;
        v->sample_rate = 48000;
        v->volume = 1.0f;
        apu_mixer_play(slot, 1);

        got_calls = got_samples = 0;
        run_period(d);
        CHECK("one submit with the mixer on", got_calls == 1 && got_samples == PERIOD);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] != clamp16(want_l(i) + 1000)
                    || got[i][1] != clamp16(want_r(i) - 2000)) ok = 0;
        CHECK("mixer is added to the DSP frame", ok);

        /* 3. Muted: silence, still one period. */
        g_audio_muted = 1;
        got_calls = got_samples = 0;
        run_period(d);
        for (ok = 1, i = 0; i < PERIOD; i++)
            if (got[i][0] || got[i][1]) ok = 0;
        CHECK("muted period is silent", ok && got_samples == PERIOD);
        g_audio_muted = 0;
        apu_mixer_free_voice(slot);
    }

    /* 4. Ten periods: 2560 samples, no more, no fewer. */
    got_calls = got_samples = 0;
    for (i = 0; i < 10; i++) run_period(d);
    CHECK("ten periods, ten submits", got_calls == 10);
    CHECK("ten periods, 2560 samples", got_samples == 10 * PERIOD);

    free(d);
    pacing_case();
    trapped_case();
    stall_fallback_case();
    starve_case();
    frame_kind_case();
    switch_case();
    voice_trace_case();                      /* last: they redirect stderr */
    ring_case();
    if (failures == 0) { printf("apu_monitor: ALL PASS\n"); return 0; }
    printf("apu_monitor: %d FAILURE(S)\n", failures);
    return 1;
}
