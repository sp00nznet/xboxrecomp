/* D3D11 presentation for the framebuffer window (see gpu_present.c). */
#ifndef XBOX_GPU_PRESENT_H
#define XBOX_GPU_PRESENT_H
#if defined(_WIN32)
#include <windows.h>
#include <stdint.h>

int  gpu_present_enabled(void);                 /* RECOMP_D3D_PRESENT=1 */
int  gpu_present_init(HWND hwnd, uint32_t width, uint32_t height);
/* width*height B8G8R8A8; returns 0 when nothing was shown (occluded). */
int  gpu_present_frame(const uint32_t *pixels);
int  gpu_present_active(void);
void gpu_present_shutdown(void);
#endif
#endif
