/*
 * apu_dsp_emu: the GP device model (src/apu/apu_dsp_emu.c) on the dsp56300
 * JIT, without the APU around it. A hand-assembled GP program, bootstrapped
 * from scratch by the GPRST edge, runs a two-descriptor DMA chain (scratch
 * into X, then X out to FIFO 0), reads the DMA status back the way DSOUND's
 * kernel polls it, writes the mixbuf at X:0x1400 and halts.
 */
#include "apu_dsp_emu.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);            \
            fprintf(stderr, __VA_ARGS__);                                   \
            fputc('\n', stderr);                                            \
            failures++;                                                     \
        }                                                                   \
    } while (0)

static uint8_t scratch[0x4000];
static uint8_t fifo0[64];
static size_t fifo0_len;

static void scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr, size_t len,
                       bool dir)
{
    (void)opaque;
    if (addr + len > sizeof(scratch)) {
        fprintf(stderr, "FAIL scratch access %x+%zx\n", addr, len);
        failures++;
        return;
    }
    if (dir)
        memcpy(scratch + addr, ptr, len);
    else
        memcpy(ptr, scratch + addr, len);
}

static void fifo_rw(void *opaque, uint8_t *ptr, unsigned index, size_t len,
                    bool dir)
{
    (void)opaque;
    CHECK(index == 0 && dir, "fifo %u dir %d", index, dir);
    if (fifo0_len + len <= sizeof(fifo0)) {
        memcpy(fifo0 + fifo0_len, ptr, len);
        fifo0_len += len;
    }
}

/* The program (dsp56300-disasm):
 *   P:00 0A8581 000000  jclr #1,x:$ffffc5,$0000    wait START_FRAME
 *   P:02 08F485 000002  movep #2,x:$ffffc5          ack it
 *   P:04 56F400 000010  move #>$10,a
 *   P:06 08CE14         movep a,x:$ffffd4           next_block = X:$10
 *   P:07 08F496 000001  movep #1,x:$ffffd6          start
 *   P:09..P:14          move x:$ffffd6,x0 / move x0,x:$200+i, three times
 *   P:15 44F000 FFFFC5  move x:$ffffc5,x0
 *   P:17 447000 000203  move x0,x:$203
 *   P:19 08F485 000080  movep #$80,x:$ffffc5        clear EOL
 *   P:1B 44F400 123456  move #>$123456,x0
 *   P:1D 447000 001400  move x0,x:$1400
 *   P:1F 447000 001421  move x0,x:$1421
 *   P:21 08F484 000001  movep #1,x:$ffffc4          halt
 *   P:23 0C0000         jmp $0000
 */
static const uint32_t prog[] = {
    0x0A8581, 0x000000, 0x08F485, 0x000002, 0x56F400, 0x000010, 0x08CE14,
    0x08F496, 0x000001, 0x44F000, 0xFFFFD6, 0x447000, 0x000200, 0x44F000,
    0xFFFFD6, 0x447000, 0x000201, 0x44F000, 0xFFFFD6, 0x447000, 0x000202,
    0x44F000, 0xFFFFC5, 0x447000, 0x000203, 0x08F485, 0x000080, 0x44F400,
    0x123456, 0x447000, 0x001400, 0x447000, 0x001421, 0x08F484, 0x000001,
    0x0C0000,
};
#define PROG_IMM 0x1C   /* the #>$123456 extension word */

#define SCRATCH_DATA 0x2000u
#define CAP 200000u

