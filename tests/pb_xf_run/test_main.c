/**
 * pb_xf_run: the transform program and constant uploads as one run.
 *
 * nv2a_pb_scan hands the rest of a method header's words to
 * nv2a_pb_exec_run, which executes the SET_TRANSFORM_PROGRAM and
 * SET_TRANSFORM_CONSTANT uploads in one call instead of one
 * nv2a_pb_exec_method each. It must leave exactly the state the
 * word-by-word path leaves: the same prog[] and c[] contents, the same load
 * pointers, the same counters and the same c[] write generation. Each case
 * is fed twice from zeroed slots, once word by word and once as a
 * pushbuffer through the walk, and the results compared: an incrementing
 * header that runs out of the program window into the constant window, a
 * non-incrementing one, loads that run past the last slot, a walk whose PUT
 * falls inside a run, a header that starts below the window and runs into
 * it, and a one-word header. The run also does the per-method prologue once:
 * a held merge chain drawn before the first store, the open batch no longer
 * pure, the fence pairing dropped and the methods counted; two cases check
 * those against the word-by-word path with a stub backend that merges and
 * counts its draws. No GPU, so it runs on every POSIX host.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
uint32_t nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc,
                          const uint32_t *params, uint32_t n);
void nv2a_pb_scan(uint32_t start_va, uint32_t end_va);
void nv2a_pb_exec_prologue_state(uint32_t *methods, int *batch_pure,
                                 int *fence_ref_next, uint32_t *held);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))
#define PROG_LOAD  0x1E9Cu    /* SET_TRANSFORM_PROGRAM_LOAD */
#define CONST_LOAD 0x1EA4u    /* SET_TRANSFORM_CONSTANT_LOAD */
#define BEGIN_END  0x17FCu    /* SET_BEGIN_END */
#define AE16       0x1800u    /* ARRAY_ELEMENT16: two indices a word */
#define AE32       0x1808u    /* ARRAY_ELEMENT32 */
#define FENCE_REF  0x0310u    /* on a subchannel other than Kelvin's */
/* 2 MB into the contiguous window (kernel.h XBOX_CONTIG_BASE 0x80000000). */
#define PB_VA      0x80200000u

static int s_fail;

static void expect(const char *what, int ok)
{
    fprintf(stderr, "[pb_xf_run] %-58s %s\n", what, ok ? "PASS" : "FAIL");
    s_fail += !ok;
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

/* What one pass leaves behind. */
struct snap {
    uint32_t prog[NV2A_VSH_SLOTS][4];
    float c[NV2A_VSH_CONSTANTS][4];
    uint32_t load, cload, prog_dwords, const_dwords, cgen;
};

/* Every slot back to zero, word by word, and the counters' base taken. */
static void zero_slots(struct snap *base)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    uint32_t i;
    M(PROG_LOAD, 0);
    for (i = 0; i < NV2A_VSH_SLOTS * 4; i++)
        M(0x0B00 + 4 * (i & 3), 0);
    M(CONST_LOAD, 0);
    for (i = 0; i < NV2A_VSH_CONSTANTS * 4; i++)
        M(0x0B80 + 4 * (i & 3), 0);
    base->prog_dwords = v->prog_dwords;
    base->const_dwords = v->const_dwords;
    base->cgen = nv2a_pb_vsh_consts_gen();
}

static void take(struct snap *s, const struct snap *base)
{
    const struct nv2a_pb_vsh *v = nv2a_pb_vsh_state();
    memcpy(s->prog, v->prog, sizeof s->prog);
    memcpy(s->c, v->c, sizeof s->c);
    s->load = v->load;
    s->cload = v->cload;
    s->prog_dwords = v->prog_dwords - base->prog_dwords;
    s->const_dwords = v->const_dwords - base->const_dwords;
    s->cgen = nv2a_pb_vsh_consts_gen() - base->cgen;
}

static uint32_t s_rng = 0x1234567u;
static uint32_t rnd(void)
{
    s_rng = s_rng * 1103515245u + 12345u;
    return (s_rng >> 8) | (s_rng << 24);
}

/* One case: a header of n words at `method` (non-incrementing if noninc),
 * after the two load pointers are set. `cut` > 0 puts PUT that many words
 * into the parameters, so the run spans two walks. */
