/**
 * pb_draw_arrays: an NV097_DRAW_ARRAYS run (start, count) expands into the
 * open batch's indices start..start+count-1, as 16-bit values, up to
 * NV_MAX_INDICES. The decode trim replaced the per-index loop (store and bound
 * check through s_gpu.idx_count for every index) with one clamp and a fill;
 * this holds the fill to what the loop stored.
 *
 * A reference model runs the old loop beside the executor, and a recording
 * backend (no merging) takes each batch's index list at END:
 *
 *   fill       runs of 256 up to 100 slots short of the cap, then one that
 *              crosses it and one more after it: the batch drawn holds
 *              exactly the model's NV_MAX_INDICES indices, and counts as
 *              one overflowed batch (note_idx_overflow fired)
 *   exact      runs that end exactly on the cap: every index kept, no
 *              overflow counted
 *   wrap       a run past 0xFFFF stores its indices truncated to 16 bits
 *              and adds one to the over-0xFFFF count per run, not per
 *              index; a run ending on 0xFFFF adds nothing
 *   outside    a run with no batch open stores nothing and counts nothing
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_idx_stats(uint32_t *max, uint32_t *overflowed, uint32_t *over_ffff);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define BEGIN_END   0x17FCu
#define DRAW_ARRAYS 0x1810u
#define BLEND       0x0304u

static uint16_t s_got[NV_MAX_INDICES];
static uint32_t s_got_n;
static int s_draws;
static int s_fail;

static void rec_on_draw(const struct nv2a_pb_draw *d)
{
    s_got_n = d->idx_count <= NV_MAX_INDICES ? d->idx_count : NV_MAX_INDICES;
    memcpy(s_got, d->idx, s_got_n * sizeof *d->idx);
    s_draws++;
}

static const struct nv2a_pb_backend s_rec = {
    NULL, NULL, rec_on_draw, NULL, NULL, NULL, NULL, NULL,
    0,                  /* writes_back */
    NULL,               /* sync_guest */
    0,                  /* merge_draws: each batch drawn as it came */
};

/* The loop DRAW_ARRAYS ran before the trim, on the model's own buffer. */
static uint16_t s_ref[NV_MAX_INDICES];
static uint32_t s_ref_n, s_ref_over_ffff;
static int s_ref_cut;

static void run(uint32_t start, uint32_t count)
{
    uint32_t i;

    M(DRAW_ARRAYS, ((count - 1u) << 24) | start);
    if (start + count - 1u > 0xFFFFu)
        s_ref_over_ffff++;
    for (i = 0; i < count && s_ref_n < NV_MAX_INDICES; i++)
        s_ref[s_ref_n++] = (uint16_t)(start + i);
    if (i < count)
        s_ref_cut = 1;
}

static void begin(void)
{
    M(BEGIN_END, NV_PRIM_POINTS);
    s_ref_n = 0;
    s_ref_cut = 0;
}

