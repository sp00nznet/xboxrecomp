/**
 * pb_draw_merge: back-to-back batches of one triangle primitive, with no
 * method between them, reach a GPU backend as one triangle list
 * (nv2a_merge). BLiNX 1 draws its stage 2 snow as thousands of 3-
 * and 4-vertex strips a flip, and a backend draw call each was the frame.
 *
 * A recording backend takes the draws; the stream is hand-written:
 *
 *   chain      three strips, then a state method: one TRIANGLES draw with
 *              each strip's triangles in order, odd ones wound as the
 *              strip had them; the counters, and the backend's
 *              batches (the flip log golden anchors read), still see three
 *   alone      one strip, then a state method: drawn as it came
 *   prim       a strip then quads: two draws, each as it came
 *   span       two strips far apart in the arrays: two draws
 *   inside     a chain ended by a method inside the next batch: the chain
 *              is drawn first and that batch keeps its own indices
 *   walk end   a chain still held when the walk ends is drawn then
 *   self       strips that sample the colour surface they draw into are
 *              drawn one by one: each sees what the one before it wrote
 *   cap        a batch that grows past the batch cap while a chain is
 *              held ends the chain, and is drawn whole on its own
 *   aspect     with display.aspect set (Hor+ places each batch from its
 *              own vertices) only what the backend's merge_placed takes
 *              is merged: nothing without the hook or when it says no,
 *              the strips as stock when it says yes, which it is asked
 *              with the batch's program flag, 1 for a program batch;
 *              a no mid-chain draws the chain held so far and leaves
 *              that batch alone
 *   placed     nv2a_merge_placed takes only a settled 3D program batch
 *              with the depth test on that samples no render target
 *   snap       with render.snap_2d on above scale 1 (it classes each
 *              batch from its own vertices) only what merge_placed takes
 *              is merged, as under Hor+; at scale 1, where the snap is
 *              off, batches merge as stock
 *   faces      a cull face or front face change between batches (FRONT or
 *              BACK, CW or CCW) keeps the chain: a batch that culls the
 *              other winding joins wound the other way (first two vertices
 *              of each triangle swapped), one that culls the same as is,
 *              and the chain draws with its first batch's faces, the
 *              stream's restored after; FRONT_AND_BACK, a face change
 *              inside a batch and a cull enable change still end it.
 * TRIANGLES, QUADS and FAN batches flip per
 *              triangle too; a batch past the span after a face change
 *              draws the chain under its faces and starts a new one under
 *              the new faces; the walk's end and a transform constant run
 *              draw a held flipped chain and restore the stream's faces
 *   unchanged  a cull enable or vertex array offset / format write that
 *              leaves the state as it is keeps the chain; one that changes
 *              it ends the chain
 *   narrow     with pb_merge=narrow (the second ctest, argument "narrow")
 *              only batches with nothing between them merge
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "nv2a_pb_state.h"
#include "xbox_memory_layout.h"
#include "nv2a_backend_common.h"

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param);
void nv2a_pb_exec_walk_end(void);
uint32_t nv2a_pb_exec_run(uint32_t subch, uint32_t method, int noninc,
                          const uint32_t *p, uint32_t n);

/* The kernel library expects these from the title's generated code. */
typedef void (*recomp_func_t)(void);
recomp_func_t recomp_lookup(unsigned int va) { (void)va; return 0; }
recomp_func_t recomp_lookup_manual(unsigned int va) { (void)va; return 0; }

#define M(method, param) nv2a_pb_exec_method(0, (method), (param))

#define BEGIN_END   0x17FCu
#define DRAW_ARRAYS 0x1810u
#define BLEND       0x0304u
#define DATA4F_4X   0x1A40u     /* SET_VERTEX_DATA4F(4).x: not a vertex */
#define COLOR_OFF   0x0210u     /* SET_SURFACE_COLOR_OFFSET */
#define TEX0_OFF    0x1B00u     /* SET_TEXTURE_OFFSET(0) */
#define TEX0_CTL0   0x1B0Cu     /* SET_TEXTURE_CONTROL0(0), bit 30 enables */
#define XFORM_MODE  0x1E94u     /* SET_TRANSFORM_EXECUTION_MODE, 2: PROGRAM */
#define CULL_ENABLE 0x0308u
#define CULL_FACE   0x039Cu     /* 0x404 FRONT, 0x405 BACK, 0x408 both */
#define FRONT_FACE  0x03A0u     /* 0x900 CW, 0x901 CCW */
#define VA_OFF0     0x1720u     /* SET_VERTEX_DATA_ARRAY_OFFSET(0) */
#define VA_FMT0     0x1760u     /* SET_VERTEX_DATA_ARRAY_FORMAT(0) */
#define XFORM_CONST 0x0B80u     /* SET_TRANSFORM_CONSTANT(0) */