static void one_case(const char *name, uint32_t prog_at, uint32_t const_at,
                     uint32_t method, int noninc, uint32_t n, uint32_t cut)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t *pb = (uint32_t *)(mem + PB_VA);
    uint32_t data[512], i, end;
    struct snap base, ref, run;
    char what[96];

    for (i = 0; i < n; i++) {
        data[i] = rnd() & 0x3FFFFFFFu;    /* finite floats in the c[] case */
        if (data[i] == 0)
            data[i] = 1;
    }

    zero_slots(&base);
    M(PROG_LOAD, prog_at);
    M(CONST_LOAD, const_at);
    for (i = 0; i < n; i++)
        M(method + (noninc ? 0 : 4 * i), data[i]);
    take(&ref, &base);

    zero_slots(&base);
    M(PROG_LOAD, prog_at);
    M(CONST_LOAD, const_at);
    pb[0] = (noninc ? 0x40000000u : 0) | (n << 18) | method;
    memcpy(&pb[1], data, n * 4);
    end = PB_VA + 4 * (1 + n);
    if (cut) {
        nv2a_pb_scan(PB_VA, PB_VA + 4 * (1 + cut));
        nv2a_pb_scan(PB_VA + 4 * (1 + cut), end);
    } else {
        nv2a_pb_scan(PB_VA, end);
    }
    take(&run, &base);

    snprintf(what, sizeof what, "%s: prog[] matches", name);
    expect(what, !memcmp(ref.prog, run.prog, sizeof ref.prog));
    snprintf(what, sizeof what, "%s: c[] matches", name);
    expect(what, !memcmp(ref.c, run.c, sizeof ref.c));
    snprintf(what, sizeof what, "%s: load pointers match", name);
    expect(what, ref.load == run.load && ref.cload == run.cload);
    snprintf(what, sizeof what, "%s: dword counters match", name);
    expect(what, ref.prog_dwords == run.prog_dwords
                 && ref.const_dwords == run.const_dwords);
    snprintf(what, sizeof what, "%s: c[] generation moves alike", name);
    expect(what, ref.cgen == run.cgen);
}

/* A backend that takes merged draws and notes, at each draw, the constant
 * the prologue cases store, so the order of the flush and the store shows. */
#define PRO_CONST 12u
static uint32_t s_draws;
static float s_c_at_draw;

static void stub_draw(const struct nv2a_pb_draw *d)
{
    (void)d;
    s_draws++;
    s_c_at_draw = nv2a_pb_vsh_state()->c[PRO_CONST][0];
}

static const struct nv2a_pb_backend s_stub = {
    .on_draw = stub_draw,
    .merge_draws = 1,
};

struct pro {
    uint32_t held, draws_at_run, draws, methods;
    int pure, fence_before, fence;
    float c_at_draw;
};

/* Eight constant words, word by word (walk 0) or as one header through the
 * walk, after `fence` a fence reference on another subchannel, else two
 * merged one-triangle batches held and a third begun with indices only. */
static void prologue_case(int walk, int fence, struct pro *o)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t *pb = (uint32_t *)(mem + PB_VA);
    uint32_t data[8], m0, i;
    struct snap base;
    int k;

    for (i = 0; i < 8; i++)
        data[i] = 0x3F800000u + i;            /* 1.0f and up */
    zero_slots(&base);
    M(CONST_LOAD, PRO_CONST);
    s_draws = 0;
    s_c_at_draw = -1.0f;
    if (fence) {
        nv2a_pb_exec_method(1, FENCE_REF, 7);
    } else {
        for (k = 0; k < 2; k++) {
            M(BEGIN_END, 5);                  /* triangles */
            M(AE16, 0x00010000u);
            M(AE32, 2);
            M(BEGIN_END, 0);
        }
        M(BEGIN_END, 5);
        M(AE16, 0x00010000u);
    }
    nv2a_pb_exec_prologue_state(&m0, &o->pure, &o->fence_before, &o->held);
    if (walk) {
        pb[0] = (8u << 18) | 0x0B80u;
        memcpy(&pb[1], data, sizeof data);
        nv2a_pb_scan(PB_VA, PB_VA + 4 * 9);
    } else {
        for (i = 0; i < 8; i++)
            M(0x0B80 + 4 * (i & 3), data[i]);
    }
    o->draws_at_run = s_draws;
    o->c_at_draw = s_c_at_draw;
    nv2a_pb_exec_prologue_state(&o->methods, &o->pure, &o->fence, &o->held);
    o->methods -= m0;
    if (!fence) {
        M(AE32, 2);
        M(BEGIN_END, 0);                      /* no longer pure: drawn alone */
    }
    o->draws = s_draws;
}

