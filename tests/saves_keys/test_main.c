/*
 * saves_keys - the build's console keys are zero and xbox_saves_keys_id
 * says "zero" (pad-recording-ui R6, safeguard 1). Every build of every
 * host then writes keys = "zero" in a saves zip, so a keys mismatch can
 * only come from a foreign or hand-edited zip, and the re-sign under the
 * zero key gives the digest this build's title writes.
 */
#include "kernel.h"

#include <stdio.h>
#include <string.h>

static int s_fail;

#define CHECK(c)                                                    \
    do {                                                            \
        if (!(c)) {                                                 \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); \
            s_fail = 1;                                             \
        }                                                           \
    } while (0)

static int all_zero(const UCHAR *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (p[i])
            return 0;
    return 1;
}

int main(void)
{
    CHECK(all_zero(xbox_HDKey, sizeof xbox_HDKey));
    CHECK(all_zero(xbox_SignatureKey, sizeof xbox_SignatureKey));
    CHECK(!strcmp(xbox_saves_keys_id(), "zero"));

    /* A key set that is not zero gets a fingerprint instead, so the zero
     * id above is the arrays' test and not a constant. */
    xbox_SignatureKey[3] = 0x5a;
    {
        const char *id = xbox_saves_keys_id();
        CHECK(strcmp(id, "zero") != 0);
        CHECK(strlen(id) == 16);
    }
    xbox_SignatureKey[3] = 0;
    CHECK(!strcmp(xbox_saves_keys_id(), "zero"));

    if (s_fail)
        return 1;
    puts("saves_keys: ok");
    return 0;
}
