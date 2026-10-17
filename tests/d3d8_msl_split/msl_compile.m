/* d3d8_msl_split's Metal step: compile one MSL source and look up its entry
 * point, with the options nv2a_pb_metal.m's init() uses (fast math off,
 * safe math mode, precise functions). */
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>

int msl_compile(const char *src, const char *fn, char *err, int errsize)
{
    static id<MTLDevice> dev;
    static MTLCompileOptions *opt;
    int rc = 0;
    @autoreleasepool {
        NSError *e = nil;
        id<MTLLibrary> lib;
        if (!dev) {
            dev = MTLCreateSystemDefaultDevice();
            opt = [MTLCompileOptions new];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            opt.fastMathEnabled = NO;
#pragma clang diagnostic pop
            if (@available(macOS 15.0, *)) {
                opt.mathMode = MTLMathModeSafe;
                opt.mathFloatingPointFunctions = MTLMathFloatingPointFunctionsPrecise;
            }
        }
        if (!dev) {
            snprintf(err, (size_t)errsize, "no Metal device");
            return -1;
        }
        lib = [dev newLibraryWithSource:[NSString stringWithUTF8String:src]
                                options:opt error:&e];
        if (!lib) {
            snprintf(err, (size_t)errsize, "%s", e ? e.localizedDescription.UTF8String : "?");
            rc = -1;
        } else {
            id<MTLFunction> f = [lib newFunctionWithName:[NSString stringWithUTF8String:fn]];
            if (!f) {
                snprintf(err, (size_t)errsize, "no function %s", fn);
                rc = -1;
            }
            [f release];
            [lib release];
        }
    }
    return rc;
}