#define MAX_DRAWS 16
static struct {
    uint32_t prim, n, batches;
    uint32_t cull, front;       /* the faces the executor drew with */
    uint16_t idx[64];
} s_draw[MAX_DRAWS];
static int s_draws;
static int s_fail;

static void rec_on_draw(const struct nv2a_pb_draw *d)
{
    if (s_draws < MAX_DRAWS) {
        s_draw[s_draws].prim = d->prim;
        s_draw[s_draws].n = d->idx_count;
        s_draw[s_draws].batches = d->batches;
        s_draw[s_draws].cull = nv2a_pb_vsh_state()->cull_face;
        s_draw[s_draws].front = nv2a_pb_vsh_state()->front_face;
        memcpy(s_draw[s_draws].idx, d->idx,
               (d->idx_count < 64 ? d->idx_count : 64) * sizeof *d->idx);
    }
    s_draws++;
}

static const struct nv2a_pb_backend s_rec = {
    NULL, NULL, rec_on_draw, NULL, NULL, NULL, NULL, NULL,
    0,                  /* writes_back */
    NULL,               /* sync_guest */
    1,                  /* merge_draws */
};

/* The same backend with a merge_placed hook that answers s_placed (or,
 * when s_placed_seq is set, its call'th character, '1' for yes) and
 * records what it was asked. */
static int s_placed, s_placed_calls, s_placed_program = -1;
static const char *s_placed_seq;
static int rec_merge_placed(int program)
{
    s_placed_calls++;
    s_placed_program = program;
    if (s_placed_seq)
        return s_placed_seq[s_placed_calls - 1] == '1';
    return s_placed;
}
static const struct nv2a_pb_backend s_rec_placed = {
    NULL, NULL, rec_on_draw, NULL, NULL, NULL, NULL, NULL,
    0,                  /* writes_back */
    NULL,               /* sync_guest */
    1,                  /* merge_draws */
    rec_merge_placed,
};

static void batch(uint32_t prim, uint32_t start, uint32_t count)
{
    M(BEGIN_END, prim);
    M(DRAW_ARRAYS, ((count - 1u) << 24) | start);
    M(BEGIN_END, 0);
}

static void check(const char *what, int ok)
{
    fprintf(stderr, "[pb_draw_merge] %-60s %s\n", what, ok ? "PASS" : "FAIL");
    s_fail += !ok;
}

