/* No game data: exercise the actual runtime helpers under all rounding modes. */
#include <stdio.h>
#include <fenv.h>
#define RECOMP_GENERATED_CODE 1
#include "../../templates/runtime/recomp_types.h"

int main(void)
{
    fenv_t saved;
    const int modes[] = {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO};
    volatile float halves[] = {-2.5f, -1.5f, 1.5f, 2.5f};
    const int32_t rounded[4][4] = {
        {-2, -2, 2, 2}, {-3, -2, 1, 2}, {-2, -1, 2, 3}, {-2, -1, 1, 2}
    };
    const int32_t truncated[] = {-2, -1, 1, 2};
    volatile float limits[] = {
        NAN, INFINITY, -INFINITY, 2147483648.0f, -2147483904.0f,
        -2147483648.0f, 2147483520.0f
    };
    unsigned checks = 0, failures = 0;
    if (feholdexcept(&saved)) return 2;
    for (unsigned mode = 0; mode < 4; ++mode) {
        if (fesetround(modes[mode])) { fesetenv(&saved); return 2; }
        for (int truncate = 0; truncate < 2; ++truncate) {
            for (unsigned pair = 0; pair < 4; pair += 2) {
                RecompMmx result = MMX_FROM_PS(halves[pair], halves[pair + 1], truncate);
                for (unsigned lane = 0; lane < 2; ++lane) {
                    int32_t expected = truncate ? truncated[pair + lane] : rounded[mode][pair + lane];
                    ++checks;
                    if (result.d[lane] != expected) {
                        ++failures;
                        printf("mode=%u truncate=%d input=%g: got=%d expected=%d\n",
                            mode, truncate, (double)halves[pair + lane], result.d[lane], expected);
                    }
                }
            }
            for (unsigned i = 0; i < 7; ++i) {
                RecompMmx result = MMX_FROM_PS(limits[i], 7.0f, truncate);
                uint32_t expected = i == 6 ? 0x7fffff80u : 0x80000000u;
                checks += 2;
                if (result.ud[0] != expected) ++failures;
                if (result.d[1] != 7) ++failures;
            }
        }
    }
    fesetenv(&saved);
    printf("MMX conversions: %u checks, %u failures\n", checks, failures);
    return failures != 0;
}
