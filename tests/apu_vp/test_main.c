/* The voice processor against a small fake guest RAM.
 *
 * apu_vp.c is compiled into this file so its static helpers can be called
 * directly. Each check below fails on the code it was written for, except
 * resample_half_rate_lands_on_source_samples, which pins that positions off
 * rate 1 did not move:
 *   - adpcm_*: the decode buffers held two 36-byte blocks' worth, so a
 *     wider samples-per-block value overran them on the stack (a mono
 *     voice already at two).
 *   - gain_*: a volume change stepped at the frame boundary instead of
 *     ramping across the frame.
 *   - outbuf_sge_*: output buffer SGE entries were written into the voices'
 *     table (VPSGEADDR), so an output buffer at handle 1 overwrote the first
 *     page of a voice buffer at inbuf handle 1.
 *   - resample_*: the resampler held one fetched sample back, so a voice at
 *     rate 1 looping a 32-sample buffer rewritten each frame (Burnout 3's
 *     effect returns from the GP) began each frame with the last frame's
 *     sample 0; and at rate 0.3 its source count (floorf of the summed
 *     steps) came out one short every few frames, dropping a sample.
 *   - *_fetch_ending_before_ebo_*: a voice wrapped, ended or moved to its
 *     next stream segment when a fetch stopped at cbo == ebo, before reading
 *     sample ebo, so that sample was never played (and a segment was
 *     signalled done a fetch early).
 *   - one_shot_final_frame_reaches_mix_bins: voice_process returned without
 *     mixing when the voice was off after its fetch loop, so the frame in
 *     which a one-shot ended (its last samples, sample ebo among them) was
 *     fetched and thrown away. The *_final_frame_* checks pin it for a
 *     one-shot at rate 1, a one-shot at rate 0.5 (the resampler returns a
 *     short count, then the voice is off) and a non-persist stream that runs
 *     out of packets.
 *   - sample_reads_take_the_data_resolver: written with the data/phys split, so
 *     it pins it: PCM and ADPCM samples come from mcpx_apu_phys_data, the
 *     voice and SGE tables from mcpx_apu_phys.
 *
 * Run with: ctest --test-dir <build> */
#include "../../src/apu/apu_vp.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Symbols the VP takes from the rest of the APU. */
MCPXAPUState *g_state;
struct McpxApuDebug g_dbg, g_dbg_cache;
int g_dbg_voice_monitor = -1;
uint64_t g_dbg_muted_voices[4];
uint8_t *g_apu_ram_ptr;

#define RAM_SIZE   0x100000u
#define VOICE_BASE 0x10000u   /* NV_PAPU_VPVADDR */
#define SGE_BASE   0x20000u   /* NV_PAPU_VPSGEADDR */
#define DATA_BASE  0x30000u   /* what SGE entry 0 names */
#define GPF_BASE   0x40000u   /* NV_PAPU_GPFADDR */

static uint8_t *s_ram;
static int s_failures;

uint8_t *mcpx_apu_phys(uint64_t addr)
{
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: guest access at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_ram + addr;
}

const char *mcpx_apu_phys_class(uint64_t addr) { (void)addr; return "fake"; }
/* Sample data resolves as everything else does unless a check sets s_data:
 * then it reads that second fake RAM, so a sample read that went back to
 * mcpx_apu_phys, or a voice or SGE read moved to the data resolver, shows. */
static uint8_t *s_data;
uint8_t *mcpx_apu_phys_data(uint64_t addr)
{
    if (!s_data) return mcpx_apu_phys(addr);
    if (addr >= RAM_SIZE) {
        fprintf(stderr, "FAIL: sample read at 0x%llX is outside the fake RAM\n",
                (unsigned long long)addr);
        exit(1);
    }
    return s_data + addr;
}
const char *mcpx_apu_phys_data_class(uint64_t addr) { (void)addr; return "fake"; }
int apu_env_on(const char *name) { (void)name; return 0; }
void apu_lockstat_hold_end(int who) { (void)who; }
void apu_lockstat_hold_begin(int who) { (void)who; }
void apu_lockstat_lock(MCPXAPUState *d, int who) { (void)who; qemu_mutex_lock(&d->lock); }
void apu_lockstat_unlock(MCPXAPUState *d, int who) { (void)who; qemu_mutex_unlock(&d->lock); }
ApuIrqStats apu_irq_stats;

