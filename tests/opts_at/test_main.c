/* opts_at - see CMakeLists.txt. */
#include <stdio.h>
#include <string.h>

#include "nv2a_opts_at.h"

static int s_fail;

#define CHECK(c) do { if (!(c)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); s_fail = 1; } } while (0)

static struct nv2a_opts_at e[NV2A_OPTS_AT_MAX];

static void good_entries(void)
{
    int n = nv2a_opts_at_parse("600:aniso=16,700:filter=integer,800:fullscreen=1,"
                               "900:movie=fill,1000:overscan=24,1100:pacing=sleep,"
                               "1200:scale=3,1300:snap_2d=1", e);
    CHECK(n == 8);
    CHECK(e[0].flip == 600 && e[0].mask == NV2A_OPT_ANISO && e[0].o.aniso == 16);
    CHECK(e[1].mask == NV2A_OPT_FILTER && e[1].o.present_filter == NV2A_PRESENT_INTEGER);
    CHECK(e[2].mask == NV2A_OPT_FULLSCREEN && e[2].o.fullscreen == 1);
    CHECK(e[3].mask == NV2A_OPT_MOVIE && e[3].o.movie_mode == NV2A_MOVIE_FILL);
    CHECK(e[4].mask == NV2A_OPT_OVERSCAN && e[4].o.overscan == 24);
    CHECK(e[5].mask == 0 && e[5].pacing == 1);
    CHECK(e[6].mask == NV2A_OPT_SCALE && e[6].o.render_scale == 3);
    CHECK(e[7].mask == NV2A_OPT_SNAP2D && e[7].o.snap_2d == 1);
    CHECK(e[0].pacing == -1);
    CHECK(nv2a_opts_at_parse(NULL, e) == 0);
    CHECK(nv2a_opts_at_parse("", e) == 0);
}

static void bad_entries(void)
{
    /* each bad one is skipped, the good one after it kept */
    static const char *const bad[] = {
        "x:aniso=4", "-1:aniso=4", ":aniso=4", "12a:aniso=4",  /* flip */
        "10:zoom=2", "10:=2", "10aniso=2", "10:aniso",         /* key */
        "10:aniso=", "10:filter=", "10:aniso=4x",              /* value */
        "10:filter=bilinear", "10:movie=stretch", "10:pacing=nap", "10:snap_2d=on",
    };
    char buf[64];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        snprintf(buf, sizeof buf, "%s,5:aniso=2", bad[i]);
        int n = nv2a_opts_at_parse(buf, e);
        CHECK(n == 1);
        if (n != 1)
            printf("  entry '%s'\n", bad[i]);
        CHECK(e[0].flip == 5 && e[0].o.aniso == 2);
    }
}

static void cap(void)
{
    char buf[1024];
    size_t o = 0;
    for (int i = 0; i < NV2A_OPTS_AT_MAX + 3; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%s%d:aniso=4", i ? "," : "", i);
    CHECK(nv2a_opts_at_parse(buf, e) == NV2A_OPTS_AT_MAX);
    CHECK(e[NV2A_OPTS_AT_MAX - 1].flip == NV2A_OPTS_AT_MAX - 1);
}

int main(void)
{
    good_entries();
    bad_entries();
    cap();
    printf(s_fail ? "opts_at: FAIL\n" : "opts_at: ok\n");
    return s_fail;
}
