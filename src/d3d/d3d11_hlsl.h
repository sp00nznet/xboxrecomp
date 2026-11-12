/**
 * HLSL to DXBC without d3dcompiler_47: the Linux D3D11 host's compiler
 * (d3d11_host_sdl.c), over vkd3d-shader's own C API (d3d11_hlsl_vkd3d.c).
 *
 * Under Proton the backend's D3DCompile is Wine's d3dcompiler_47, which is
 * vkd3d-shader already, so this is the same compiler without Wine.
 *
 * Its own header and translation unit, with no Windows or D3D type: calling
 * D3DCompile through dxvk-native's d3dcompiler.h into libvkd3d-utils
 * crashed on a header ABI mismatch (the linux-native-vulkan spike), while
 * the vkd3d_shader_compile API in a file that never sees the DXVK headers
 * works. The caller wraps the bytes in its own ID3DBlob.
 */
#ifndef D3D11_HLSL_H
#define D3D11_HLSL_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The compiler's version string ("vkd3d-shader 1.18"; the build needs 1.18 or later), part of the shader
 * cache key; NULL when there is none. */
const char *d3d11_hlsl_version(void);

/* D3DCompile's job: src (len bytes, NUL not needed) with entry point entry
 * for target ("vs_5_0", "ps_5_0") to DXBC. flags1 and flags2 are
 * D3DCompile's (D3DCOMPILE_*); d3d11_hlsl_flags says what is kept. name
 * labels the messages. On success *code (d3d11_hlsl_free) and *code_len
 * hold the bytecode. *messages, when not NULL, gets the compiler's text
 * (d3d11_hlsl_free; may stay NULL). Returns 0 on success, else vkd3d's
 * negative result. */
int d3d11_hlsl_compile(const void *src, size_t len, const char *name, const char *entry,
                       const char *target, unsigned flags1, unsigned flags2,
                       void **code, size_t *code_len, char **messages);

/* Of flags1, the bits the compile maps to a vkd3d option; the rest (the
 * optimisation level, debug and validation bits) have no vkd3d meaning and
 * are dropped, as Wine's D3DCompile drops them ("Ignoring flags"). */
unsigned d3d11_hlsl_flags(unsigned flags1);

/* Frees what d3d11_hlsl_compile returned. */
void d3d11_hlsl_free(void *p);

#ifdef __cplusplus
}
#endif

#endif /* D3D11_HLSL_H */
