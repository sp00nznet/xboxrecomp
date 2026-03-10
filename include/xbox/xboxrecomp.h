/**
 * xboxrecomp.h - Xbox Static Recompilation Runtime
 *
 * Master header for the xboxrecomp link-time library.
 * Include this from your game-specific recompilation project.
 *
 * Usage:
 *   1. Build xboxrecomp as a static library (cmake --build)
 *   2. Link against the 'xboxrecomp' umbrella target (or individual libs)
 *   3. Implement the required callbacks (see "Game Integration Points" below)
 *   4. Call xbox_memory_init() to set up the 64MB Xbox address space
 *   5. Call xbox_kernel_init() / xbox_kernel_bridge_init() to wire up kernel imports
 *
 * Libraries provided:
 *   xbox_kernel  - Xbox kernel → Win32 (memory, file I/O, threading, sync, crypto)
 *   xbox_d3d8    - D3D8 → D3D11 graphics
 *   xbox_dsound  - DirectSound → software mixer
 *   xbox_apu     - MCPX APU audio emulation (from xemu)
 *   xbox_nv2a    - NV2A GPU emulation (from xemu)
 *   xbox_input   - Xbox gamepad → XInput
 *
 * ═══════════════════════════════════════════════════════════════
 * Game Integration Points (YOU must implement these)
 * ═══════════════════════════════════════════════════════════════
 *
 * The kernel bridge calls these to resolve recompiled function addresses:
 *
 *   typedef void (*recomp_func_t)(void);
 *   recomp_func_t recomp_lookup(uint32_t xbox_va);
 *   recomp_func_t recomp_lookup_manual(uint32_t xbox_va);
 *
 * Your recompiler output generates these dispatch tables.
 * See templates/runtime/recomp_types.h for the register model and macros.
 *
 * Global state (defined in xbox_memory_layout.c):
 *   extern ptrdiff_t g_xbox_mem_offset;  // Native-to-Xbox VA offset
 *   extern uint32_t  g_eax, g_ecx, g_edx, g_esp;  // Volatile registers
 *   extern uint32_t  g_ebx, g_esi, g_edi;          // Callee-saved registers
 *   extern uint32_t  g_seh_ebp;                      // SEH frame pointer
 */

#ifndef XBOXRECOMP_H
#define XBOXRECOMP_H

/* Individual module headers */
#include "kernel.h"             /* Xbox kernel types and functions */
#include "xbox_memory_layout.h" /* Memory layout API */
#include "d3d8_xbox.h"          /* D3D8 compatibility interface */
#include "dsound_xbox.h"        /* DirectSound compatibility */
#include "xinput_xbox.h"        /* XInput compatibility */
#include "apu.h"                /* MCPX APU public API */

/* NV2A headers */
#include "nv2a_state.h"         /* NV2A GPU state */
#include "nv2a_regs.h"          /* NV2A register definitions */
#include "qemu_shim.h"          /* QEMU type abstraction */

#endif /* XBOXRECOMP_H */
