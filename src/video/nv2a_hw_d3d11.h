/**
 * NV2A hardware renderer: the pushbuffer executor's draws on a D3D11 device
 * (nv2a_hw_d3d11.c). Windows only.
 */
#ifndef XBOXRECOMP_NV2A_HW_D3D11_H
#define XBOXRECOMP_NV2A_HW_D3D11_H

#include <stdint.h>

/* RECOMP_HW_RENDER=1 registers the D3D11 renderer as the pushbuffer
 * executor's back end (nv2a_backend.h), vertex programs included, on the
 * fastest hardware adapter; RECOMP_HW_RENDER=warp on WARP. Unset or anything
 * else: nothing happens and the executor keeps rasterising on the CPU. Call
 * once at startup, before the title runs; the executor itself is switched on
 * by RECOMP_PB_EXEC. Returns 1 if the renderer was registered. It also turns
 * on RECOMP_STRIP_ALT_WINDING when that is unset: the GPU culls, so strips
 * need the console's winding (nv2a_hw_d3d11.c). */
int xbox_Nv2aHwRendererInstall(void);

/* The same without the switch: warp 0 picks the hardware adapter with the
 * most video memory, 1 WARP. For tests. Returns 1 if registered. */
int nv2a_hw_d3d11_register(int warp);

/* Counters since registration: [0] draws, [1] of them vertex-program
 * batches, [2] program shaders compiled, [3] surfaces loaded from guest
 * memory, [4] surfaces written back, [5] textures decoded. */
void nv2a_hw_d3d11_stats(uint32_t out[6]);

/* RECOMP_HW_SHADER_CACHE: shaders read from the disk cache [0] and compiled
 * [1], since process start. */
void nv2a_hw_d3d11_shader_cache_counts(uint32_t out[2]);

#endif
