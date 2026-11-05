/**
 * Shared MSL compile options and the pinned-version fallback; see
 * metal_compile.h.
 */
#if defined(__APPLE__)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>
#include "recomp_env.h"
#include "metal_compile.h"

#if __has_feature(objc_arc)
#error "metal_compile.m is manual retain/release: build it without -fobjc-arc"
#endif

/* Built once and never released. The present library (window creation)
 * and the pb backend (ack thread) can make their first compile at the same
 * moment, so the init runs under dispatch_once. */
static MTLCompileOptions *s_pinned, *s_os;
static unsigned s_version;
static dispatch_once_t s_once;

/* The versions MTLLanguageVersion names (1.0 to 4.1). Anything else would
 * fail every compile, the backend's base library first, so it is ignored
 * with a warning instead. */
static int known_version(unsigned maj, unsigned min)
{
    switch (maj) {
    case 1: return min <= 2;
    case 2: return min <= 4;
    case 3: return min <= 2;
    case 4: return min <= 1;
    default: return 0;
    }
}

static MTLCompileOptions *make_options(int pin)
{
    MTLCompileOptions *opts = [[MTLCompileOptions alloc] init];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    opts.fastMathEnabled = NO;
#pragma clang diagnostic pop
    if (@available(macOS 15.0, iOS 18.0, *)) {
        opts.mathMode = MTLMathModeSafe;
        opts.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
    }
    if (!pin)
        return opts;
    if (@available(macOS 13.0, iOS 16.0, *)) {
        const char *v = recomp_env(RENV_METAL_MSL_VERSION);
        unsigned maj = 3, min = 0;
        if (v && *v) {
            unsigned a = 0, b = 0;
            int n = sscanf(v, "%u.%u", &a, &b);
            if (n >= 1 && a == 0)
                maj = 0;
            else if (n >= 1 && known_version(a, b))
                maj = a, min = b;
            else
                fprintf(stderr, "[metal] metal_msl_version=%s is not a known MSL version"
                        " (1.0 to 4.1, or 0): ignored, shaders compile at 3.0\n", v);
        }
        if (maj) {
            s_version = (maj << 16) | min;
            opts.languageVersion = (MTLLanguageVersion)s_version;
        }
    }
    return opts;
}

static void init_options(void *unused)
{
    (void)unused;
    s_pinned = make_options(1);
    s_os = make_options(0);
}

MTLCompileOptions *metal_compile_options(void)
{
    dispatch_once_f(&s_once, NULL, init_options);
    return s_pinned;
}

unsigned metal_compile_pinned_version(void)
{
    metal_compile_options();
    return s_version;
}

id<MTLLibrary> metal_compile_library(id<MTLDevice> dev, NSString *src,
                                     const char *what, NSError **err)
{
    static atomic_flag said = ATOMIC_FLAG_INIT;
    NSError *e = nil, *e2 = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:src options:metal_compile_options()
                                             error:&e];
    if (lib || !s_version) {
        if (err)
            *err = e;
        return lib;
    }
    lib = [dev newLibraryWithSource:src options:s_os error:&e2];
    if (lib && !atomic_flag_test_and_set(&said))
        fprintf(stderr, "[metal] %s failed at MSL %u.%u and compiled at the OS default"
                " (slower; keep the generators within the pin): %s\n", what,
                s_version >> 16, s_version & 0xffff,
                e ? [[e localizedDescription] UTF8String] : "?");
    if (err)
        *err = e;
    return lib;
}

#endif