static void prologue_cases(void)
{
    struct pro ref, run;

    nv2a_pb_set_backend(&s_stub);
    prologue_case(0, 0, &ref);
    expect("prologue: the reference holds two batches, then flushes",
           ref.draws_at_run == 1 && ref.c_at_draw == 0.0f && ref.draws == 2);
    prologue_case(1, 0, &run);
    expect("prologue: held chain drawn before the first store",
           run.draws_at_run == ref.draws_at_run && run.c_at_draw == ref.c_at_draw);
    expect("prologue: nothing held after the run", run.held == 0 && ref.held == 0);
    expect("prologue: open batch no longer pure", run.pure == ref.pure && !run.pure);
    expect("prologue: the batch after the run is drawn alone", run.draws == ref.draws);
    expect("prologue: methods counted alike", run.methods == ref.methods
                                              && run.methods == 8);

    prologue_case(0, 1, &ref);
    prologue_case(1, 1, &run);
    expect("prologue: fence pairing dropped by the run",
           ref.fence_before && run.fence_before && !ref.fence && !run.fence);
    expect("prologue: methods counted alike after a fence",
           run.methods == ref.methods && run.methods == 8);
    nv2a_pb_set_backend(NULL);
}

int main(void)
{
    static uint8_t xbe[0x1000];
    uint32_t p[64] = { 0 };

    setvbuf(stderr, NULL, _IONBF, 0);
    setenv("RECOMP_PB_EXEC", "1", 1);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_xf_run] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(NULL);

    expect("before the first method: left to exec_method",
           nv2a_pb_exec_run(0, 0x0B00, 0, p, 8) == 0);
    M(0x0304, 1);                          /* SET_BLEND_ENABLE: first call */
    expect("program run: taken", nv2a_pb_exec_run(0, 0x0B00, 0, p, 8) == 8);
    expect("incrementing run: stops at the window's end",
           nv2a_pb_exec_run(0, 0x0B70, 0, p, 8) == 4);
    expect("non-incrementing run: all of it",
           nv2a_pb_exec_run(0, 0x0B7C, 1, p, 40) == 40);
    expect("one word: left to exec_method",
           nv2a_pb_exec_run(0, 0x0B80, 0, p, 1) == 0);
    expect("another subchannel: left to exec_method",
           nv2a_pb_exec_run(1, 0x0B80, 0, p, 8) == 0);
    expect("another method: left to exec_method",
           nv2a_pb_exec_run(0, 0x0A20, 0, p, 4) == 0
           && nv2a_pb_exec_run(0, 0x0C00, 0, p, 4) == 0);

    one_case("program into constants", 10, 20, 0x0B00, 0, 40, 0);
    one_case("constants, mid-row start", 0, 33, 0x0B84, 0, 31, 0);
    one_case("non-incrementing constant", 0, 7, 0x0B88, 1, 13, 0);
    one_case("non-incrementing program", 3, 0, 0x0B7C, 1, 29, 0);
    one_case("program past the last slot", NV2A_VSH_SLOTS - 3, 0, 0x0B00, 0, 32, 0);
    one_case("constants past the last slot", 0, NV2A_VSH_CONSTANTS - 2, 0x0B80, 0, 32, 0);
    one_case("non-incrementing past the last slot", 0, NV2A_VSH_CONSTANTS - 1,
             0x0B8C, 1, 300, 0);
    one_case("PUT inside the run", 40, 50, 0x0B10, 0, 28, 9);
    one_case("one-word header", 5, 6, 0x0B8C, 0, 1, 0);
    one_case("into the window from below", 7, 0, 0x0AF0, 0, 12, 0);
    prologue_cases();

    if (s_fail) {
        fprintf(stderr, "[pb_xf_run] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_xf_run] done: all checks pass\n");
    return 0;
}