static void check(const char *what, int ok)
{
    fprintf(stderr, "[pb_draw_arrays] %-60s %s\n", what, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static int drawn_as_model(void)
{
    return s_draws == 1 && s_got_n == s_ref_n
        && !memcmp(s_got, s_ref, s_ref_n * sizeof *s_ref);
}

static void build_xbe(uint8_t *xbe, size_t n)
{
    uint32_t v;
    memset(xbe, 0, n);
    memcpy(xbe, "XBEH", 4);
    v = 0x00010000u; memcpy(xbe + 0x104, &v, 4);   /* base address */
    v = (uint32_t)n; memcpy(xbe + 0x108, &v, 4);    /* header size */
    v = 0;           memcpy(xbe + 0x11C, &v, 4);    /* no sections */
    v = 0x00010200u; memcpy(xbe + 0x120, &v, 4);    /* section headers */
    v = 0x00010800u ^ 0x5B6D40B6u; memcpy(xbe + 0x158, &v, 4); /* empty thunks */
}

int main(void)
{
    static uint8_t xbe[0x1000];
    static const uint16_t wrap[] = {
        0xFFF0, 0xFFF1, 0xFFF2, 0xFFF3, 0xFFF4, 0xFFF5, 0xFFF6, 0xFFF7,
        0xFFF8, 0xFFF9, 0xFFFA, 0xFFFB, 0xFFFC, 0xFFFD, 0xFFFE, 0xFFFF,
        0x0000, 0x0001, 0x0002, 0x0003, 0x0004, 0x0005, 0x0006, 0x0007,
        0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x000F,
        0x0000, 0x0001};
    uint32_t max, over, over_ffff, over0, ffff0, k;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_draw_arrays] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(&s_rec);
    M(BLEND, 0);                            /* first method: the executor's setup */

    /* fill: starts that step through the 16-bit range (some past 0xFFFF,
     * which truncate) so a fill that indexed wrong would not match. */
    nv2a_pb_exec_idx_stats(NULL, &over0, &ffff0);
    s_ref_over_ffff = 0;
    s_draws = 0;
    begin();
    for (k = 0; s_ref_n + 256u <= NV_MAX_INDICES - 100u; k++)
        run((k * 0x1F3u) & 0x1FFFFu, 256);
    run(0x1234, 256);                       /* crosses the cap: 100 kept */
    run(0x4000, 17);                        /* after it: nothing kept */
    check("fill: the model ran into the cap", s_ref_n == NV_MAX_INDICES && s_ref_cut);
    M(BEGIN_END, 0);
    nv2a_pb_exec_idx_stats(&max, &over, &over_ffff);
    check("fill: drawn batch matches the per-index loop, index for index",
          drawn_as_model());
    check("fill: the cut batch counts as one overflowed batch", over - over0 == 1);
    check("fill: largest batch drawn is NV_MAX_INDICES", max == NV_MAX_INDICES);
    check("fill: over-0xFFFF count as the loop's", over_ffff - ffff0 == s_ref_over_ffff
          && s_ref_over_ffff > 0);

    /* exact: 0x800 runs of 256 fill the 0x80000 slots with nothing over. */
    nv2a_pb_exec_idx_stats(NULL, &over0, &ffff0);
    s_ref_over_ffff = 0;
    s_draws = 0;
    begin();
    for (k = 0; k < NV_MAX_INDICES / 256u; k++)
        run((k * 0x100u) & 0xFFFFu, 256);
    M(BEGIN_END, 0);
    nv2a_pb_exec_idx_stats(NULL, &over, &over_ffff);
    check("exact: a batch ending on the cap keeps every index",
          s_ref_n == NV_MAX_INDICES && !s_ref_cut && drawn_as_model());
    check("exact: and is not counted as overflowed", over == over0);
    check("exact: no run past 0xFFFF", over_ffff == ffff0);

    /* wrap: 0xFFF0+32 crosses 0xFFFF, 0xFFE0+32 ends on it, 0x10000+2 starts
     * past it. */
    nv2a_pb_exec_idx_stats(NULL, NULL, &ffff0);
    s_ref_over_ffff = 0;
    s_draws = 0;
    begin();
    run(0xFFF0, 32);
    run(0x10000, 2);
    M(BEGIN_END, 0);
    nv2a_pb_exec_idx_stats(NULL, NULL, &over_ffff);
    check("wrap: indices past 0xFFFF truncated to 16 bits",
          drawn_as_model() && s_got_n == 34 && !memcmp(s_got, wrap, sizeof wrap));
    check("wrap: one count per run past 0xFFFF, not per index",
          over_ffff - ffff0 == 2 && s_ref_over_ffff == 2);
    nv2a_pb_exec_idx_stats(NULL, NULL, &ffff0);
    s_draws = 0;
    begin();
    run(0xFFE0, 32);
    M(BEGIN_END, 0);
    nv2a_pb_exec_idx_stats(NULL, NULL, &over_ffff);
    check("wrap: a run ending on 0xFFFF is not counted",
          drawn_as_model() && over_ffff == ffff0);

    /* outside: no batch open. */
    nv2a_pb_exec_idx_stats(NULL, NULL, &ffff0);
    s_draws = 0;
    M(DRAW_ARRAYS, (31u << 24) | 0xFFF0u);
    M(BLEND, 0);
    nv2a_pb_exec_idx_stats(NULL, NULL, &over_ffff);
    check("outside: a run with no batch open draws and counts nothing",
          s_draws == 0 && over_ffff == ffff0);

    fprintf(stderr, "[pb_draw_arrays] %s\n", s_fail ? "FAIL" : "PASS");
    return s_fail ? 1 : 0;
}
