cmake_minimum_required(VERSION 3.20)

# Xbox Kernel Replacement Layer
# Replaces 147 Xbox kernel imports with Win32 equivalents.
# Memory layout, file I/O, threading, synchronization, crypto, HAL.

set(KERNEL_SOURCES
    kernel_path.c
    kernel_pool.c
    kernel_rtl.c
    kernel_memory.c
    kernel_file.c
    kernel_ramfs.c
    kernel_missing.c
    kernel_thread.c
    kernel_user_apc.c
    kernel_sync.c
    kernel_hal.c
    kernel_prof.c
    kernel_pacing.c
    guest_time.c
    guest_state_hash.c
    kernel_guest_cpu.c
    kernel_guest_det.c
    kernel_xbox.c
    kernel_language.c
    kernel_ob.c
    kernel_io.c
    kernel_crypto.c
    kernel_thunks.c
    kernel_bridge.c
    xbox_memory_layout.c
    xbe_sections.c
    guest_vmem.c
    icall_feedback.c
    recomp_trace.c
    recomp_hook.c
    recomp_hook_run.c
    recomp_guest_call.c
    nv2a_pb_scan.c
    nv2a_pb_exec.c
    nv2a_flip_hold.c
    nv2a_kick_wake.c
    nv2a_backend_common.c
    nv2a_opts_at.c
    nv2a_aspect.c
    nv2a_tex_pack.c
    nv2a_tex_dump.c
    nv2a_vsh_cpu.c
    nv2a_vsh_ff.c
    nv2a_vsh_cdump.c
    nv2a_fb_ring.c
    nv2a_combiner.c
)