int main(void)
{
    /* Program at the start of scratch, for the boot ROM copy. Then eight
     * words of data, with top bytes the DMA must drop. */
    memcpy(scratch, prog, sizeof(prog));
    uint32_t data[8];
    for (uint32_t i = 0; i < 8; i++)
        data[i] = 0xA5000000u | (0x111111u * (i + 1));
    memcpy(scratch + SCRATCH_DATA, data, sizeof(data));

    ApuDspEmuConfig cfg = { .name = "GP", .is_gp = true,
                            .scratch_rw = scratch_rw, .fifo_rw = fifo_rw };
    ApuDspEmu *e = apu_dsp_emu_create(&cfg);
    if (!e) {
        fprintf(stderr, "FAIL create\n");
        return 1;
    }
    uint8_t *win = apu_dsp_emu_window(e);
    uint32_t *x = (uint32_t *)(win + APU_DSP_WIN_X);
    uint32_t *p = (uint32_t *)(win + APU_DSP_WIN_P);

    /* The chain, written through the window as a title would.
     * X:$10: circular scratch (buf 0xE) to X:$100, 4 words of 24 bits in 4
     *        bytes, over a 32-byte ring at 0x2000, offset written back.
     * X:$17: X:$100 out to FIFO 0 as 16-bit samples, then EOL. */
    const uint32_t d1[7] = { 0x17, 0x9D0, 4, 0x100, 0, SCRATCH_DATA, 0x1F };
    const uint32_t d2[7] = { 0x4000, 0x402, 4, 0x100, 0, 0, 0 };
    memcpy(&x[0x10], d1, sizeof(d1));
    memcpy(&x[0x17], d2, sizeof(d2));

    CHECK(!apu_dsp_emu_running(e), "running before GPRST");
    CHECK(apu_dsp_emu_run_frame(e, CAP) == 0, "ran in reset");
    *(uint32_t *)(win + APU_DSP_WIN_RST) = 3;
    CHECK(apu_dsp_emu_poll_rst(e), "GPRST edge not seen");
    CHECK(apu_dsp_emu_running(e), "not running after GPRST");
    CHECK(p[2] == 0x08F485, "bootstrap copy: P:2 = %06x", p[2]);

    /* Frame 1. */
    apu_dsp_emu_run_frame(e, CAP);
    ApuDspEmuStats st;
    apu_dsp_emu_get_stats(e, &st);
    CHECK(st.capped == 0, "frame 1 capped at pc %06x", st.pc);
    CHECK(st.dma_blocks == 2, "dma blocks %llu",
          (unsigned long long)st.dma_blocks);
    CHECK(st.dma_rejected == 0, "dma rejected");
    for (uint32_t i = 0; i < 4; i++)
        CHECK(x[0x100 + i] == (data[i] & 0xFFFFFF), "X:%x = %06x",
              0x100 + i, x[0x100 + i]);
    CHECK(x[0x14] == 0x10, "writeback offset %06x", x[0x14]);
    CHECK(fifo0_len == 8, "fifo0 got %zu bytes", fifo0_len);
    for (uint32_t i = 0; i < 4 && i * 2 < fifo0_len; i++) {
        uint16_t v;
        memcpy(&v, fifo0 + i * 2, 2);
        CHECK(v == (uint16_t)((data[i] & 0xFFFFFF) >> 8), "fifo0[%u] %04x",
              i, v);
    }
    /* RUNNING twice, then STOPPED on the third control read; EOL raised. */
    CHECK(x[0x200] == 0x10 && x[0x201] == 0x10 && x[0x202] == 0x20,
          "control reads %06x %06x %06x", x[0x200], x[0x201], x[0x202]);
    CHECK(x[0x203] == 0x80, "interrupt status %06x", x[0x203]);
    /* The mixbuf: X:$1400 is X:$C00, X:$1421 is X:$C21. */
    CHECK(x[0xC00] == 0x123456 && x[0xC21] == 0x123456,
          "mixbuf alias %06x %06x", x[0xC00], x[0xC21]);
    CHECK(apu_dsp_emu_read(e, APU_DSP_SPACE_X, APU_DSP_MIXBUF_BASE + 0x21) ==
              0x123456, "mixbuf read through X:$1421");
    /* And the window's GPMIXBUF page has the frame's copy. */
    uint32_t *mixwin = (uint32_t *)(win + APU_DSP_WIN_MIXBUF);
    CHECK(mixwin[0] == 0x123456 && mixwin[0x21] == 0x123456 &&
              mixwin[0x3FF] == x[0xFFF],
          "GPMIXBUF page %06x %06x", mixwin[0], mixwin[0x21]);

    /* Frame 2: the host changes the program through the window, the ring
     * moves on to its second half. */
    p[PROG_IMM] = 0x654321;
    fifo0_len = 0;
    apu_dsp_emu_run_frame(e, CAP);
    apu_dsp_emu_get_stats(e, &st);
    CHECK(st.capped == 0, "frame 2 capped at pc %06x", st.pc);
    CHECK(st.p_host_words == 1, "host P words %llu",
          (unsigned long long)st.p_host_words);
    CHECK(x[0xC00] == 0x654321, "patched program not run: %06x", x[0xC00]);
    CHECK(mixwin[0] == 0x654321, "GPMIXBUF page not refreshed: %06x",
          mixwin[0]);
    for (uint32_t i = 0; i < 4; i++)
        CHECK(x[0x100 + i] == (data[4 + i] & 0xFFFFFF), "ring X:%x = %06x",
              0x100 + i, x[0x100 + i]);
    CHECK(x[0x14] == 0, "ring wrap offset %06x", x[0x14]);

    /* Frame 3: system memory to DSP from an unknown buffer (0xD) is not a
     * transfer the hardware makes. The descriptor is refused, nothing lands
     * in X, the chain ends and the program still reaches its halt. */
    x[0x11] = 0x9A0;
    x[0x100] = 0x777777;
    apu_dsp_emu_run_frame(e, CAP);
    apu_dsp_emu_get_stats(e, &st);
    CHECK(st.capped == 0, "frame 3 capped at pc %06x", st.pc);
    CHECK(st.dma_rejected == 1, "rejected %llu",
          (unsigned long long)st.dma_rejected);
    CHECK(x[0x100] == 0x777777, "refused descriptor wrote X:$100 = %06x",
          x[0x100]);
    CHECK(x[0x203] == 0x80, "EOL after refusal: %06x", x[0x203]);

    /* Frame 4: 8-bit samples (format 0) are refused like an unknown
     * format. */
    x[0x11] = 0x1D0;
    apu_dsp_emu_run_frame(e, CAP);
    apu_dsp_emu_get_stats(e, &st);
    CHECK(st.capped == 0, "frame 4 capped at pc %06x", st.pc);
    CHECK(st.dma_rejected == 2, "format 0: rejected %llu",
          (unsigned long long)st.dma_rejected);

    /* Frame 5: a descriptor linked to itself never reaches EOL. The walk
     * stops after 0x800 descriptors and refuses the chain, and the program
     * still halts. */
    x[0x10] = 0x10;
    x[0x11] = 0x9D0;
    uint64_t blocks = st.dma_blocks;
    uint64_t repeats = st.trace_repeats;
    apu_dsp_emu_run_frame(e, CAP);
    apu_dsp_emu_get_stats(e, &st);
    CHECK(st.capped == 0, "frame 5 capped at pc %06x", st.pc);
    CHECK(st.dma_rejected == 3, "chain loop: rejected %llu",
          (unsigned long long)st.dma_rejected);
    CHECK(st.dma_blocks - blocks == 0x800, "chain loop walked %llu",
          (unsigned long long)(st.dma_blocks - blocks));
    CHECK(x[0x203] == 0x80, "EOL after chain loop: %06x", x[0x203]);
    /* The 0x800 walks of one descriptor, and the refused walk after them,
     * take one trace entry, so the register writes before them stay in the
     * ring for the reject log. */
    CHECK(st.trace_repeats - repeats == 0x800, "trace folded %llu repeats",
          (unsigned long long)(st.trace_repeats - repeats));

    /* GPRST cleared: held in reset, frames do nothing. */
    *(uint32_t *)(win + APU_DSP_WIN_RST) = 0;
    CHECK(apu_dsp_emu_poll_rst(e), "GPRST clear not seen");
    CHECK(!apu_dsp_emu_running(e), "running in reset");
    CHECK(apu_dsp_emu_run_frame(e, CAP) == 0, "ran in reset");

    apu_dsp_emu_destroy(e);

    /* GPRST already 3 in the title's window when the device is created:
     * the first poll still boots it. */
    static uint8_t pre[APU_DSP_WINDOW_BYTES];
    *(uint32_t *)(pre + APU_DSP_WIN_RST) = 3;
    cfg.window = pre;
    e = apu_dsp_emu_create(&cfg);
    CHECK(e && apu_dsp_emu_poll_rst(e), "GPRST set before create not seen");
    CHECK(e && apu_dsp_emu_running(e), "not running after early GPRST");
    if (e)
        apu_dsp_emu_destroy(e);

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("apu_dsp_emu: ok\n");
    return 0;
}
