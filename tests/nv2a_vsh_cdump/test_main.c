/* RECOMP_DEBUG=vsh_const_flips / vsh_const_prog parsing, the filter, and the
 * record layout the header documents (readers depend on it). */
#include "nv2a_vsh_cdump.h"

#include <stdio.h>
#include <string.h>

static int fails;

#define CHECK(cond) do { \
    if (!(cond)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); fails++; } \
} while (0)

static void test_parse(void)
{
    struct nv2a_vsh_cdump_cfg c;

    CHECK(nv2a_vsh_cdump_parse(NULL, NULL, &c) == 0);
    CHECK(c.first == 0 && c.last == 0xFFFFFFFFu && c.every == 1 && c.nprog == 0);

    CHECK(nv2a_vsh_cdump_parse("3300-3900/30", NULL, &c) == 0);
    CHECK(c.first == 3300 && c.last == 3900 && c.every == 30);

    CHECK(nv2a_vsh_cdump_parse("120", NULL, &c) == 0);
    CHECK(c.first == 120 && c.last == 120 && c.every == 1);

    CHECK(nv2a_vsh_cdump_parse("500-", NULL, &c) == 0);
    CHECK(c.first == 500 && c.last == 0xFFFFFFFFu);

    CHECK(nv2a_vsh_cdump_parse("-40/4", NULL, &c) == 0);
    CHECK(c.first == 0 && c.last == 40 && c.every == 4);

    CHECK(nv2a_vsh_cdump_parse("10/5", NULL, &c) == 0);
    CHECK(c.first == 10 && c.last == 10 && c.every == 5);

    CHECK(nv2a_vsh_cdump_parse(NULL, "0x1A2B3C4D,deadbeef", &c) == 0);
    CHECK(c.nprog == 2 && c.prog[0] == 0x1A2B3C4Du && c.prog[1] == 0xDEADBEEFu);

    /* Malformed values switch the dump off rather than dump everything. */
    CHECK(nv2a_vsh_cdump_parse("9-3", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("1-9/0", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("1-9x", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("abc", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "12,", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "12;34", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "1,2,3,4,5,6,7,8,9,a,b,c,d,e,f,10,11", &c) != 0);

    /* Signs, blanks and out-of-range values, which strtoul would take (and
     * a 32-bit unsigned long would wrap without the range check seeing it). */
    CHECK(nv2a_vsh_cdump_parse("5--3", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("-5-9", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("+5-9", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(" 5-9", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("5- 9", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("1-9/-2", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("4294967296", NULL, &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "-1", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "+AA", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "AA, BB", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse(NULL, "100000000", &c) != 0);
    CHECK(nv2a_vsh_cdump_parse("4294967295", "FFFFFFFF", &c) == 0);
    CHECK(c.first == 0xFFFFFFFFu && c.prog[0] == 0xFFFFFFFFu);
}

static void test_wanted(void)
{
    struct nv2a_vsh_cdump_cfg c;

    nv2a_vsh_cdump_parse("100-200/10", "AA,BB", &c);
    CHECK(!nv2a_vsh_cdump_wanted(&c, 99, 0xAA));
    CHECK(nv2a_vsh_cdump_wanted(&c, 100, 0xAA));
    CHECK(!nv2a_vsh_cdump_wanted(&c, 105, 0xAA));
    CHECK(nv2a_vsh_cdump_wanted(&c, 110, 0xBB));
    CHECK(nv2a_vsh_cdump_wanted(&c, 200, 0xAA));
    CHECK(!nv2a_vsh_cdump_wanted(&c, 210, 0xAA));
    CHECK(!nv2a_vsh_cdump_wanted(&c, 110, 0xCC));

    nv2a_vsh_cdump_parse(NULL, NULL, &c);
    CHECK(nv2a_vsh_cdump_wanted(&c, 0, 0) && nv2a_vsh_cdump_wanted(&c, 0xFFFFFFFFu, 1));
}

static void test_record(void)
{
    static float c[NV2A_VSH_CONSTANTS][4];
    unsigned char buf[2 * NV2A_VSH_CDUMP_RECORD + 1];
    uint32_t head[5];
    float f;
    FILE *fp = tmpfile();
    int i;

    CHECK(fp != NULL);
    if (!fp)
        return;
    for (i = 0; i < NV2A_VSH_CONSTANTS; i++) {
        c[i][0] = (float)i; c[i][1] = -1.0f; c[i][2] = 0.5f; c[i][3] = (float)(i * 2);
    }
    CHECK(nv2a_vsh_cdump_write(fp, 3600, 77, 0xCAFEF00Du, 12, 40, (const float (*)[4])c) == 0);
    CHECK(nv2a_vsh_cdump_write(fp, 3601, 78, 1, 0, 1, (const float (*)[4])c) == 0);
    rewind(fp);
    CHECK(fread(buf, 1, sizeof buf, fp) == 2 * NV2A_VSH_CDUMP_RECORD);
    fclose(fp);

    CHECK(NV2A_VSH_CDUMP_RECORD == 3096);
    CHECK(memcmp(buf, "VCD1", 4) == 0);
    memcpy(head, buf + 4, sizeof head);
    CHECK(head[0] == 3600 && head[1] == 77 && head[2] == 0xCAFEF00Du
          && head[3] == 12 && head[4] == 40);
    memcpy(&f, buf + 24 + 191 * 16 + 12, 4);
    CHECK(f == 382.0f);
    CHECK(memcmp(buf + NV2A_VSH_CDUMP_RECORD, "VCD1", 4) == 0);
    memcpy(head, buf + NV2A_VSH_CDUMP_RECORD + 4, sizeof head);
    CHECK(head[0] == 3601 && head[1] == 78);
}

int main(void)
{
    test_parse();
    test_wanted();
    test_record();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("nv2a_vsh_cdump: ok\n");
    return 0;
}
