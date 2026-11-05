/* The pinned dsp56300 archive links and its JIT runs on this host: a core
 * with X, Y and P buffers runs a NOP loop (P:0x10 jumps back to 0), and a
 * word written through the API reads back. This is what XBOXRECOMP_APU_DSP
 * adds to every link, the Rust std's system libraries included, so a
 * missing one (CoreFoundation, Security; libunwind under llvm-mingw) fails
 * here and not at the first game link. */
#include <dsp56300.h>

#include <stdio.h>
#include <stdlib.h>

#define MEM_WORDS 0x1000u

static int s_checks, s_fail;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main(void)
{
    uint32_t *x = calloc(MEM_WORDS, 4), *y = calloc(MEM_WORDS, 4);
    uint32_t *p = calloc(MEM_WORDS, 4);
    Dsp56300MemoryRegion xr = { .start = 0, .end = MEM_WORDS,
        .kind = DSP56300_REGION_BUFFER, .data = { .buffer = { .base = x } } };
    Dsp56300MemoryRegion yr = { .start = 0, .end = MEM_WORDS,
        .kind = DSP56300_REGION_BUFFER, .data = { .buffer = { .base = y } } };
    Dsp56300MemoryRegion pr = { .start = 0, .end = MEM_WORDS,
        .kind = DSP56300_REGION_BUFFER, .data = { .buffer = { .base = p } } };
    Dsp56300CreateInfo ci = { .memory_map = {
        .x_regions = &xr, .x_count = 1, .y_regions = &yr, .y_count = 1,
        .p_regions = &pr, .p_count = 1 } };
    Dsp56300Jit *dsp;
    Dsp56300State st;

    if (!x || !y || !p)
        return 2;
    p[0x10] = 0x0C0000;                   /* jmp $0 (short absolute) */
    dsp = dsp56300_create(&ci);
    CHECK(dsp != NULL);
    if (!dsp)
        return 1;
    dsp56300_reset(dsp);
    dsp56300_run(dsp, 1000);
    CHECK(dsp56300_cycle_count(dsp) > 0);
    dsp56300_get_state(dsp, &st);
    CHECK(st.pc <= 0x10);

    dsp56300_write_memory(dsp, DSP56300_MEM_SPACE_X, 0x20, 0x123456);
    CHECK(dsp56300_read_memory(dsp, DSP56300_MEM_SPACE_X, 0x20) == 0x123456);
    CHECK(x[0x20] == 0x123456);

    dsp56300_destroy(dsp);
    free(x);
    free(y);
    free(p);
    printf("apu_dsp_link: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
