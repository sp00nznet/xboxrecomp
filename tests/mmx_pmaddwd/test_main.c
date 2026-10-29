#include <stdint.h>
#include <stdio.h>

#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"

typedef struct {
    int16_t a[4], b[4];
    uint32_t expected[2];
} TestCase;

int main(void)
{
    static const TestCase cases[] = {
        {{-32768, -32768, -32768, -32768}, {-32768, -32768, -32768, -32768},
         {0x80000000u, 0x80000000u}},
        {{-32768, -32768, 1, -2}, {-32768, -32768, -3, 4},
         {0x80000000u, 0xFFFFFFF5u}},
        {{1, -2, -32768, -32768}, {-3, 4, -32768, -32768},
         {0xFFFFFFF5u, 0x80000000u}},
        {{-32768, -32768, 32767, 32767}, {32767, 32767, 32767, 32767},
         {0x80010000u, 0x7FFE0002u}},
        {{-32768, 32767, 0, 0}, {32767, 32767, -32768, 32767},
         {0xFFFF8001u, 0}},
        {{0, 0, 0, 0}, {0, 0, 0, 0}, {0, 0}},
    };
    unsigned failures = 0;
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        RecompMmx a = {0}, b = {0};
        for (unsigned j = 0; j < 4; j++) {
            a.w[j] = cases[i].a[j];
            b.w[j] = cases[i].b[j];
        }
        RecompMmx result = MMX_PMADDWD(a, b);
        for (unsigned lane = 0; lane < 2; lane++) {
            if (result.ud[lane] != cases[i].expected[lane]) {
                fprintf(stderr, "case %u lane %u: got %08x, expected %08x\n",
                        i, lane, result.ud[lane], cases[i].expected[lane]);
                failures++;
            }
        }
    }
    if (!failures)
        puts("PMADDWD edge cases passed");
    return failures != 0;
}