#define CHECK(cond, ...) do { if (!(cond)) { \
    fprintf(stderr, "FAIL line %d: ", __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); s_failures++; } } while (0)

static void set_voice_reg(uint16_t v, uint32_t reg, uint32_t value)
{
    memcpy(s_ram + VOICE_BASE + v * NV_PAVS_SIZE + reg, &value, 4);
}

/* Voice 0: an ADPCM mono buffer voice whose block holds `blocks` * 36 bytes,
 * the data read through SGE entries 0 and 1. */
static void setup_adpcm_voice(MCPXAPUState *d, unsigned blocks)
{
    uint32_t fmt = (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_ADPCM << 30) |
                   (((blocks - 1) & 0x1F) << 16);
    uint32_t sge[4] = { DATA_BASE, 0, DATA_BASE + 0x1000u, 0 };

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d->regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    memcpy(s_ram + SGE_BASE, sge, sizeof(sge));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT, fmt);
    set_voice_reg(0, NV_PAVS_VOICE_PAR_NEXT, 0x1000);         /* EBO */

    /* A block header (predictor 0, step index 20) and varied nibbles. */
    s_ram[DATA_BASE + 2] = 20;
    for (unsigned i = 4; i < 36u * blocks; i++)
        s_ram[DATA_BASE + i] = (uint8_t)(i * 37u + 11u);
}

static void adpcm_wide_block_decodes(void)
{
    static MCPXAPUState d;
    float samples[NUM_SAMPLES_PER_FRAME][2];
    int16_t expect[65 * ADPCM_MAX_BLOCKS * 2];

    for (unsigned blocks = 1; blocks <= ADPCM_MAX_BLOCKS; blocks++) {
        setup_adpcm_voice(&d, blocks);
        adpcm_decode_block(expect, s_ram + DATA_BASE, 36u * blocks, 1);
        memset(samples, 0, sizeof(samples));
        int n = voice_get_samples(&d, 0, samples, NUM_SAMPLES_PER_FRAME);
        CHECK(n == NUM_SAMPLES_PER_FRAME, "%u blocks: %d samples", blocks, n);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            if (samples[i][0] != int16_to_float(expect[i])) {
                CHECK(0, "%u blocks: sample %d is %f, decoder says %f",
                      blocks, i, samples[i][0], int16_to_float(expect[i]));
                break;
            }
    }
}

static void adpcm_whole_field_fits(void)
{
    /* samples-per-block is 5 bits plus one: the buffers hold every value
     * the descriptor can name, so adpcm_wide_block_decodes covers them all
     * and no voice is ever refused. */
    CHECK(ADPCM_MAX_BLOCKS >= 0x1F + 1, "ADPCM_MAX_BLOCKS %d < 32", ADPCM_MAX_BLOCKS);
}

static uint32_t ram32(uint32_t addr)
{
    uint32_t v;
    memcpy(&v, s_ram + addr, 4);
    return v;
}

/* The order JSRF uses: a voice buffer mapped at inbuf handles 1-2, then its
 * output buffer remapped at outbuf handles 0-1. */
static void outbuf_sge_goes_to_gp_fifo_table(void)
{
    static MCPXAPUState d;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    d.regs[NV_PAPU_GPFADDR] = GPF_BASE;
    d.regs[NV_PAPU_GPFMAXSGE] = 1;

    fe_method(&d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE, 1);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET, 0x014B0000u);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE, 2);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_INBUF_SGE_OFFSET, 0x014B1000u);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE, 0);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET, 0x00379000u);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE, 1);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET, 0x0037A000u);

    CHECK(ram32(SGE_BASE + 0) == 0, "voice SGE 0 is %08X, expected untouched",
          ram32(SGE_BASE + 0));
    CHECK(ram32(SGE_BASE + 8) == 0x014B0000u,
          "voice SGE 1 is %08X, expected 014B0000", ram32(SGE_BASE + 8));
    CHECK(ram32(SGE_BASE + 16) == 0x014B1000u,
          "voice SGE 2 is %08X, expected 014B1000", ram32(SGE_BASE + 16));
    CHECK(ram32(GPF_BASE + 0) == 0x00379000u,
          "GP FIFO SGE 0 is %08X, expected 00379000", ram32(GPF_BASE + 0));
    CHECK(ram32(GPF_BASE + 8) == 0x0037A000u,
          "GP FIFO SGE 1 is %08X, expected 0037A000", ram32(GPF_BASE + 8));
}

