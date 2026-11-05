/* apu_xaudio2.h's pure parts: the buffer count per host (deeper under Wine,
 * where the APU frame thread can be preempted past 4 buffers), the pacing
 * marks each count gives, and the APU thread priority key. */
#include "apu_xaudio2.h"

#include <stdio.h>

static int failures;
#define CHECK(name, cond) \
    do { if (!(cond)) { printf("FAIL: %s\n", name); failures++; } } while (0)

static void test_counts(void)
{
    CHECK("native count 4", xa2_default_buf_count(0) == 4);
    CHECK("wine count", xa2_default_buf_count(1) == XA2_NUM_BUFS_WINE);
    /* 4 stays legal: the A/B may pick the priority route over depth. */
    CHECK("wine count within 4..8 storage",
          XA2_NUM_BUFS_WINE >= XA2_NUM_BUFS_DEFAULT && XA2_NUM_BUFS_WINE <= 8);
    CHECK("buffer 512", XA2_BUF_SAMPLES_DEFAULT == 512);
}

static void test_marks(void)
{
    static const struct { int count, low, high; } c[] = {
        { 4, 1024, 2048 }, { 6, 1536, 3072 }, { 8, 2048, 4096 },
    };
    unsigned i;
    for (i = 0; i < sizeof c / sizeof c[0]; i++) {
        int low, high;
        char name[64];
        xa2_pacing_marks(512, c[i].count, 256, &low, &high);
        snprintf(name, sizeof name, "marks 512 x %d", c[i].count);
        CHECK(name, low == c[i].low && high == c[i].high);
    }
}

static void test_priority(void)
{
    int bad;
    CHECK("unset is normal", xa2_parse_priority(NULL, 0, &bad) == XA2_PRIO_NORMAL && !bad);
    CHECK("empty is normal", xa2_parse_priority("", 0, &bad) == XA2_PRIO_NORMAL && !bad);
    CHECK("normal", xa2_parse_priority("normal", 0, &bad) == XA2_PRIO_NORMAL && !bad);
    CHECK("highest", xa2_parse_priority("highest", 0, &bad) == XA2_PRIO_HIGHEST && !bad);
    CHECK("time_critical without the debug key is highest, reported",
          xa2_parse_priority("time_critical", 0, &bad) == XA2_PRIO_HIGHEST && bad);
    CHECK("time_critical with the debug key",
          xa2_parse_priority("time_critical", 1, &bad) == XA2_PRIO_TIME_CRITICAL && !bad);
    CHECK("unknown is normal, reported",
          xa2_parse_priority("realtime", 1, &bad) == XA2_PRIO_NORMAL && bad);
    CHECK("names", !strcmp(xa2_priority_name(XA2_PRIO_HIGHEST), "highest") &&
                   !strcmp(xa2_priority_name(XA2_PRIO_TIME_CRITICAL), "time_critical") &&
                   !strcmp(xa2_priority_name(XA2_PRIO_NORMAL), "normal"));
}

int main(void)
{
    test_counts();
    test_marks();
    test_priority();
    if (failures) return 1;
    printf("xa2_defaults: all passed\n");
    return 0;
}
