/* The APU brought up and torn down several times in one process.
 *
 * Three kinds of state outlived an instance. The =auto doorbell ack once
 * cached the slot it derived from GPSADDR as a host pointer: a new instance
 * whose title programmed the same GPSADDR acked a dword in the old RAM and
 * never the new one. The ack keeps its configuration and the GPRST gate in
 * file statics: a GP held in reset by one instance left the ack paused in
 * the next, and the RECOMP_APU_DSP_ACK setting read by the first instance
 * was never read again. And shutdown never released the GP/EP state that
 * init allocates.
 *
 * Small zeroed buffers stand in for RAM, as in apu_dsp_ack: with no heap,
 * no image and no contiguous allocation above page 0, mcpx_apu_phys resolves
 * a low physical address to the buffer + address. The frame thread idles
 * after init (pause_requested) until something resumes it, so the test polls
 * the doorbell itself. */
#include "apu_state.h"
#include "apu.h"
#include "apu_xaudio2.h"
#include "apu_regs.h"
#include "recomp_env.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__APPLE__)
#  include <malloc/malloc.h>
#elif defined(__GLIBC__)
#  include <malloc.h>
#endif

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

/* ---- fake device: every xa2_* symbol, so apu_xaudio2.o is not linked ---- */
static int dev_inits, dev_shutdowns, dev_open;
int  xa2_init(void)            { dev_inits++; dev_open = 1; return 1; }
void xa2_shutdown(void)        { dev_shutdowns++; dev_open = 0; }
int  xa2_is_active(void)       { return dev_open; }
int  xa2_get_buffer_size(void) { return 512; }
int  xa2_get_buffer_count(void) { return 4; }
unsigned xa2_underruns(void)   { return 0; }
int  xa2_queued_samples(void)  { return 0; }
uint64_t xa2_samples_played(void) { return 0; }
void xa2_expect_drain(void)    {}
int  xa2_submit_samples(const int16_t *s, int n) { (void)s; (void)n; return 1; }

#define FAKE_RAM   (256u << 10)
#define SGE_TABLE  0x00010000u
#define GP_PAGE    0x00020000u
#define DOORBELL   (GP_PAGE + 0x810u)

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-62s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

/* recomp_env reads the environment once per process; set the value it
 * hands out instead, as a changed environment would after a reload. */
static void set_ack_env(const char *v)
{
    recomp_env_set(RENV_APU_DSP_ACK, v);
}

static uint32_t *dword(uint8_t *ram, uint32_t phys)
{
    return (uint32_t *)(ram + phys);
}

/* A new instance whose GP scratch table names GP_PAGE. */
static MCPXAPUState *bring_up(uint8_t *ram)
{
    MCPXAPUState *d = mcpx_apu_init_standalone(ram);
    if (d) {
        d->regs[NV_PAPU_GPSADDR] = SGE_TABLE;
        *dword(ram, SGE_TABLE) = GP_PAGE;
    }
    return d;
}

#if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define APU_TEST_ASAN 1
#  endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#  define APU_TEST_ASAN 1
#endif

/* Bytes the allocator holds, or -1 where the host has no cheap count. Under
 * ASan the quarantine holds freed blocks, so the count would read as growth. */
static long long heap_in_use(void)
{
#if defined(APU_TEST_ASAN)
    return -1;
#elif defined(__APPLE__)
    malloc_statistics_t s;
    malloc_zone_statistics(NULL, &s);
    return (long long)s.size_in_use;
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
    struct mallinfo2 m = mallinfo2();
    return (long long)(m.uordblks + m.hblkhd);
#else
    return -1;
#endif
}

/* Same GPSADDR, new RAM: the new instance acks in its own RAM. Both buffers
 * stay allocated, so the old behaviour writes a live buffer, not freed
 * memory. */
