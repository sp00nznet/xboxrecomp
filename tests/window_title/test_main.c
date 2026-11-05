/* The title bar rules (window_title.h): the name alone unless stats are
 * asked for, and without them no write while the name stays the same. */
#include <stdio.h>
#include <string.h>
#include "window_title.h"

#define CHECK(x) do { if (!(x)) { printf("FAIL line %d: %s\n", __LINE__, #x); return 1; } } while (0)

int main(void)
{
    struct xbox_title_clock c;
    char tb[64];
    double fps = -1.0;
    int t, writes;

    /* Format: the name, or the name and the counters. */
    xbox_title_format(tb, sizeof tb, "Example", 0, 59.94, 1, 812);
    CHECK(!strcmp(tb, "Example"));
    xbox_title_format(tb, sizeof tb, "Example", 1, 59.94, 1, 812);
    CHECK(!strcmp(tb, "Example | FPS: 59.9 | draws: 812"));
    xbox_title_format(tb, sizeof tb, "Example", 1, 30.0, 0, 812);
    CHECK(!strcmp(tb, "Example | FPS: 30.0"));
    xbox_title_format(tb, 8, "Example (long)", 0, 0, 0, 0);
    CHECK(!strcmp(tb, "Example"));               /* truncated, terminated */

    /* Without stats: the first call writes, then nothing for a minute of
     * frames until the name changes. */
    memset(&c, 0, sizeof c);
    CHECK(xbox_title_due(&c, 0, 5, 0, 0, &fps) && fps == 0.0);
    for (writes = 0, t = 16; t < 60000; t += 16)
        writes += xbox_title_due(&c, 0, (uint64_t)t, (uint64_t)t / 16, 0, &fps);
    CHECK(writes == 0);
    CHECK(xbox_title_due(&c, 0, 60000, 3750, 1, &fps));
    CHECK(!xbox_title_due(&c, 0, 61000, 3812, 1, &fps));

    /* With stats: at once (fps 0), then once a second with the rate. */
    memset(&c, 0, sizeof c);
    CHECK(xbox_title_due(&c, 1, 1000, 100, 0, &fps) && fps == 0.0);
    CHECK(!xbox_title_due(&c, 1, 1999, 160, 0, &fps));
    CHECK(xbox_title_due(&c, 1, 2000, 160, 0, &fps) && fps == 60.0);
    for (writes = 0, t = 2016; t < 12000; t += 16)
        writes += xbox_title_due(&c, 1, (uint64_t)t, (uint64_t)t * 60 / 1000, 0, &fps);
    CHECK(writes == 9 || writes == 10);

    /* The certificate name: UTF-16 to UTF-8, a character never split. */
    {
        static const uint16_t name[] = { 'E', 'x', 'a', 'm', 'p', 'l', 'e', 0, 'x' };
        static const uint16_t wide[] = { 'A', 0x00E9, 0x3042, 0 };   /* A, e-acute, hiragana a */
        char u[16];
        CHECK(xbox_title_utf16_to_utf8(u, sizeof u, name, 40) == 7 && !strcmp(u, "Example"));
        CHECK(xbox_title_utf16_to_utf8(u, sizeof u, name, 3) == 3 && !strcmp(u, "Exa"));
        CHECK(xbox_title_utf16_to_utf8(u, sizeof u, wide, 40) == 6
              && !strcmp(u, "A\xC3\xA9\xE3\x81\x82"));
        CHECK(xbox_title_utf16_to_utf8(u, 6, wide, 40) == 3 && !strcmp(u, "A\xC3\xA9"));
    }

    printf("window_title: ok\n");
    return 0;
}