/* With no GP FIFO table set up, the entry has nowhere to go: it must not
 * land at physical 0 or in the voices' table. */
static void outbuf_sge_without_table_is_dropped(void)
{
    static MCPXAPUState d;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPSGEADDR] = SGE_BASE;

    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE, 1);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET, 0x0037A000u);

    CHECK(ram32(8) == 0, "physical 8 is %08X, expected untouched", ram32(8));
    CHECK(ram32(SGE_BASE + 8) == 0, "voice SGE 1 is %08X, expected untouched",
          ram32(SGE_BASE + 8));
}

/* GPFMAXSGE bounds the table (inclusive): a handle past it would write past
 * the table into whatever follows it. */
static void outbuf_sge_past_max_is_dropped(void)
{
    static MCPXAPUState d;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    d.regs[NV_PAPU_GPFADDR] = GPF_BASE;
    d.regs[NV_PAPU_GPFMAXSGE] = 1;

    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE, 2);
    fe_method(&d, NV1BA0_PIO_SET_CURRENT_OUTBUF_SGE_OFFSET, 0x0037B000u);

    CHECK(ram32(GPF_BASE + 16) == 0,
          "GP FIFO SGE 2 is %08X, expected untouched (GPFMAXSGE 1)",
          ram32(GPF_BASE + 16));
    CHECK(ram32(SGE_BASE + 16) == 0, "voice SGE 2 is %08X, expected untouched",
          ram32(SGE_BASE + 16));
}

/* Voice 0: a looping mono s16 PCM buffer voice of `len` samples at
 * DATA_BASE. */