static void doorbell_case(uint8_t *ram_a, uint8_t *ram_b)
{
    MCPXAPUState *d;

    set_ack_env("auto");
    d = bring_up(ram_a);

    if (!d) { failures++; return; }
    *dword(ram_a, DOORBELL) = 0x11;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram_a, DOORBELL) == 0, "first instance: poll clears its doorbell");
    mcpx_apu_shutdown(d);

    d = bring_up(ram_b);
    if (!d) { failures++; return; }
    *dword(ram_a, DOORBELL) = 0x22;
    *dword(ram_b, DOORBELL) = 0x33;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram_b, DOORBELL) == 0, "reinit, same GPSADDR: poll clears the new RAM's doorbell");
    check(*dword(ram_a, DOORBELL) == 0x22, "reinit: the old RAM is left alone");
    mcpx_apu_shutdown(d);
}

/* A GP held in reset when the instance went away: the next init must start
 * with the GP running, as a fresh process does. */
static void gate_case(uint8_t *ram)
{
    MCPXAPUState *d;

    set_ack_env("auto");
    d = bring_up(ram);
    if (!d) { failures++; return; }
    mcpx_apu_dsp_gprst_write(0);                       /* hold the GP */
    *dword(ram, DOORBELL) = 0x11;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x11, "held in reset: poll leaves the doorbell");
    mcpx_apu_shutdown(d);

    d = bring_up(ram);
    if (!d) { failures++; return; }
    *dword(ram, DOORBELL) = 0x22;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0, "reinit: GP counts as running, poll clears it");
    mcpx_apu_shutdown(d);
}

/* The ack setting is read again by each instance. */
static void config_case(uint8_t *ram)
{
    MCPXAPUState *d;

    set_ack_env("off");
    d = bring_up(ram);
    if (!d) { failures++; return; }
    *dword(ram, DOORBELL) = 0x33;
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0x33, "RECOMP_APU_DSP_ACK=off: poll leaves it");
    mcpx_apu_shutdown(d);

    set_ack_env("auto");
    d = bring_up(ram);
    if (!d) { failures++; return; }
    mcpx_apu_dsp_ack_poll(d);
    check(*dword(ram, DOORBELL) == 0, "reinit with =auto: poll clears it");
    mcpx_apu_shutdown(d);
}

/* Several init/shutdown cycles: the device is opened and closed once each,
 * and the heap does not grow by the GP/EP state per cycle. A double free
 * aborts in the allocator. The first cycle is the baseline: lazy one-time
 * allocations land there. */
static void cycle_case(uint8_t *ram)
{
    const int cycles = 4;
    long long base = -1, now = -1;
    int i, i0 = dev_inits, s0 = dev_shutdowns;
    char what[96];

    for (i = 0; i < cycles; i++) {
        MCPXAPUState *d = bring_up(ram);
        if (!d) { failures++; return; }
        mcpx_apu_shutdown(d);
        now = heap_in_use();
        if (i == 0)
            base = now;
    }
    snprintf(what, sizeof what, "%d cycles: device opened %d, closed %d", cycles,
             dev_inits - i0, dev_shutdowns - s0);
    check(dev_inits - i0 == cycles && dev_shutdowns - s0 == cycles, what);
    if (base < 0) {
        printf("  %-62s skipped\n", "heap growth (no allocator count here, or under ASan)");
        return;
    }
    snprintf(what, sizeof what, "heap growth over %d cycles: %lld bytes (DSPState %zu)",
             cycles - 1, now - base, sizeof(DSPState));
    check(now - base < (long long)sizeof(DSPState), what);
}

int main(void)
{
    uint8_t *ram_a = (uint8_t *)calloc(1, FAKE_RAM);
    uint8_t *ram_b = (uint8_t *)calloc(1, FAKE_RAM);

    if (!ram_a || !ram_b) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    doorbell_case(ram_a, ram_b);
    gate_case(ram_b);
    config_case(ram_b);
    cycle_case(ram_b);
    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    free(ram_a);
    free(ram_b);
    return failures ? 1 : 0;
}
