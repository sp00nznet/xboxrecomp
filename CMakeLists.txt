cmake_minimum_required(VERSION 3.20)
project(xboxrecomp VERSION 0.14.0 LANGUAGES C)

# Xbox Static Recompilation Runtime Library
# Provides link-time libraries for statically recompiled Xbox games.
# Pattern: extract emulator subsystems (xemu) into reusable static libraries.
#
# Libraries:
#   xbox_kernel    - Xbox kernel replacement layer (147 imports → Win32)
#   xbox_d3d8      - D3D8 → D3D11 graphics compatibility
#   xbox_dsound    - DirectSound → software mixer compatibility
#   xbox_input     - Xbox gamepad → XInput compatibility
#   xbox_apu       - MCPX APU audio emulation (extracted from xemu)
#   xbox_nv2a      - NV2A GPU emulation (extracted from xemu)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# macOS: the Metal render backend (src/d3d/nv2a_pb_metal.m) is Objective-C.
if(APPLE)
    enable_language(OBJC)
endif()

# Common definitions for all targets
add_compile_definitions(
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

# Option to build examples

# The umbrella target the game links (filled in below): made first so the
# reproducible-build flags reach the libraries and, through it, the game.
add_library(xboxrecomp INTERFACE)
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/reproducible.cmake)
xboxrecomp_reproducible(xboxrecomp ${CMAKE_CURRENT_SOURCE_DIR})

# ── Runtime Libraries ─────────────────────────────────────────

# Platform: Win32/NT type vocabulary + POSIX implementation of generic
# Win32 host primitives. Carries no Xbox semantics; provides the foundation
# the kernel HLE and the graphics/audio/input layers build on.
add_subdirectory(src/platform)

# Enhancements (render scale, aspect, frame pacing, mods) and their config
# files, config.toml over config.default.toml. Opt-in: off by default; a
# title turns it on and calls it.
# ON adds the xbox_enhance library and the layer's RECOMP_* config keys
# (recomp_env.h, RECOMP_ENV_HAVE_ENHANCE_KEYS), and defines
# XBOXRECOMP_ENHANCE for the runtime's few seams the layer's mods use (the
# batch filter, the input capture); OFF compiles those out.
option(XBOXRECOMP_ENHANCE "Build the opt-in enhancements layer (src/enhance)" OFF)
if(XBOXRECOMP_ENHANCE)
    target_compile_definitions(recomp_env PUBLIC RECOMP_ENV_HAVE_ENHANCE_KEYS XBOXRECOMP_ENHANCE)
    add_subdirectory(src/enhance)
endif()
# present.shader's presets and librashader, staged beside a game's exe by
# xboxrecomp_present_shaders(<target>). Always defined, so a game can call it
# unconditionally; it does nothing without XBOXRECOMP_ENHANCE.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/librashader.cmake)
# The staged Windows library must load on a stock Windows (an objdump reads
# its import table). Other hosts check the same pinned zip on request: it is
# a 12 MB download no macOS or Linux game needs.
option(XBOXRECOMP_LIBRASHADER_TEST_WIN64
    "Check the x86-64 Windows librashader.dll's imports from a non-Windows build" OFF)
if(CMAKE_OBJDUMP AND XBOXRECOMP_LIBRASHADER AND
   ((WIN32 AND XBOXRECOMP_ENHANCE) OR (NOT WIN32 AND XBOXRECOMP_LIBRASHADER_TEST_WIN64)))
    add_subdirectory(tests/librashader_imports)
endif()

# MCPX APU DSP emulation: the GP and EP run the title's DSP56300 programs on
# the MIT dsp56300 JIT (cmake/dsp56300.cmake, a pinned prebuilt archive per
# target) instead of the stub's host-side ack and stereo fold. It is the
# stock audio path for every game: ON by default on every target
# with an archive, and RECOMP_APU_DSP=0 still picks the stub at run time.
# ON defines XBOXRECOMP_APU_DSP on recomp_env (PUBLIC, so the APU and the key
# table agree) and links dsp56300 into xbox_apu. A target without an archive
# (iPadOS, MSVC, universal macOS) defaults OFF quietly, so such a configure
# never downloads anything; an explicit ON there warns and builds as OFF. A
# failed download is an error. src/apu adds the device model and defines
# XBOXRECOMP_APU_DSP_DEVICE with it.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/dsp56300.cmake)
_xboxrecomp_dsp56300_triple(_dsp56300_triple)
if(_dsp56300_triple)
    set(_dsp56300_default ON)
else()
    set(_dsp56300_default OFF)
endif()
# A tree configured when the option defaulted OFF holds that OFF in its
# cache, which option() never replaces: turn it ON once (cmake/dsp56300.cmake).
xboxrecomp_dsp56300_migrate(${_dsp56300_default})
option(XBOXRECOMP_APU_DSP "${XBOXRECOMP_APU_DSP_HELP}" ${_dsp56300_default})
set(XBOXRECOMP_APU_DSP_TARGET "")
if(XBOXRECOMP_APU_DSP)
    xboxrecomp_dsp56300(XBOXRECOMP_APU_DSP_TARGET _dsp56300_why)
    if(XBOXRECOMP_APU_DSP_TARGET)
        target_compile_definitions(recomp_env PUBLIC XBOXRECOMP_APU_DSP)
    else()
        message(WARNING "XBOXRECOMP_APU_DSP=ON but ${_dsp56300_why}: "
            "building without the APU DSP (the stub only)")
    endif()
endif()

# Core: Xbox kernel replacement (memory layout, file I/O, threading, sync)
add_subdirectory(src/kernel)

# Graphics: D3D8 → D3D11 compatibility layer
add_subdirectory(src/d3d)

# Audio: DirectSound → software mixer compatibility
add_subdirectory(src/audio)

# Audio: MCPX APU hardware emulation (voice processor, DSP)
add_subdirectory(src/apu)

# GPU: NV2A hardware emulation (register handlers, MMIO, push buffer)
add_subdirectory(src/nv2a)

# USB: MCPX OHCI host controllers. A title's XAPI is statically linked and
# drives these registers directly, so a pad only exists if they do.
add_subdirectory(src/usb)

# Input: Xbox gamepad → XInput
add_subdirectory(src/input)

# Shared FMV player (Media Foundation -> D3D8 texture)
add_subdirectory(src/video)

# ── Umbrella target ───────────────────────────────────────────

# Convenience target that links all runtime libraries
set(XBOXRECOMP_LIBS
    xbox_kernel
    xbox_d3d8
    xbox_dsound
    xbox_input
    xbox_usb
    xbox_apu
    xbox_nv2a
    xbox_video
)
if(XBOXRECOMP_ENHANCE)
    # In the list, so GNU ld sees it inside the group below.
    list(APPEND XBOXRECOMP_LIBS xbox_enhance)
endif()
if(UNIX AND NOT APPLE)
    # The archives call each other both ways (the APU reaches the kernel's
    # memory map, the kernel raises the APU's interrupt). GNU ld scans each
    # archive once, so it needs the group; MSVC and ld64 rescan on their own.
    target_link_libraries(xboxrecomp INTERFACE
        -Wl,--start-group ${XBOXRECOMP_LIBS} -Wl,--end-group)
else()
    target_link_libraries(xboxrecomp INTERFACE ${XBOXRECOMP_LIBS})
endif()
