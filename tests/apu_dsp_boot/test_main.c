/* GPRST/EPRST going to 3 in the MCPX window copies the first 0x800 words of
 * that core's scratch (through its SGE table) into P:0, 24 bits a word, as
 * the boot ROM does; the stub runs nothing, but KUF: Heroes' DirectSound
 * reads EP P:6 back and resets both cores whenever it is not the program it
 * loaded.
 *
 * A zeroed buffer stands in for RAM (mcpx_apu_phys resolves a low physical
 * address to g_apu_ram_ptr + address) and another for the MCPX window. */
#include "apu_state.h"
#include "apu_shim.h"
#include "apu_dsp_emu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The runtime links against the title's dispatch; nothing here calls it. */
void *recomp_lookup(unsigned long address) { (void)address; abort(); }
void *recomp_lookup_manual(unsigned long address) { (void)address; abort(); }

#define FAKE_RAM   (256u << 10)
#define EP_SGE     0x00010000u   /* what EPSADDR points at */
#define EP_PAGE0   0x00020000u   /* scratch page 0 */
#define EP_PAGE1   0x00030000u   /* scratch page 1, not contiguous */
#define GP_SGE     0x00011000u
#define GP_PAGE0   0x00024000u
#define MCPX_BYTES 0x60000u
#define EP_WIN     0x50000u
#define GP_WIN     0x30000u

static int failures;

static void check(int ok, const char *what)
{
    printf("  %-58s %s\n", what, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static uint32_t *dword(uint8_t *base, uint32_t off)
{
    return (uint32_t *)(base + off);
}

static uint32_t p_word(uint8_t *mcpx, uint32_t win, uint32_t i)
{
    return *dword(mcpx, win + APU_DSP_WIN_P + i * 4);
}

static void set_rst(uint8_t *mcpx, uint32_t win, uint32_t v)
{
    *dword(mcpx, win + APU_DSP_WIN_RST) = v;
}

int main(void)
{
    MCPXAPUState *d = (MCPXAPUState *)calloc(1, sizeof(*d));
    uint8_t *ram = (uint8_t *)calloc(1, FAKE_RAM);
    uint8_t *mcpx = (uint8_t *)calloc(1, MCPX_BYTES);
    uint32_t i;
    int ok;

    if (!d || !ram || !mcpx) { fprintf(stderr, "out of memory\n"); return 2; }
    setvbuf(stdout, NULL, _IONBF, 0);
    g_apu_ram_ptr = ram;
    d->ram_ptr = ram;
    mcpx_apu_dsp_init(d);

    /* EP scratch: word i is 0xAB000000 | i, page 1 named by SGE[1]. */
    d->regs[NV_PAPU_EPSADDR] = EP_SGE;
    d->regs[NV_PAPU_EPSMAXSGE] = 1;
    *dword(ram, EP_SGE) = EP_PAGE0;
    *dword(ram, EP_SGE + 8) = EP_PAGE1;
    for (i = 0; i < 0x400; i++) {
        *dword(ram, EP_PAGE0 + i * 4) = 0xAB000000u | i;
        *dword(ram, EP_PAGE1 + i * 4) = 0xAB000000u | (0x400 + i);
    }
    *dword(ram, EP_PAGE0 + 6 * 4) = 0x00CCCCCCu;  /* DOLBY word 6 */
    *dword(mcpx, EP_WIN + APU_DSP_WIN_P + 0x800 * 4) = 0x123456u;

    /* In reset: nothing copied. */
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, EP_WIN, 6) == 0, "EPRST 0: P untouched");
    set_rst(mcpx, EP_WIN, 1);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, EP_WIN, 6) == 0, "EPRST 1 (DSP held): P untouched");

    set_rst(mcpx, EP_WIN, 3);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, EP_WIN, 6) == 0xCCCCCCu, "EPRST 3: P:6 is scratch word 6");
    ok = 1;
    for (i = 0; i < APU_DSP_BOOT_WORDS; i++)
        if (i != 6 && p_word(mcpx, EP_WIN, i) != i)
            ok = 0;
    check(ok, "0x800 words copied across both SGE pages, 24 bits each");
    check(p_word(mcpx, EP_WIN, 0x800) == 0x123456u, "P:0x800 and up left alone");
    check(p_word(mcpx, GP_WIN, 6) == 0, "GP window untouched");

    /* Running: the title's own P writes stay until the next reset. */
    *dword(mcpx, EP_WIN + APU_DSP_WIN_P + 6 * 4) = 0x777u;
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, EP_WIN, 6) == 0x777u, "still running: no second copy");
    set_rst(mcpx, EP_WIN, 0);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    set_rst(mcpx, EP_WIN, 3);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, EP_WIN, 6) == 0xCCCCCCu, "reset then 3 again: copied again");

    /* GP with one table entry: the second page is past GPSMAXSGE,
     * so P keeps what it had from word 0x400 on. */
    d->regs[NV_PAPU_GPSADDR] = GP_SGE;
    d->regs[NV_PAPU_GPSMAXSGE] = 0;
    *dword(ram, GP_SGE) = GP_PAGE0;
    for (i = 0; i < 0x400; i++)
        *dword(ram, GP_PAGE0 + i * 4) = 0x100u + i;
    *dword(mcpx, GP_WIN + APU_DSP_WIN_P + 0x400 * 4) = 0x55u;
    set_rst(mcpx, GP_WIN, 3);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, GP_WIN, 0x3FF) == 0x4FFu, "GPRST 3: page 0 copied");
    check(p_word(mcpx, GP_WIN, 0x400) == 0x55u, "GP: past GPSMAXSGE, P kept");

    /* No table: a core taken out of reset with GPSADDR unset copies nothing. */
    mcpx_apu_dsp_init(d);
    d->regs[NV_PAPU_GPSADDR] = 0;
    memset(mcpx + GP_WIN + APU_DSP_WIN_P, 0, 0x2000);
    mcpx_apu_dsp_boot_poll(d, mcpx);
    check(p_word(mcpx, GP_WIN, 0) == 0, "GPSADDR unset: P untouched");

    mcpx_apu_dsp_finalize(d);
    printf("%s\n", failures ? "FAILED" : "all passed");
    return failures ? 1 : 0;
}
