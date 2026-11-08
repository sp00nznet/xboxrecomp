/* The EEPROM user settings: the time zone is the host's, and XC_MAX_OS --
 * the whole block, which XAPI reads in one query -- agrees with the single
 * queries. */
#include "kernel.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
/* This settings-only test must never dispatch guest code. */
void *recomp_lookup(ULONG address) { (void)address; abort(); }
void *recomp_lookup_manual(ULONG address) { (void)address; abort(); }

static LONG host_bias(void)
{
#ifdef _WIN32
    TIME_ZONE_INFORMATION tzi;
    memset(&tzi, 0, sizeof tzi);
    GetTimeZoneInformation(&tzi);
    return tzi.Bias;
#else
    time_t now = time(NULL);
    struct tm lt;
    tzset();
    localtime_r(&now, &lt);
    return (LONG)(-lt.tm_gmtoff / 60);
#endif
}

static ULONG query(ULONG index)
{
    ULONG v = 0xdeadbeef, type = 0, len = 0;
    xbox_ExQueryNonVolatileSetting(index, &type, &v, sizeof v, &len);
    return v;
}

int main(int argc, char **argv)
{
    uint8_t blk[256];
    ULONG expected_video, type = 0, len = 0, v;
    LONG bias = (LONG)query(XC_TIMEZONE_BIAS);
    char *end;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "expected the XC_VIDEO flags\n");
        return 2;
    }
    expected_video = (ULONG)strtoul(argv[1], &end, 0);
    if (!argv[1][0] || *end) {
        fprintf(stderr, "invalid XC_VIDEO flags: %s\n", argv[1]);
        return 2;
    }
    if (query(XC_VIDEO) != expected_video) {
        fprintf(stderr, "FAIL: XC_VIDEO 0x%08lX, expected 0x%08lX\n",
                (unsigned long)query(XC_VIDEO),
                (unsigned long)expected_video);
        return 1;
    }
    if (bias != host_bias()) {
        fprintf(stderr, "FAIL: XC_TIMEZONE_BIAS %ld, host bias %ld\n", (long)bias, (long)host_bias());
        return 1;
    }
    memset(blk, 0xCC, sizeof blk);
    if (xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, blk, sizeof blk, &len)
        || type != 3 || len != 0x60) {
        fprintf(stderr, "FAIL: XC_MAX_OS type %lu length %lu\n", (unsigned long)type, (unsigned long)len);
        return 1;
    }
    memcpy(&v, blk + 0x04, 4);
    if ((LONG)v != bias) {
        fprintf(stderr, "FAIL: block bias %ld, single query %ld\n", (long)(LONG)v, (long)bias);
        return 1;
    }
    {
        static const struct { ULONG index, off; } f[] = {
            {XC_TZ_STD_BIAS, 0x28}, {XC_TZ_DLT_BIAS, 0x2C}, {XC_LANGUAGE, 0x30},
            {XC_VIDEO, 0x34}, {XC_AUDIO, 0x38}, {XC_MISC, 0x58}, {XC_DVD_REGION, 0x5C},
        };
        for (i = 0; i < sizeof f / sizeof f[0]; i++) {
            memcpy(&v, blk + f[i].off, 4);
            if (v != query(f[i].index)) {
                fprintf(stderr, "FAIL: index 0x%02lX: block 0x%lX at +0x%02lX, query 0x%lX\n",
                        (unsigned long)f[i].index, (unsigned long)v, (unsigned long)f[i].off,
                        (unsigned long)query(f[i].index));
                return 1;
            }
        }
    }
    {
        uint8_t small[4];
        len = 0;
        if (xbox_ExQueryNonVolatileSetting(XC_MAX_OS, &type, small, sizeof small, &len)
                != (NTSTATUS)0xC0000023L || len != 0x60) {
            fprintf(stderr, "FAIL: XC_MAX_OS into 4 bytes: want BUFFER_TOO_SMALL and 0x60\n");
            return 1;
        }
    }
    for (i = 0x60; i < sizeof blk; i++)
        if (blk[i]) {
            fprintf(stderr, "FAIL: byte 0x%02X past the block is 0x%02X\n", (unsigned)i, blk[i]);
            return 1;
        }
    printf("PASS: time zone bias %ld (host), XC_VIDEO 0x%08lX, "
           "XC_MAX_OS block matches the single queries.\n",
           (long)bias, (unsigned long)expected_video);
    return 0;
}
