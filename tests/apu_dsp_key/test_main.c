/* RECOMP_APU_DSP, the APU DSP's run-time key (recomp_env.c, apu_dsp_resolve):
 * empty and 0 keep the stub with its ack and fold untouched; 1 in a build
 * with the DSP and its device forces apu_dsp_ack to 0 and drops
 * apu_mixdown_all, with one line only when the user had set either; a build
 * without the DSP, or with it but without the device yet, ignores it with a
 * line. Unset is 1 in a build with the DSP and its device, unless
 * the game defaults the key to 0, and the stub, silently, in any other
 * build. Its name must not collide with RECOMP_APU_DSP_ACK or the
 * apu_dsp_ack list key.
 *
 * Built four ways by CMakeLists.txt: no XBOXRECOMP_APU_DSP, it alone, it
 * with XBOXRECOMP_APU_DSP_DEVICE, and that with a game default of 0
 * (TEST_GAME_STUB).
 * recomp_env.c is included rather than linked, to reach the held notes. */
#ifdef TEST_GAME_STUB
#define RECOMP_ENV_GAME_DEFAULTS(D) D(APU_DSP, "0")
#endif
#include "recomp_env.c"

#if defined(XBOXRECOMP_APU_DSP) && defined(XBOXRECOMP_APU_DSP_DEVICE)
#define DSP_RUNS 1
#else
#define DSP_RUNS 0
#endif
/* What unset resolves to: the JIT where it can run, unless the game says 0. */
#if DSP_RUNS && !defined(TEST_GAME_STUB)
#define DSP_BY_DEFAULT 1
#else
#define DSP_BY_DEFAULT 0
#endif

static int s_checks, s_fail;

#define CHECK(c) do { s_checks++; if (!(c)) { s_fail++; \
    fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static void set_env(const char *k, const char *v)
{
#ifdef _WIN32
    char buf[512];
    snprintf(buf, sizeof buf, "%s=%s", k, v ? v : "");
    _putenv(buf);
#else
    if (v) setenv(k, v, 1); else unsetenv(k);
#endif
}

static int streq(const char *a, const char *b)
{
    return a && b && !strcmp(a, b);
}

/* Reload from these variables (NULL: unset), held notes cleared first. */
static void load_env(const char *apu_dsp, const char *debug, const char *legacy_ack)
{
    s_notes_len = 0;
    if (s_notes)
        s_notes[0] = 0;
    set_env("RECOMP_TRACE", NULL);
    set_env("RECOMP_APU_DSP", apu_dsp);
    set_env("RECOMP_DEBUG", debug);
    set_env("RECOMP_APU_DSP_ACK", legacy_ack);
    set_env("RECOMP_APU_MIXDOWN_ALL", NULL);
    recomp_env_reload();
}

static int count(const char *text)
{
    const char *p = s_notes;
    int n = 0;
    while (p && (p = strstr(p, text)) != NULL) {
        n++;
        p++;
    }
    return n;
}

/* Lines of the held notes about RECOMP_APU_DSP (not the deprecation line
 * of RECOMP_APU_DSP_ACK), the default's override line included. */
static int apu_dsp_lines(void)
{
    return count("[ENV] RECOMP_APU_DSP=") + count("[ENV] the APU DSP");
}

static int noted(const char *text)
{
    return s_notes && strstr(s_notes, text) != NULL;
}

/* Empty and 0 are the stub, and so is unset where the DSP is not the
 * default: every APU key as it was. */
static void test_stub(void)
{
#if DSP_BY_DEFAULT
    const char *const off[] = { "", "0" };
#else
    const char *const off[] = { NULL, "", "0" };
#endif
    size_t i;

    for (i = 0; i < sizeof off / sizeof off[0]; i++) {
        load_env(off[i], NULL, NULL);
        CHECK(recomp_env(RENV_APU_DSP) == NULL);
        CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
        CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
        CHECK(apu_dsp_lines() == 0);

        load_env(off[i], "apu_dsp_ack=0x80A20810,apu_mixdown_all=2", NULL);
        CHECK(recomp_env(RENV_APU_DSP) == NULL);
        CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0x80A20810"));
        CHECK(streq(recomp_env(RENV_APU_MIXDOWN_ALL), "2"));
        CHECK(apu_dsp_lines() == 0);
    }
}

/* The names differ only by a suffix: neither spelling of apu_dsp_ack turns
 * RECOMP_APU_DSP on, and apu_dsp is no list key (it is a variable). The key
 * is 0, so the DSP's own default does not hide a collision. */
static void test_no_collision(void)
{
    load_env("0", NULL, "0x80A20810");
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0x80A20810"));

    load_env("0", "apu_dsp_ack=0", NULL);
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));

    load_env("0", "apu_dsp=1", NULL);
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(noted("unknown key 'apu_dsp'"));
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
}

