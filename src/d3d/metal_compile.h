/**
 * The compile options every runtime MSL library uses: the pushbuffer
 * executor's backend (nv2a_pb_metal.m) and the window's present and overlay
 * shaders (src/video/fb_present_metal.m). One place, so the two cannot drift
 * apart and one process never mixes two language versions or math modes.
 *
 * Fast math is off and, on macOS 15 and later, math mode safe with precise
 * functions: the CPU path is IEEE, and reassociation or flushed denormals
 * would move pixels across the golden tolerance.
 *
 * The language version is pinned to MSL 3.0. Left to the OS default (MSL 4.1
 * on macOS 27), every library whose functions take a texture costs about
 * 100 ms more to compile, so a combiner took 120 ms instead of 20 and the
 * first frame of a new view froze for its shaders (first-sight shader
 * stalls). 3.0 is the floor of the macOS 13 target, and the MSL generators
 * (d3d8_msl.h) and the built-in sources must stay within it. If one does
 * not, metal_compile_library retries that source once at the OS default and
 * says so, rather than losing the shader. metal_msl_version=M.m sets
 * another known version, and 0 takes the OS default.
 *
 * Objective-C, manual retain/release, macOS and iOS only.
 */
#ifndef XBOXRECOMP_METAL_COMPILE_H
#define XBOXRECOMP_METAL_COMPILE_H

#if defined(__APPLE__) && defined(__OBJC__)
#import <Metal/Metal.h>

/* The shared options (pinned); owned here, never released by the caller. */
MTLCompileOptions *metal_compile_options(void);

/* The pinned language version (major << 16 | minor), or 0 when the options
 * leave it to the OS (metal_msl_version=0, or before macOS 13). */
unsigned metal_compile_pinned_version(void);

/* newLibraryWithSource with the shared options (+1, the caller releases).
 * When a pinned compile fails, the same source compiles once more at the OS
 * default: the first such fallback per process is logged with the reason
 * the pinned compile gave. what names the source in that line. On failure
 * *err (if err) holds the pinned compile's error. */
id<MTLLibrary> metal_compile_library(id<MTLDevice> dev, NSString *src,
                                     const char *what, NSError **err);

#endif
#endif