static int draw_is(int k, uint32_t prim, const uint16_t *idx, uint32_t n)
{
    return k < s_draws && s_draw[k].prim == prim && s_draw[k].n == n
        && !memcmp(s_draw[k].idx, idx, n * sizeof *idx);
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

/* The faces and unchanged-write cases. narrow: pb_merge=narrow,
 * where each of them draws its batches apart. */
static void faces(int narrow)
{
    /* Strips of 4, 3 and 4 from 0, 4 and 7 under BACK, FRONT, BACK: the
     * middle one culls the other winding, so it joins as (5 4 6). */
    static const uint16_t flipped[] = {0, 1, 2, 2, 1, 3, 5, 4, 6, 7, 8, 9, 9, 8, 10};
    static const uint16_t chain[] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 7, 8, 9, 9, 8, 10};
    static const uint16_t strip4[] = {0, 1, 2, 3};
    static const uint16_t pair[] = {0, 1, 2, 2, 1, 3, 4, 5, 6};
    static const uint16_t pairflip[] = {0, 1, 2, 2, 1, 3, 5, 4, 6};
    static const uint16_t tris_flip[] = {0, 1, 2, 3, 4, 5, 7, 6, 8};
    static const uint16_t fan_flip[] = {0, 1, 2, 0, 2, 3, 5, 4, 6, 6, 4, 7};
    static const uint16_t far_flip[] = {0x2000, 0x2001, 0x2002, 0x2004, 0x2003, 0x2005,
                                        0x2004, 0x2005, 0x2006};
    const char *tag = narrow ? "narrow: " : "";
    char what[96];

#define CHECK(text, ok) (snprintf(what, sizeof what, "%s%s", tag, (text)), check(what, (ok)))
    M(CULL_ENABLE, 1);
    M(FRONT_FACE, 0x901);
    M(CULL_FACE, 0x405);
    M(VA_OFF0, 0x1000);
    M(VA_FMT0, 0x1002);
    M(BLEND, 0);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_ENABLE, 1);                      /* as BLiNX 2 sends it, unchanged */
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(CULL_ENABLE, 1);
    M(CULL_FACE, 0x405);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(CULL_FACE, 0x404);                    /* between batches: still held */
    if (narrow) {
        M(BLEND, 0);
        CHECK("faces: every strip drawn on its own", s_draws == 3
              && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
              && s_draw[0].cull == 0x405 && s_draw[1].cull == 0x404);
        M(CULL_FACE, 0x405);
        s_draws = 0;
        batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
        M(VA_OFF0, 0x1000);
        batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
        M(BLEND, 0);
        CHECK("unchanged: an unchanged write ends the chain", s_draws == 2);
        return;
    }
    CHECK("faces: a cull face flip between strips keeps the chain held", s_draws == 0);
    M(BLEND, 0);
    CHECK("faces: one draw, the FRONT strip wound the other way",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, flipped, 15)
          && s_draw[0].batches == 3);
    CHECK("faces: drawn with the first strip's faces",
          s_draws == 1 && s_draw[0].cull == 0x405 && s_draw[0].front == 0x901);
    CHECK("faces: the stream's cull face is back after the draw",
          nv2a_pb_vsh_state()->cull_face == 0x404);

    /* Front face CW with cull FRONT culls what CCW with BACK does: same
     * parity, joined as is. CW with BACK culls the other: flipped. */
    M(CULL_FACE, 0x405);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(FRONT_FACE, 0x900);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    CHECK("faces: CW+FRONT after CCW+BACK culls alike, joined as is",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, pair, 9)
          && s_draw[0].cull == 0x405 && s_draw[0].front == 0x901);
    M(CULL_FACE, 0x405);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(FRONT_FACE, 0x901);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    CHECK("faces: a front face change alone flips the batch",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, pairflip, 9)
          && s_draw[0].front == 0x900 && nv2a_pb_vsh_state()->front_face == 0x901);

    /* A one-batch chain is drawn as it came, with its own faces, even after
     * a face change that kept it held. */
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_FACE, 0x404);
    M(BLEND, 0);
    CHECK("faces: a lone held strip draws as it came with its faces",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && s_draw[0].cull == 0x405 && nv2a_pb_vsh_state()->cull_face == 0x404);
    M(CULL_FACE, 0x405);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_FACE, 0x408);
    CHECK("faces: FRONT_AND_BACK ends the chain", s_draws == 1);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(CULL_FACE, 0x405);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    CHECK("faces: and from FRONT_AND_BACK a face change ends it too",
          s_draws == 3 && s_draw[1].cull == 0x408);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(BEGIN_END, NV_PRIM_TRIANGLE_STRIP);
    M(CULL_FACE, 0x404);                    /* inside the next batch */
    CHECK("faces: a face change inside a batch ends the chain", s_draws == 1);
    M(DRAW_ARRAYS, (2u << 24) | 4u);
    M(BEGIN_END, 0);
    CHECK("faces: and that batch draws on its own", s_draws == 2
          && s_draw[1].prim == NV_PRIM_TRIANGLE_STRIP && s_draw[1].cull == 0x404);
    M(CULL_FACE, 0x405);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_ENABLE, 0);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(CULL_ENABLE, 1);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    CHECK("faces: a cull enable change ends the chain", s_draws == 3);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(VA_OFF0, 0x1000);
    M(VA_FMT0, 0x1002);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(VA_OFF0, 0x1000);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    CHECK("unchanged: unchanged array offset and format writes keep the chain",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, chain, 15));
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(VA_OFF0, 0x2000);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(VA_FMT0, 0x1012);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    CHECK("unchanged: a new offset or format ends the chain", s_draws == 3);

    /* Lists, fans and quads flip per triangle too: (4 5 6) (4 6 7) of the
     * second batch join as (5 4 6) (6 4 7), a TRIANGLES (6 7 8) as (7 6 8). */
    s_draws = 0;
    batch(NV_PRIM_TRIANGLES, 0, 6);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLES, 6, 3);
    M(BLEND, 0);
    CHECK("faces: a flipped TRIANGLES batch joins wound the other way",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, tris_flip, 9)
          && s_draw[0].cull == 0x405);
    M(CULL_FACE, 0x405);
    s_draws = 0;
    batch(NV_PRIM_QUADS, 0, 4);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_QUADS, 4, 4);
    M(BLEND, 0);
    CHECK("faces: a flipped QUADS batch joins wound the other way",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, fan_flip, 12)
          && s_draw[0].batches == 2 && s_draw[0].cull == 0x405);
    M(CULL_FACE, 0x405);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_FAN, 0, 4);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_FAN, 4, 4);
    M(BLEND, 0);
    CHECK("faces: a flipped FAN batch joins wound the other way",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, fan_flip, 12));
    M(CULL_FACE, 0x405);

    /* A batch past the span after a face change: the chain so far draws
     * under its own faces, and a new one starts under the new faces, so a
     * later BACK batch is the one that flips. */
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_STRIP, 0x2000, 3);
    CHECK("faces: past the span the held chain draws under its faces",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && s_draw[0].cull == 0x405 && nv2a_pb_vsh_state()->cull_face == 0x404);
    M(CULL_FACE, 0x405);
    batch(NV_PRIM_TRIANGLE_STRIP, 0x2003, 4);
    M(BLEND, 0);
    CHECK("faces: and the new chain starts under the new faces",
          s_draws == 2 && draw_is(1, NV_PRIM_TRIANGLES, far_flip, 9)
          && s_draw[1].cull == 0x404 && nv2a_pb_vsh_state()->cull_face == 0x405);

    /* The walk's end and a transform constant run draw a held flipped
     * chain too, and give the stream its faces back. */
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    nv2a_pb_exec_walk_end();
    CHECK("faces: the walk's end draws the chain, faces restored",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, pairflip, 9)
          && s_draw[0].cull == 0x405 && nv2a_pb_vsh_state()->cull_face == 0x404);
    M(CULL_FACE, 0x405);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(CULL_FACE, 0x404);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    {
        static const uint32_t konst[4];
        nv2a_pb_exec_run(0, XFORM_CONST, 0, konst, 4);
    }
    CHECK("faces: a constant run draws the chain, faces restored",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, pairflip, 9)
          && s_draw[0].cull == 0x405 && nv2a_pb_vsh_state()->cull_face == 0x404);
    M(CULL_FACE, 0x405);

    M(VA_OFF0, 0);
    M(VA_FMT0, 0);
    M(CULL_ENABLE, 0);
