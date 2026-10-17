/* RECOMP_DEBUG list parsing: a key whose value has commas in it keeps the
 * fragments after it (vsh_const_prog=AA,BB), every other key ends its value
 * at the comma, and every name in s_comma_keys is a real key. A typo there
 * silently turns a list value back into one entry plus unknown-key warnings.
 *
 * recomp_env.c is included rather than linked, to reach s_comma_keys,
 * s_keys and the held notes. */
#include "recomp_env.c"

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

/* Parse `debug` as RECOMP_DEBUG from scratch, held notes cleared first. */
static void load_debug(const char *debug)
{
    s_notes_len = 0;
    if (s_notes)
        s_notes[0] = 0;
    set_env("RECOMP_TRACE", NULL);
    set_env("RECOMP_DEBUG", debug);
    recomp_env_reload();
}

static int noted(const char *text)
{
    return s_notes && strstr(s_notes, text) != NULL;
}

static void test_comma_values(void)
{
    load_debug("vsh_const_prog=AA,BB,pb_fast=0");
    CHECK(streq(recomp_env(RENV_VSH_CONST_PROG), "AA,BB"));
    CHECK(streq(recomp_env(RENV_PB_FAST), "0"));
    CHECK(!noted("unknown key"));

    load_debug("fb_dump_at=61,121");
    CHECK(streq(recomp_env(RENV_FB_DUMP_AT), "61,121"));
    CHECK(!noted("unknown key"));

    /* A key without commas ends at the comma; the fragment is a typo. */
    load_debug("d3d11_dump=x,bogus");
    CHECK(streq(recomp_env(RENV_D3D11_DUMP), "x"));
    CHECK(noted("unknown key 'bogus'"));

    /* A switch (no value) ends a comma value: the fragment after it is not
     * glued back onto the earlier list. */
    load_debug("vsh_const_prog=AA,pb_fast,BB");
    CHECK(streq(recomp_env(RENV_VSH_CONST_PROG), "AA"));
    CHECK(noted("unknown key 'BB'"));
}

static void test_later_wins(void)
{
    load_debug("pb_fast=1,pb_fast=0");
    CHECK(streq(recomp_env(RENV_PB_FAST), "0"));

    load_debug("vsh_const_prog=AA,BB,vsh_const_prog=CC");
    CHECK(streq(recomp_env(RENV_VSH_CONST_PROG), "CC"));
}

static void test_comma_keys_exist(void)
{
    size_t i;
    int id;

    for (i = 0; i < sizeof s_comma_keys / sizeof s_comma_keys[0]; i++) {
        for (id = 0; id < RENV_COUNT; id++)
            if (!strcmp(s_keys[id].name, s_comma_keys[i]))
                break;
        if (id == RENV_COUNT)
            fprintf(stderr, "s_comma_keys: '%s' is not a key\n", s_comma_keys[i]);
        CHECK(id < RENV_COUNT && s_keys[id].tier != TIER_CONFIG);
    }
}

int main(void)
{
    test_comma_values();
    test_later_wins();
    test_comma_keys_exist();
    load_debug(NULL);
    printf("recomp_env_list: %d checks, %d failed\n", s_checks, s_fail);
    return s_fail != 0;
}
