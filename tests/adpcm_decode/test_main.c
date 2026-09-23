/* adpcm_decode_block: a block whose reserved header byte is non-zero, or whose
 * step index is out of range, decodes like the hardware decodes it instead of
 * being refused. */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "apu_state.h"

static int failures;
#define CHECK(c, ...) do { if (!(c)) { failures++; printf("FAIL: "); printf(__VA_ARGS__); printf("\n"); } } while (0)

int main(void)
{
    uint8_t blk[36], alt[36];
    int16_t a[65], b[65];
    for (int i = 0; i < 36; ++i) blk[i] = (uint8_t)(i * 37 + 11);
    blk[0] = 0x34; blk[1] = 0x12; blk[2] = 20; blk[3] = 0;

    CHECK(adpcm_decode_block(a, blk, 36, 1) == 65, "a legal block decodes 65 frames");

    /* Reserved byte set: same samples as with it clear. */
    memcpy(alt, blk, 36); alt[3] = 0x08;
    CHECK(adpcm_decode_block(b, alt, 36, 1) == 65, "a non-zero reserved byte is not refused");
    CHECK(!memcmp(a, b, sizeof a), "and it decodes identically to the reserved-clear block");

    /* The 0x08 pad JSRF ends its buffers with: every byte 0x08. */
    memset(alt, 0x08, 36);
    CHECK(adpcm_decode_block(b, alt, 36, 1) == 65, "the all-0x08 pad block decodes");
    CHECK(b[0] == 0x0808, "its first sample is the header's, 0x0808: %d", b[0]);

    /* Step index out of range (and >127, so negative as int8_t): clamped. */
    memcpy(alt, blk, 36); alt[2] = 200;
    CHECK(adpcm_decode_block(b, alt, 36, 1) == 65, "an out-of-range step index is clamped, not refused");
    memcpy(alt, blk, 36); alt[2] = 100;
    CHECK(adpcm_decode_block(b, alt, 36, 1) == 65, "index 100 is clamped to 88");

    /* A short block is still refused. */
    CHECK(adpcm_decode_block(b, blk, 3, 1) == 0, "a block shorter than its header is refused");

    if (failures) { printf("adpcm_decode_test: %d failure(s)\n", failures); return 1; }
    puts("adpcm_decode_test: all checks passed");
    return 0;
}