static void setup_pcm_loop_voice(MCPXAPUState *d, unsigned len)
{
    uint32_t fmt = (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                   (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
                   NV_PAVS_VOICE_CFG_FMT_LOOP;
    uint32_t sge[4] = { DATA_BASE, 0, DATA_BASE + 0x1000u, 0 };

    memset(s_ram, 0, RAM_SIZE);
    memset(d, 0, sizeof(*d));
    d->regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    d->regs[NV_PAPU_VPSGEADDR] = SGE_BASE;
    memcpy(s_ram + SGE_BASE, sge, sizeof(sge));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT, fmt);
    set_voice_reg(0, NV_PAVS_VOICE_PAR_NEXT, len - 1);        /* EBO */
    set_voice_reg(0, NV_PAVS_VOICE_PAR_STATE,
                  NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);
}

static void put_s16(unsigned i, int16_t x)
{
    memcpy(s_ram + DATA_BASE + 2 * i, &x, 2);
}

/* One frame of voice 0 at `rate`, the way voice_process asks for it. */
static int resample_frame(MCPXAPUState *d, float rate,
                          float out[NUM_SAMPLES_PER_FRAME][2])
{
    int n = 0, empty = 0;
    while (n < NUM_SAMPLES_PER_FRAME) {
        int c = voice_resample(d, 0, &out[n], NUM_SAMPLES_PER_FRAME - n, rate);
        if (c < 0 || (c == 0 && ++empty > 4))
            break;
        n += c;
    }
    return n;
}

static void resample_rate_one_plays_this_frames_buffer(void)
{
    static MCPXAPUState d;
    float out[NUM_SAMPLES_PER_FRAME][2];

    setup_pcm_loop_voice(&d, NUM_SAMPLES_PER_FRAME);
    for (int frame = 0; frame < 6; frame++) {
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
            put_s16((unsigned)i, (int16_t)(frame * 1000 + i * 7 + 1));
        int n = resample_frame(&d, 1.0f, out);
        CHECK(n == NUM_SAMPLES_PER_FRAME, "frame %d: %d samples", frame, n);
        for (int i = 0; i < n; i++) {
            float want = int16_to_float((int16_t)(frame * 1000 + i * 7 + 1));
            if (out[i][0] != want) {
                CHECK(0, "frame %d: sample %d is %f, this frame's buffer has %f",
                      frame, i, out[i][0], want);
                break;
            }
        }
    }
}

/* Not a regression check: pins that positions off rate 1 are unchanged. */
static void resample_half_rate_lands_on_source_samples(void)
{
    static MCPXAPUState d;
    float out[NUM_SAMPLES_PER_FRAME][2];
    const unsigned len = 1024;

    setup_pcm_loop_voice(&d, len);
    for (unsigned i = 0; i < len; i++)
        put_s16(i, (int16_t)(i * 13));
    /* Rate 0.5: two source samples an output, each on a whole position. */
    for (int frame = 0; frame < 4; frame++) {
        int n = resample_frame(&d, 0.5f, out);
        CHECK(n == NUM_SAMPLES_PER_FRAME, "frame %d: %d samples", frame, n);
        for (int i = 0; i < n; i++) {
            unsigned at = 2u * (unsigned)(frame * NUM_SAMPLES_PER_FRAME + i);
            float want = int16_to_float((int16_t)(at * 13));
            if (out[i][0] != want) {
                CHECK(0, "frame %d: output %d is %f, source %u is %f",
                      frame, i, out[i][0], at, want);
                break;
            }
        }
    }
}

/* Rate 0.3 over a ramp: output k sits at source position k / 0.3. A dropped
 * source sample shifts every later output a whole position (13 ramp units),
 * far outside the rounding tolerance. */
static void resample_rate_0_3_drops_no_source_sample(void)
{
    static MCPXAPUState d;
    float out[NUM_SAMPLES_PER_FRAME][2];
    const unsigned len = 1024;
    const float step = 1.0f / 0.3f;

    setup_pcm_loop_voice(&d, len);
    for (unsigned i = 0; i < len; i++)
        put_s16(i, (int16_t)(i * 13));
    for (int frame = 0; frame < 8; frame++) {
        int n = resample_frame(&d, 0.3f, out);
        CHECK(n == NUM_SAMPLES_PER_FRAME, "frame %d: %d samples", frame, n);
        for (int i = 0; i < n; i++) {
            double at = (double)(frame * NUM_SAMPLES_PER_FRAME + i) * step;
            double got = out[i][0] * 32768.0 / 13.0;
            if (fabs(got - at) > 0.05) {
                CHECK(0, "frame %d: output %d sits at source %.3f, expected %.3f",
                      frame, i, got, at);
                break;
            }
        }
    }
}

/* EBO is inclusive: an 8-sample loop (EBO 7) fetched in 7s must still play
 * sample 7. The voice wrapped when a fetch ended at cbo == ebo, before
 * sample ebo was read, so it played 0..6 over and over. */
static void loop_fetch_ending_before_ebo_plays_ebo(void)
{
    static MCPXAPUState d;
    float samples[7][2];
    const unsigned len = 8;

    setup_pcm_loop_voice(&d, len);
    for (unsigned i = 0; i < len; i++)
        put_s16(i, (int16_t)(i * 100 + 1));
    unsigned at = 0;
    for (int fetch = 0; fetch < 3; fetch++) {
        int n = voice_get_samples(&d, 0, samples, 7);
        CHECK(n > 0, "fetch %d: %d samples", fetch, n);
        for (int i = 0; i < n; i++, at++) {
            float want = int16_to_float((int16_t)((at % len) * 100 + 1));
            if (samples[i][0] != want) {
                CHECK(0, "fetch %d: sample %d is %f, loop position %u has %f",
                      fetch, i, samples[i][0], at % len, want);
                return;
            }
        }
    }
    /* A fetch stops at the loop end, so 7, 1 (sample 7), then 7 again. */
    CHECK(at == 15, "3 fetches played %u samples, expected 7 + 1 + 7", at);
}

/* The same for a one-shot voice: a fetch that stops before EBO leaves the
 * voice on, and the next one plays sample ebo before the voice ends. */
static void one_shot_fetch_ending_before_ebo_plays_ebo(void)
{
    static MCPXAPUState d;
    float samples[7][2];

    setup_pcm_loop_voice(&d, 8);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28));
    for (unsigned i = 0; i < 8; i++)
        put_s16(i, (int16_t)(i * 100 + 1));
    int n = voice_get_samples(&d, 0, samples, 7);
    CHECK(n == 7, "first fetch: %d samples, expected 7", n);
    CHECK(voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE,
                         NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
          "voice ended after sample 6, before playing sample 7");
    n = voice_get_samples(&d, 0, samples, 7);
    CHECK(n == 1, "second fetch: %d samples, expected sample 7 alone", n);
    if (n >= 1)
        CHECK(samples[0][0] == int16_to_float(701),
              "second fetch played %f, sample 7 is %f", samples[0][0],
              int16_to_float(701));
    CHECK(!voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
          "voice still on after its last sample");
    uint32_t cbo = voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_OFFSET,
                                  NV_PAVS_VOICE_PAR_OFFSET_CBO);
    CHECK(cbo == 7, "ended voice has CBO %u, expected EBO 7", cbo);
}

