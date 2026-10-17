/* The voice processor against a small fake guest RAM.
 *
 * apu_vp.c is compiled into this file so its static helpers can be called
 * directly. Each check below fails on the code it was written for:
 *   - adpcm_*: the decode buffers held two 36-byte blocks' worth, so a
 *     wider samples-per-block value overran them on the stack (a mono
 *     voice already at two).
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

static void adpcm_too_wide_block_is_refused(void)
{
    static MCPXAPUState d;
    float samples[NUM_SAMPLES_PER_FRAME][2];

    setup_adpcm_voice(&d, ADPCM_MAX_BLOCKS + 1);
    int n = voice_get_samples(&d, 0, samples, NUM_SAMPLES_PER_FRAME);
    CHECK(n == -1, "%d blocks: got %d samples, expected the voice refused",
          ADPCM_MAX_BLOCKS + 1, n);
}

int main(void)
{
    s_ram = (uint8_t *)calloc(1, RAM_SIZE);
    if (!s_ram) return 2;

    adpcm_wide_block_decodes();
    adpcm_too_wide_block_is_refused();

    free(s_ram);
    if (s_failures) {
        fprintf(stderr, "%d check(s) failed\n", s_failures);
        return 1;
    }
    puts("PASS: ADPCM blocks up to the hardware limit decode, wider ones are "
         "refused.");
    return 0;
}