# The enhancements overlay (stats strip, toasts, the menu; enhance-menu-
# preview R2, R7, R8): microui and stb_truetype vendored unmodified in
# third_party/, built with their own warnings off, and the Atkinson
# Hyperlegible Regular TTF embedded as C arrays at build time, with the
# Lucide icon subset and the input-glyph font beside it (overlay-revamp
# D7, D8). Only XBOXRECOMP_ENHANCE builds carry any of it.
if(XBOXRECOMP_ENHANCE)
    get_filename_component(_xr_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(_xr_tp "${_xr_root}/third_party")
    set(_ovl_fonts
        "${_xr_tp}/fonts/atkinson-hyperlegible/AtkinsonHyperlegible-Regular.ttf"
        "${_xr_tp}/lucide/lucide-subset.ttf"
        "${_xr_tp}/kenney-input/xbr-input.ttf")
    # The size gate of 3B.1: the embedded faces stay under 0.7 MB.
    set(_ovl_font_bytes 0)
    foreach(_f IN LISTS _ovl_fonts)
        file(SIZE "${_f}" _sz)
        math(EXPR _ovl_font_bytes "${_ovl_font_bytes} + ${_sz}")
    endforeach()
    if(_ovl_font_bytes GREATER_EQUAL 716800)
        message(FATAL_ERROR "overlay fonts: ${_ovl_font_bytes} bytes, over the 0.7 MB cap")
    endif()
    set(_ovl_fonts_c "${CMAKE_CURRENT_BINARY_DIR}/nv2a_overlay_fonts.c")
    # "|" between list items: a ";" would split the command's argument.
    string(REPLACE ";" "|" _ovl_fonts_arg "${_ovl_fonts}")
    add_custom_command(
        OUTPUT "${_ovl_fonts_c}"
        COMMAND ${CMAKE_COMMAND} "-DOUT=${_ovl_fonts_c}"
                "-DNAMES=nv2a_ovl_font_regular|nv2a_ovl_font_icons|nv2a_ovl_font_input"
                "-DFILES=${_ovl_fonts_arg}"
                -P "${_xr_root}/cmake/embed_files.cmake"
        DEPENDS ${_ovl_fonts} "${_xr_root}/cmake/embed_files.cmake"
        COMMENT "Embedding the overlay fonts"
        VERBATIM)
    set(_ovl_vendored "${_xr_tp}/microui/microui.c"
                      "${CMAKE_CURRENT_SOURCE_DIR}/nv2a_overlay_stb.c")
    set_source_files_properties(${_ovl_vendored} PROPERTIES
        COMPILE_OPTIONS "$<IF:$<C_COMPILER_ID:MSVC>,/w,-w>")
    list(APPEND KERNEL_SOURCES
        nv2a_overlay.c
        nv2a_overlay_ui.c
        nv2a_overlay_font.c
        nv2a_overlay_glyph.c
        nv2a_stats.c
        nv2a_present_chain.c
        nv2a_dbg_draw.c
        ${_ovl_vendored}
        "${_ovl_fonts_c}")
endif()

add_library(xbox_kernel STATIC ${KERNEL_SOURCES})

# The languages the game ships (kernel_language.h), from its game.toml
# [system] languages through the CLI: the menu offers only these, and
# system.language refuses any other. On kernel_language.c alone, so a new
# list recompiles one file. Empty: the game declares none.
set(XBOXRECOMP_LANGUAGES "" CACHE STRING
    "Languages the game ships, a comma list of en ja de fr es it ko zh pt (game.toml [system] languages)")
if(XBOXRECOMP_LANGUAGES)
    string(REPLACE "," ";" _xr_langs "${XBOXRECOMP_LANGUAGES}")
    foreach(_l IN LISTS _xr_langs)
        if(NOT _l MATCHES "^(en|ja|de|fr|es|it|ko|zh|pt)$")
            message(FATAL_ERROR "XBOXRECOMP_LANGUAGES=${XBOXRECOMP_LANGUAGES}: '${_l}' "
                "is not one of en ja de fr es it ko zh pt")
        endif()
    endforeach()
    set_source_files_properties(kernel_language.c PROPERTIES
        COMPILE_DEFINITIONS "XBOXRECOMP_LANGUAGES=\"${XBOXRECOMP_LANGUAGES}\"")
endif()
if(XBOXRECOMP_ENHANCE)
    target_include_directories(xbox_kernel PUBLIC "${_xr_tp}/microui" "${_xr_tp}/stb"
                                                  "${_xr_tp}/librashader")
    # present.shader loads librashader at run time (nv2a_present_chain.c).
    target_link_libraries(xbox_kernel PRIVATE ${CMAKE_DL_LIBS})
endif()

# See src/platform/CMakeLists.txt: resolve src/ relative to this file, not via
# CMAKE_SOURCE_DIR, which is the consuming project's root under add_subdirectory().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

target_include_directories(xbox_kernel PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${XBOXRECOMP_SRC_DIR}
)

if(WIN32)
    # platform is an INTERFACE target on Windows: it contributes the src/
    # include path that "platform/xbox_winnt.h" resolves against.
    target_link_libraries(xbox_kernel PUBLIC platform kernel32 bcrypt)
else()
    # Linux: platform layer (POSIX host primitives) + OpenSSL for crypto.
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xbox_kernel PUBLIC platform OpenSSL::Crypto)
endif()

# The watchdog finalises the APU's WAV dump before _exit (apu_wav_close_nowait).
# Link-only, so xbox_apu follows xbox_kernel on the link line.
target_link_libraries(xbox_kernel PRIVATE xbox_apu)
# The trapped-MMIO dispatch (xbox_PosixMmioFault, xbox_VehMmioFault) routes
# to the APU and to the USB OHCI model.
target_link_libraries(xbox_kernel PRIVATE xbox_usb)

if(MSVC)
    target_compile_options(xbox_kernel PRIVATE /W4 /WX- /O2 /GS- /Gy)
    target_compile_definitions(xbox_kernel PRIVATE _CRT_SECURE_NO_WARNINGS)
else()
    target_compile_options(xbox_kernel PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
endif()

# The trapped-MMIO dispatch (xbox_VehMmioFault, xbox_PosixMmioFault) routes
# to the APU and the USB OHCI models. Link-only: both already call back into
# the kernel, and CMake repeats static libraries in such a cycle.
target_link_libraries(xbox_kernel PRIVATE xbox_apu xbox_usb)

target_compile_definitions(xbox_kernel PRIVATE $<$<CONFIG:Debug>:_DEBUG>)