#define SSL_BASE 0x50000u   /* NV_PAPU_VPSSLADDR */
#define FEN_BASE 0x60000u   /* NV_PAPU_FENADDR */

/* The same for a stream voice: one 8-sample SSL segment fetched in 7s plays
 * sample 7, and the segment is done (SSLA_DONE, the move to the next SSL)
 * only after it. The old test moved on at cbo == ebo, dropping a packet's
 * last sample and signalling it done a fetch early. */
static void stream_fetch_ending_before_ebo_plays_ebo(void)
{
    static MCPXAPUState d;
    float samples[7][2];
    uint32_t ssl[2] = { DATA_BASE,
                        8u | (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 16) };
    const uint32_t done_at = FEN_BASE + 16u * (MCPX_HW_NOTIFIER_BASE_OFFSET +
                                               MCPX_HW_NOTIFIER_SSLA_DONE) + 15u;

    setup_pcm_loop_voice(&d, 8);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
                  NV_PAVS_VOICE_CFG_FMT_DATA_TYPE |
                  NV_PAVS_VOICE_CFG_FMT_PERSIST);
    d.regs[NV_PAPU_VPSSLADDR] = SSL_BASE;
    d.regs[NV_PAPU_FENADDR] = FEN_BASE;
    memcpy(s_ram + SSL_BASE, ssl, sizeof(ssl));
    d.vp.ssl[0].base[0] = 0;
    d.vp.ssl[0].count[0] = 1;
    for (unsigned i = 0; i < 8; i++)
        put_s16(i, (int16_t)(i * 100 + 1));

    int n = voice_get_samples(&d, 0, samples, 7);
    CHECK(n == 7, "first fetch: %d samples, expected 7", n);
    CHECK(s_ram[done_at] == 0 && d.vp.ssl[0].ssl_index == 0 &&
          d.vp.ssl[0].ssl_seg == 0,
          "segment done after sample 6 (SSLA_DONE %u, ssl %d seg %d), before "
          "playing sample 7", s_ram[done_at], d.vp.ssl[0].ssl_index,
          d.vp.ssl[0].ssl_seg);
    n = voice_get_samples(&d, 0, samples, 7);
    CHECK(n == 1, "second fetch: %d samples, expected sample 7 alone", n);
    if (n >= 1)
        CHECK(samples[0][0] == int16_to_float(701),
              "second fetch played %f, sample 7 is %f", samples[0][0],
              int16_to_float(701));
    CHECK(s_ram[done_at] == NV1BA0_NOTIFICATION_STATUS_DONE_SUCCESS &&
          d.vp.ssl[0].ssl_index == 1,
          "segment not done after sample 7 (SSLA_DONE %u, ssl %d)",
          s_ram[done_at], d.vp.ssl[0].ssl_index);
}

