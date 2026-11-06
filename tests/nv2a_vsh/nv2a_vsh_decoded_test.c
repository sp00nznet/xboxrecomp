/* The decoded vertex-program runner against the field() loop it replaces.
 *
 * Includes the interpreter itself, so both runners can be called directly:
 * random programs (every MAC and ILU op, every source mux, swizzles, negates,
 * a0-relative constants, every output register) on random inputs must give
 * byte-identical outputs, including after the program, a single instruction
 * or the start slot changes between runs. Then both are timed on one
 * program of typical length; the times are printed, not checked. */
#include "../../src/kernel/nv2a_vsh_interp.c"

#include <time.h>

static uint32_t s_rng = 12345u;

static uint32_t rnd(void)
{
    s_rng ^= s_rng << 13;
    s_rng ^= s_rng >> 17;
    s_rng ^= s_rng << 5;
    return s_rng;
}

static float rndf(void)
{
    return ((float)(rnd() & 0xFFFF) / 65535.0f - 0.5f) * 8.0f;
}

/* One random instruction. Constant writes (out to c[]) are left to
 * s_cxt_write, which stays off, as most titles leave it. */
static void random_ins(uint32_t w[4], int final)
{
    w[0] = 0;
    w[1] = rnd() & 0x0FFFFFFFu;
    w[2] = rnd();
    w[3] = rnd() & ~1u;
    /* Keep a0-relative reads mostly inside c[]: the base index below 96. */
    w[1] = (w[1] & ~(0xFFu << 13)) | ((rnd() % 96u) << 13);
    if (final)
        w[3] |= 1u;
}

static void load_random_program(uint32_t start, uint32_t len)
{
    uint32_t i, w[4];

    for (i = 0; i < len; i++) {
        random_ins(w, i + 1 == len);
        nv2a_vsh_set_instruction(start + i, w);
    }
    nv2a_vsh_set_start_slot(start);
}

static void random_inputs(float in[NV2A_VSH_INPUTS][4])
{
    int i, k;

    for (i = 0; i < NV2A_VSH_INPUTS; i++)
        for (k = 0; k < 4; k++)
            in[i][k] = rndf();
}

static int compare_once(const float in[NV2A_VSH_INPUTS][4], const char *what)
{
    Nv2aVshOutput a, b;
    int ra, rb;

    memset(&a, 0xA5, sizeof a);
    memset(&b, 0xA5, sizeof b);
    ra = vsh_run_fields(in, &a);
    rb = vsh_run_decoded(in, &b);
    if (ra != rb || (ra && memcmp(&a, &b, sizeof a) != 0)) {
        printf("FAIL (%s): fields %d pos %g %g %g %g, decoded %d pos %g %g %g %g\n",
               what, ra, a.pos[0], a.pos[1], a.pos[2], a.pos[3],
               rb, b.pos[0], b.pos[1], b.pos[2], b.pos[3]);
        return 1;
    }
    return 0;
}

static double now_s(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + ts.tv_nsec * 1e-9;
}

int main(void)
{
    float in[NV2A_VSH_INPUTS][4];
    uint32_t p, v, i;
    int fails = 0;

    for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
        float c[4] = {rndf(), rndf(), rndf(), rndf()};
        nv2a_vsh_set_constant(i, c);
    }

    /* 2,000 random programs of 1..40 instructions at random start slots,
     * 16 vertices each. */
    for (p = 0; p < 2000 && fails < 8; p++) {
        uint32_t len = 1 + rnd() % 40;
        uint32_t start = rnd() % (NV2A_VSH_SLOTS - len);

        load_random_program(start, len);
        for (v = 0; v < 16 && fails < 8; v++) {
            random_inputs(in);
            fails += compare_once(in, "random program");
        }
        /* One instruction replaced in place: the decoded copy must follow. */
        {
            uint32_t w[4];
            random_ins(w, len == 1);
            nv2a_vsh_set_instruction(start, w);
            random_inputs(in);
            fails += compare_once(in, "instruction replaced");
        }
        /* The same words, a later start slot. */
        if (len > 2) {
            nv2a_vsh_set_start_slot(start + 1);
            random_inputs(in);
            fails += compare_once(in, "start slot moved");
        }
    }
    /* Uploads through the method path: _PROGRAM_LOAD then _PROGRAM words. */
    {
        uint32_t w[4];
        nv2a_vsh_set_load_slot(10);
        for (i = 0; i < 12; i++) {
            int k;
            random_ins(w, i == 11);
            for (k = 0; k < 4; k++)
                nv2a_vsh_program_word(w[k]);
        }
        nv2a_vsh_set_start_slot(10);
        random_inputs(in);
        fails += compare_once(in, "method upload");
    }
    if (fails) {
        printf("FAIL: %d mismatches\n", fails);
        return 1;
    }

    /* Timing: 2 million vertices each way through one 24-instruction program
     * with the op mix of a D3D vertex shader -- DP4/MAD/MUL/ADD/DP3/MOV on
     * the MAC, one RSQ and one RCC on the ILU, no LIT/EXP/LOG, whose powf
     * and exp2f would hide what is being timed. */
    {
        static const uint32_t mac_ops[6] = {7, 4, 2, 3, 5, 1};
        const uint32_t n = 2000000u;
        Nv2aVshOutput o;
        double t0, tf, td;
        volatile float sink = 0;
        uint32_t w[4];

        for (i = 0; i < 24; i++) {
            uint32_t ilu = i == 9 ? 4u : (i == 21 ? 3u : 0u);
            random_ins(w, i == 23);
            w[1] = (w[1] & ~(0x7Fu << 21)) | (mac_ops[rnd() % 6] << 21) | (ilu << 25);
            w[3] &= ~(1u << 1);                       /* no a0-relative reads */
            nv2a_vsh_set_instruction(i, w);
        }
        nv2a_vsh_set_start_slot(0);
        random_inputs(in);
        t0 = now_s();
        for (i = 0; i < n; i++) {
            in[0][0] = (float)(i & 255);
            vsh_run_fields(in, &o);
            sink += o.pos[0];
        }
        tf = now_s() - t0;
        t0 = now_s();
        for (i = 0; i < n; i++) {
            in[0][0] = (float)(i & 255);
            vsh_run_decoded(in, &o);
            sink += o.pos[0];
        }
        td = now_s() - t0;
        printf("24-instruction program: field() loop %.1f ns/vertex, decoded %.1f ns/vertex (%.2fx)\n",
               tf * 1e9 / n, td * 1e9 / n, tf / td);
        (void)sink;
    }
    puts("ok");
    return 0;
}