#undef CHECK
}

int main(int argc, char **argv)
{
    static uint8_t xbe[0x1000];
    /* Strips of 4, 3 and 4 from 0, 4 and 7: (0 1 2) (2 1 3), (4 5 6),
     * (7 8 9) (9 8 10). */
    static const uint16_t chain[] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 7, 8, 9, 9, 8, 10};
    static const uint16_t strip4[] = {0, 1, 2, 3};
    static const uint16_t quads[] = {20, 21, 22, 23};
    static const uint16_t far[] = {0x2000, 0x2001, 0x2002, 0x2003};
    static const uint16_t two[] = {0, 1, 2, 2, 1, 3, 4, 5, 6, 6, 5, 7};
    static const uint16_t after[] = {10, 11, 12, 13};
    uint32_t draws0, verts0;

    setvbuf(stderr, NULL, _IONBF, 0);
    build_xbe(xbe, sizeof xbe);
    if (!xbox_MemoryLayoutInit(xbe, sizeof xbe)) {
        fprintf(stderr, "[pb_draw_merge] FAIL: guest memory init\n");
        return 1;
    }
    nv2a_pb_set_backend(&s_rec);
    M(BLEND, 0);                            /* first method: the executor's setup */
    if (argc > 1 && !strcmp(argv[1], "narrow")) {
        faces(1);
        goto done;
    }

    draws0 = nv2a_pb_gpu_state()->draws;
    verts0 = nv2a_pb_gpu_state()->verts;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    check("chain: held until the next method", s_draws == 0);
    M(BLEND, 0);
    check("chain: one TRIANGLES draw of the three strips' triangles",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, chain, 15));
    check("chain: the draw counter counts three batches",
          nv2a_pb_gpu_state()->draws - draws0 == 3);
    check("chain: the vertex counter counts the strips' 11 indices",
          nv2a_pb_gpu_state()->verts - verts0 == 11);
    check("chain: the backend is told it stands for three batches",
          s_draws == 1 && s_draw[0].batches == 3);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    M(BLEND, 0);
    check("alone: drawn as it came", s_draws == 1
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && s_draw[0].batches == 1);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_QUADS, 20, 4);
    M(BLEND, 0);
    check("prim: a strip then quads, each as it came", s_draws == 2
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && draw_is(1, NV_PRIM_QUADS, quads, 4));

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 0x2000, 4);
    M(BLEND, 0);
    check("span: strips 0x2000 vertices apart stay apart", s_draws == 2
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && draw_is(1, NV_PRIM_TRIANGLE_STRIP, far, 4));

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 4);
    M(BEGIN_END, NV_PRIM_TRIANGLE_STRIP);
    M(DRAW_ARRAYS, (3u << 24) | 10u);
    M(DATA4F_4X, 0);                        /* not an index: ends the chain */
    check("inside: the chain is drawn before the method",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, two, 12));
    M(BEGIN_END, 0);
    check("inside: the open batch keeps its indices", s_draws == 2
          && draw_is(1, NV_PRIM_TRIANGLE_STRIP, after, 4));

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    nv2a_pb_exec_walk_end();
    check("walk end: the held chain is drawn",
          s_draws == 1 && draw_is(0, NV_PRIM_TRIANGLES, chain, 15));

    M(COLOR_OFF, 0x00100000u);
    M(TEX0_OFF, 0x00100000u);
    M(TEX0_CTL0, 1u << 30);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("self: strips sampling their own target are drawn one by one",
          s_draws == 2 && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4)
          && s_draw[0].batches == 1 && s_draw[1].batches == 1);
    M(TEX0_OFF, 0x00200000u);
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("self: another texture merges again", s_draws == 1
          && s_draw[0].prim == NV_PRIM_TRIANGLES && s_draw[0].batches == 2);
    M(TEX0_CTL0, 0);

    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 4);
    M(BEGIN_END, NV_PRIM_TRIANGLE_STRIP);
    M(DRAW_ARRAYS, 199u << 24);   /* 200 indices: 0..199 */
    check("cap: under the cap the chain stays held", s_draws == 0);
    M(DRAW_ARRAYS, (99u << 24) | 200u);                /* 100 more: past 256 */
    check("cap: past the cap the chain is drawn", s_draws == 1
          && draw_is(0, NV_PRIM_TRIANGLES, two, 12));
    M(BEGIN_END, 0);
    check("cap: the long batch is drawn whole on its own", s_draws == 2
          && s_draw[1].prim == NV_PRIM_TRIANGLE_STRIP && s_draw[1].n == 300
          && s_draw[1].idx[0] == 0 && s_draw[1].idx[63] == 63);

    {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.aspect_w = 16;
        o.aspect_h = 9;
        nv2a_host_opts_set(&o);
    }
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("aspect: without merge_placed Hor+ draws every batch alone", s_draws == 2
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4));
    nv2a_pb_set_backend(&s_rec_placed);
    s_placed = 0;
    s_placed_calls = 0;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("aspect: merge_placed 0 draws every batch on its own", s_draws == 2
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4) && s_placed_calls == 2);
    s_placed = 1;
    s_placed_calls = 0;
    s_placed_program = -1;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    check("aspect: merge_placed 1 merges the strips as stock", s_draws == 1
          && draw_is(0, NV_PRIM_TRIANGLES, chain, 15) && s_draw[0].batches == 3);
    check("aspect: merge_placed is asked per batch with its program flag",
          s_placed_calls == 3 && s_placed_program == 0);
    /* Yes, yes, then no: the no flushes the pair held so far as one list
     * and the third batch draws alone with its own indices. */
    {
        static const uint16_t third[] = {7, 8, 9, 10};
        s_placed_seq = "110";
        s_placed_calls = 0;
        s_draws = 0;
        batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
        batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
        batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
        M(BLEND, 0);
        check("aspect: yes, yes, no draws the pair merged, the third alone",
              s_draws == 2 && draw_is(0, NV_PRIM_TRIANGLES, chain, 9)
              && s_draw[0].batches == 2
              && draw_is(1, NV_PRIM_TRIANGLE_STRIP, third, 4)
              && s_placed_calls == 3);
        s_placed_seq = NULL;
    }
    /* A program batch (SET_TRANSFORM_EXECUTION_MODE PROGRAM) asks with 1. */
    M(XFORM_MODE, 2);
    s_placed_calls = 0;
    s_placed_program = -1;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("aspect: a program batch is asked with program 1 and merges",
          s_placed_calls == 2 && s_placed_program == 1 && s_draws == 1
          && s_draw[0].prim == NV_PRIM_TRIANGLES);
    M(XFORM_MODE, 0);
    nv2a_pb_set_backend(&s_rec);

    {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.aspect_w = 0;
        o.aspect_h = 0;
        nv2a_host_opts_set(&o);
    }
    faces(0);

    {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.aspect_w = 0;
        o.aspect_h = 0;
        o.snap_2d = 1;
        o.render_scale = 1;
        nv2a_host_opts_set(&o);
    }
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("snap: at scale 1 the snap is off and the strips merge", s_draws == 1
          && s_draw[0].prim == NV_PRIM_TRIANGLES);
    {
        struct nv2a_host_opts o = *nv2a_host_opts();
        o.render_scale = 2;
        nv2a_host_opts_set(&o);
    }
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("snap: render.snap_2d above scale 1 without merge_placed draws every batch alone", s_draws == 2
          && draw_is(0, NV_PRIM_TRIANGLE_STRIP, strip4, 4));
    nv2a_pb_set_backend(&s_rec_placed);
    s_placed = 0;
    s_placed_calls = 0;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    M(BLEND, 0);
    check("snap: merge_placed 0 draws every batch on its own", s_draws == 2
          && s_placed_calls == 2);
    s_placed = 1;
    s_placed_calls = 0;
    s_draws = 0;
    batch(NV_PRIM_TRIANGLE_STRIP, 0, 4);
    batch(NV_PRIM_TRIANGLE_STRIP, 4, 3);
    batch(NV_PRIM_TRIANGLE_STRIP, 7, 4);
    M(BLEND, 0);
    check("snap: merge_placed 1 merges the strips as stock", s_draws == 1
          && draw_is(0, NV_PRIM_TRIANGLES, chain, 15) && s_placed_calls == 3);
    nv2a_pb_set_backend(&s_rec);

    {
        /* A program whose first 16 batches were all 3D, depth on, no
         * target sampled: placed the same from any vertices. */
        struct nv2a_aspect_gate g3 = { 0, 16, 0 }, g2 = { 1, 40, 0 }, gnew = { -1, 0, 0 },
                                gearly = { 0, 15, 0 };
        check("placed: a settled 3D program batch may merge",
              nv2a_merge_placed(1, 1, &g3, 0));
        check("placed: pass-through, no depth test, or a target read may not",
              !nv2a_merge_placed(0, 1, &g3, 0) && !nv2a_merge_placed(1, 0, &g3, 0)
              && !nv2a_merge_placed(1, 1, &g3, 1));
        check("placed: a program seen 2D, unchecked or not settled may not",
              !nv2a_merge_placed(1, 1, &g2, 0) && !nv2a_merge_placed(1, 1, &gnew, 0)
              && !nv2a_merge_placed(1, 1, &gearly, 0) && !nv2a_merge_placed(1, 1, NULL, 0));
    }

done:
    if (s_fail) {
        fprintf(stderr, "[pb_draw_merge] FAIL: %d check(s)\n", s_fail);
        return 1;
    }
    fprintf(stderr, "[pb_draw_merge] done: all checks pass\n");
    return 0;
}