/* A 40-sample one-shot (EBO 39) at rate 1 ends 8 samples into its second
 * frame. Those 8 samples, sample ebo last, must reach the mix bins; the
 * frame after, with the voice off, adds nothing. Voice 0 is a 3D voice with
 * every bin on mix bin 0, unit volume and envelope, so bin 0 gets 8 times
 * each sample. */
static void one_shot_final_frame_reaches_mix_bins(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    const unsigned len = 40;

    setup_pcm_loop_voice(&d, len);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_HRTF_TARGET, HRTF_NULL_HANDLE);
    for (unsigned i = 0; i < len; i++)
        put_s16(i, (int16_t)(i * 100 + 1));

    for (int frame = 0; frame < 3; frame++) {
        memset(mixbins, 0, sizeof(mixbins));
        voice_process(&d, mixbins, sample_buf, 0, 0);
        for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
            unsigned at = (unsigned)(frame * NUM_SAMPLES_PER_FRAME + i);
            float want = at < len ? 8.0f * int16_to_float((int16_t)(at * 100 + 1))
                                  : 0.0f;
            if (fabsf(mixbins[0][i] - want) > 1e-6f) {
                CHECK(0, "frame %d: mix bin 0 sample %d is %f, expected %f "
                      "(voice sample %u of %u)", frame, i, mixbins[0][i], want,
                      at, len);
                break;
            }
        }
    }
    CHECK(!voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
          "voice still on after its last sample");
}

/* Rate 0.5: each output takes two source samples, so a 40-sample one-shot
 * ends 20 outputs into its first frame, the resampler returning those 20
 * before the voice is off. They must reach the mix bins (source 0, 2, ..
 * 38, each on a whole position). */
static void one_shot_half_rate_final_frame_reaches_mix_bins(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    const unsigned len = 40;

    setup_pcm_loop_voice(&d, len);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28));
    set_voice_reg(0, NV_PAVS_VOICE_CFG_HRTF_TARGET, HRTF_NULL_HANDLE);
    voice_set_mask(&d, 0, NV_PAVS_VOICE_TAR_PITCH_LINK,
                   NV_PAVS_VOICE_TAR_PITCH_LINK_PITCH, 4096);  /* rate 0.5 */
    for (unsigned i = 0; i < len; i++)
        put_s16(i, (int16_t)(i * 100 + 1));

    memset(mixbins, 0, sizeof(mixbins));
    voice_process(&d, mixbins, sample_buf, 0, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        unsigned at = 2u * (unsigned)i;
        float want = at < len ? 8.0f * int16_to_float((int16_t)(at * 100 + 1))
                              : 0.0f;
        if (fabsf(mixbins[0][i] - want) > 1e-6f) {
            CHECK(0, "mix bin 0 sample %d is %f, expected %f (source %u of %u)",
                  i, mixbins[0][i], want, at, len);
            break;
        }
    }
    CHECK(!voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
          "voice still on after its last sample");
}

/* A non-persist stream with one 8-sample packet and nothing queued after
 * it: the frame plays the packet, then the empty next SSL turns the voice
 * off. The 8 samples must reach the mix bins. A non-persist stream plays
 * only in its release phase (voice_get_samples turns it off before that),
 * so the envelope sits early in a long release: one gain g for the frame,
 * close to 1, and bin 0 holds 8 * g * each sample. */
