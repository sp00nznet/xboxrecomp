# MCPX APU Emulation Library
# Extracted from xemu (https://github.com/xemu-project/xemu)
#
# Voice Processor (VP): ADPCM/PCM decode, sample mixing, envelopes, filters
# Global Processor (GP): Stub - effects bypass
# Encode Processor (EP): Stub - direct mixbin passthrough

add_library(xbox_apu STATIC
    apu_core.c
    apu_vp.c
    apu_dsp.c
    apu_mmio_hook.c
    apu_fault_bench.c
    apu_xaudio2.c
    apu_sdl.c
    apu_wav.c
    apu_vclock.c
    apu_lockstat.c
)

target_include_directories(xbox_apu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# XBOXRECOMP_APU_DSP (top-level CMakeLists.txt): the dsp56300 JIT. PRIVATE,
# so its header stays inside the APU; a static xbox_apu still carries the
# archive and its system libraries to the game's link. apu_dsp_emu.c is the
# GP/EP device on it and apu_dsp_run.c runs it each APU frame. With them in,
# XBOXRECOMP_APU_DSP_DEVICE (PUBLIC on recomp_env, like XBOXRECOMP_APU_DSP)
# lets RECOMP_APU_DSP=1 through the key table and makes it the run-time
# default; RECOMP_APU_DSP=0 still plays as the stub.
if(XBOXRECOMP_APU_DSP_TARGET)
    target_sources(xbox_apu PRIVATE apu_dsp_emu.c apu_dsp_run.c)
    target_link_libraries(xbox_apu PRIVATE ${XBOXRECOMP_APU_DSP_TARGET})
    target_compile_definitions(recomp_env PUBLIC XBOXRECOMP_APU_DSP_DEVICE)
endif()

# qemu_shim.h lives in nv2a/ - shared QEMU type abstraction
target_include_directories(xbox_apu PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../nv2a
)

if(MSVC)
    target_compile_options(xbox_apu PRIVATE
        /W3
        /wd4244  # conversion warnings (float/double/int)
        /wd4146  # unary minus on unsigned
        /wd4267  # size_t to int conversion
    )
    target_compile_definitions(xbox_apu PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _USE_MATH_DEFINES
    )
endif()

if(WIN32)
    # xaudio2_8 for XAudio2Create, which apu_xaudio2.c calls directly.
    # It was missing: nothing had ever referenced this library from a linked
    # target, so the archive was built and never pulled in, and the first
    # target to actually call into the APU failed at link with an undefined
    # XAudio2Create rather than at build time here.
    target_link_libraries(xbox_apu PRIVATE platform kernel32 winmm xaudio2_8 ole32)
else()
    # POSIX (macOS, Linux): platform layer (Win32 primitives on POSIX),
    # SDL3 for the audio device (apu_sdl.c) and libm
    find_package(SDL3 3.2 CONFIG REQUIRED)
    target_link_libraries(xbox_apu PRIVATE platform SDL3::SDL3 m)
endif()