/* Neither 0 nor 1: the stub, with a line. */
static void test_bad_value(void)
{
    load_env("yes", NULL, NULL);
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
    CHECK(noted("RECOMP_APU_DSP=yes: not 0 or 1"));
    CHECK(apu_dsp_lines() == 1);
}

#if DSP_RUNS
#if DSP_BY_DEFAULT
/* Unset: the JIT, with the same overrides as an explicit 1, and a line
 * that does not claim the user set RECOMP_APU_DSP=1. */
static void test_dsp_default(void)
{
    load_env(NULL, NULL, NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP), "1"));
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
    CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
    CHECK(apu_dsp_lines() == 0);

    load_env(NULL, "apu_dsp_ack=auto,apu_mixdown_all=2", NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP), "1"));
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
    CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
    CHECK(apu_dsp_lines() == 1);
    CHECK(noted("[ENV] the APU DSP (on by default) overrides apu_dsp_ack"
                " (now 0) and apu_mixdown_all (ignored)"));
    CHECK(!noted("RECOMP_APU_DSP=1"));
}
#else
/* The game's D(APU_DSP, "0"): unset is the stub, silently; the environment
 * still wins. */
static void test_game_stub(void)
{
    load_env(NULL, "apu_mixdown_all=2", NULL);
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
    CHECK(streq(recomp_env(RENV_APU_MIXDOWN_ALL), "2"));
    CHECK(apu_dsp_lines() == 0);

    load_env("1", NULL, NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP), "1"));
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
}
#endif

static void test_dsp_on(void)
{
    /* Nothing else set: the ack goes off, quietly. */
    load_env("1", NULL, NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP), "1"));
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
    CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
    CHECK(apu_dsp_lines() == 0);

    /* Both set: both overridden, in one line. */
    load_env("1", "apu_dsp_ack=auto,apu_mixdown_all=2", NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
    CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
    CHECK(apu_dsp_lines() == 1);
    CHECK(noted("apu_dsp_ack (now 0) and apu_mixdown_all (ignored)"));

    /* One set, through its old variable: still the user's, still one line. */
    load_env("1", NULL, "0x80A20810");
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0"));
    CHECK(apu_dsp_lines() == 1);
    CHECK(noted("overrides apu_dsp_ack (now 0):"));

    load_env("1", "apu_mixdown_all=0", NULL);
    CHECK(recomp_env(RENV_APU_MIXDOWN_ALL) == NULL);
    CHECK(apu_dsp_lines() == 1);
    CHECK(noted("overrides apu_mixdown_all (ignored):"));
}
#else
static void test_stub_kept(void)
{
    /* A build without the DSP, or with it but not its device, keeps the stub,
     * ack and fold included: with the ack off and nothing to answer the
     * doorbell, audio init would wait for ever. */
    load_env("1", "apu_mixdown_all=2", "0x80A20810");
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "0x80A20810"));
    CHECK(streq(recomp_env(RENV_APU_MIXDOWN_ALL), "2"));
#ifdef XBOXRECOMP_APU_DSP
    CHECK(noted("RECOMP_APU_DSP=1: this build has the dsp56300 JIT but not"
                " the APU DSP device (XBOXRECOMP_APU_DSP_DEVICE); the stub"));
#else
    CHECK(noted("RECOMP_APU_DSP=1: this build has no APU DSP"));
#endif
    CHECK(apu_dsp_lines() == 1);

    load_env("1", NULL, NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
    CHECK(apu_dsp_lines() == 1);

    /* Unset here is a build choice, not a mistake: the stub, no line. */
    load_env(NULL, NULL, NULL);
    CHECK(recomp_env(RENV_APU_DSP) == NULL);
    CHECK(streq(recomp_env(RENV_APU_DSP_ACK), "auto"));
    CHECK(apu_dsp_lines() == 0);
}
#endif

int main(void)
{
    test_stub();
    test_no_collision();
    test_bad_value();
#if DSP_RUNS
#if DSP_BY_DEFAULT
    test_dsp_default();
#else
    test_game_stub();
#endif
    test_dsp_on();
#else
    test_stub_kept();
#endif
    load_env(NULL, NULL, NULL);
    printf("apu_dsp_key (%s): %d checks, %d failed\n",
#if defined(TEST_GAME_STUB)
           "DSP and device, game default 0",
#elif defined(XBOXRECOMP_APU_DSP_DEVICE)
           "DSP and device",
#elif defined(XBOXRECOMP_APU_DSP)
           "DSP, no device",
#else
           "no DSP",
#endif
           s_checks, s_fail);
    return s_fail != 0;
}