static void stream_end_final_frame_reaches_mix_bins(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    static float sample_buf[NUM_SAMPLES_PER_FRAME][2];
    uint32_t ssl[2] = { DATA_BASE,
                        8u | (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 16) };

    setup_pcm_loop_voice(&d, 8);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_FMT,
                  (NV_PAVS_VOICE_CFG_FMT_CONTAINER_SIZE_B16 << 30) |
                  (NV_PAVS_VOICE_CFG_FMT_SAMPLE_SIZE_S16 << 28) |
                  NV_PAVS_VOICE_CFG_FMT_DATA_TYPE);
    set_voice_reg(0, NV_PAVS_VOICE_CFG_HRTF_TARGET, HRTF_NULL_HANDLE);
    voice_set_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE, NV_PAVS_VOICE_PAR_STATE_EACUR,
                   NV_PAVS_VOICE_PAR_STATE_EFCUR_RELEASE);
    voice_set_mask(&d, 0, NV_PAVS_VOICE_CUR_ECNT, NV_PAVS_VOICE_CUR_ECNT_EACOUNT,
                   0xFFF0);
    voice_set_mask(&d, 0, NV_PAVS_VOICE_TAR_LFO_ENV,
                   NV_PAVS_VOICE_TAR_LFO_ENV_EA_RELEASERATE, 0xFFF);
    voice_set_mask(&d, 0, NV_PAVS_VOICE_PAR_OFFSET,
                   NV_PAVS_VOICE_PAR_OFFSET_EALVL, 0xFF);
    d.regs[NV_PAPU_VPSSLADDR] = SSL_BASE;
    d.regs[NV_PAPU_FENADDR] = FEN_BASE;
    memcpy(s_ram + SSL_BASE, ssl, sizeof(ssl));
    d.vp.ssl[0].base[0] = 0;
    d.vp.ssl[0].count[0] = 1;
    d.vp.ssl[0].count[1] = 0;
    for (unsigned i = 0; i < 8; i++)
        put_s16(i, (int16_t)(i * 100 + 1));

    memset(mixbins, 0, sizeof(mixbins));
    voice_process(&d, mixbins, sample_buf, 0, 0);
    float g = mixbins[0][0] / (8.0f * int16_to_float(1));
    CHECK(g > 0.99f && g <= 1.0f, "stream's frame mixed at gain %f, expected "
          "its release envelope near 1", g);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float want = i < 8 ? 8.0f * g * int16_to_float((int16_t)(i * 100 + 1))
                           : 0.0f;
        if (fabsf(mixbins[0][i] - want) > 1e-5f) {
            CHECK(0, "mix bin 0 sample %d is %f, expected %f (packet has 8)",
                  i, mixbins[0][i], want);
            break;
        }
    }
    CHECK(!voice_get_mask(&d, 0, NV_PAVS_VOICE_PAR_STATE,
                          NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE),
          "stream still on with no packet left");
}

/* Voice 100 (not 3D, so no HRTF) in multipass mode: its input is mixbin 20,
 * held at 1.0, and only bin 1 is audible. Full volume for a frame, then
 * silent: the second frame must fall from the first's level to nothing. */
static void gain_ramps_across_a_frame(void)
{
    static MCPXAPUState d;
    static float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];
    const uint16_t v = 100;
    const uint32_t vola_silent = 0xFFFFFFFFu;  /* every volume 0xFFF: off */
    const uint32_t vola_full = vola_silent & ~NV_PAVS_VOICE_TAR_VOLA_VOLUME0;

    memset(s_ram, 0, RAM_SIZE);
    memset(&d, 0, sizeof(d));
    d.regs[NV_PAPU_VPVADDR] = VOICE_BASE;
    set_voice_reg(v, NV_PAVS_VOICE_CFG_FMT,
                  NV_PAVS_VOICE_CFG_FMT_MULTIPASS | (20u << 16));
    set_voice_reg(v, NV_PAVS_VOICE_PAR_STATE,
                  NV_PAVS_VOICE_PAR_STATE_ACTIVE_VOICE);  /* envelopes off: 1.0 */
    set_voice_reg(v, NV_PAVS_VOICE_CFG_VBIN, 1);              /* V0BIN = 1 */
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLB, 0xFFFFFFFFu);
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLC, 0xFFFFFFFFu);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        mixbins[20][i] = 1.0f;

    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLA, vola_full);
    voice_process(&d, mixbins, d.vp.sample_buf, v, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++)
        if (mixbins[1][i] != 1.0f) {
            CHECK(0, "frame 1 sample %d is %f, expected the voice's level 1.0",
                  i, mixbins[1][i]);
            break;
        }

    memset(mixbins[1], 0, sizeof(mixbins[1]));
    set_voice_reg(v, NV_PAVS_VOICE_TAR_VOLA, vola_silent);
    voice_process(&d, mixbins, d.vp.sample_buf, v, 0);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        float expect = 1.0f - (float)(i + 1) / NUM_SAMPLES_PER_FRAME;
        if (fabsf(mixbins[1][i] - expect) > 1e-6f) {
            CHECK(0, "frame 2 sample %d is %f, expected the ramp's %f",
                  i, mixbins[1][i], expect);
            break;
        }
    }
}

/* The decoders read samples through mcpx_apu_phys_data (an image page the
 * title named) and the voice and SGE tables through mcpx_apu_phys.
 * The two fake RAMs hold different sample bytes and only s_ram holds the
 * tables, so a read on the wrong resolver decodes the wrong samples. */
static void sample_reads_take_the_data_resolver(void)
{
    static MCPXAPUState d;
    float out[NUM_SAMPLES_PER_FRAME][2];
    int16_t expect[65 * 2];
    uint8_t *data = (uint8_t *)calloc(1, RAM_SIZE);
    int n;

    if (!data) { CHECK(0, "no memory for the second fake RAM"); return; }

    /* ADPCM: one block, its bytes in data; s_ram's copy is scrambled. */
    setup_adpcm_voice(&d, 1);
    memcpy(data + DATA_BASE, s_ram + DATA_BASE, 36);
    for (unsigned i = 4; i < 36; i++)
        s_ram[DATA_BASE + i] ^= 0x5A;
    adpcm_decode_block(expect, data + DATA_BASE, 36, 1);
    s_data = data;
    memset(out, 0, sizeof(out));
    n = voice_get_samples(&d, 0, out, NUM_SAMPLES_PER_FRAME);
    CHECK(n == NUM_SAMPLES_PER_FRAME, "ADPCM: %d samples", n);
    for (int i = 0; i < n; i++)
        if (out[i][0] != int16_to_float(expect[i])) {
            CHECK(0, "ADPCM: sample %d is %f, the data resolver's block says %f",
                  i, out[i][0], int16_to_float(expect[i]));
            break;
        }
    s_data = NULL;

    /* PCM S16: a looped frame, different values on each side. */
    setup_pcm_loop_voice(&d, NUM_SAMPLES_PER_FRAME);
    for (int i = 0; i < NUM_SAMPLES_PER_FRAME; i++) {
        int16_t x = (int16_t)(i * 211 + 3);
        memcpy(data + DATA_BASE + 2 * i, &x, 2);
        put_s16((unsigned)i, (int16_t)-x);
    }
    s_data = data;
    n = resample_frame(&d, 1.0f, out);
    CHECK(n == NUM_SAMPLES_PER_FRAME, "PCM: %d samples", n);
    for (int i = 0; i < n; i++)
        if (out[i][0] != int16_to_float((int16_t)(i * 211 + 3))) {
            CHECK(0, "PCM: sample %d is %f, the data resolver's buffer has %f",
                  i, out[i][0], int16_to_float((int16_t)(i * 211 + 3)));
            break;
        }
    s_data = NULL;
    free(data);
}

int main(void)
{
    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;

    adpcm_wide_block_decodes();
    adpcm_whole_field_fits();
    outbuf_sge_goes_to_gp_fifo_table();
    outbuf_sge_without_table_is_dropped();
    outbuf_sge_past_max_is_dropped();
    resample_rate_one_plays_this_frames_buffer();
    resample_half_rate_lands_on_source_samples();
    resample_rate_0_3_drops_no_source_sample();
    loop_fetch_ending_before_ebo_plays_ebo();
    one_shot_fetch_ending_before_ebo_plays_ebo();
    stream_fetch_ending_before_ebo_plays_ebo();
    one_shot_final_frame_reaches_mix_bins();
    one_shot_half_rate_final_frame_reaches_mix_bins();
    stream_end_final_frame_reaches_mix_bins();
    gain_ramps_across_a_frame();
    sample_reads_take_the_data_resolver();

    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: every ADPCM block count the descriptor can name decodes; output buffer SGE entries go to the GP FIFO table; the "
         "resampler plays each frame's source, no sample held back; a fetch "
         "ending before EBO still plays sample EBO; the frame a voice ends in "
         "is mixed (one-shot at rate 1 and 0.5, stream end); volume changes "
         "ramp across a frame; samples are read through the data resolver and the "
         "voice and SGE tables through the other.");
    return 0;
}
