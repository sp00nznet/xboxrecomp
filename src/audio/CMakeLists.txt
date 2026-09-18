# Xbox DirectSound Compatibility Layer
# Software mixer with waveOut/XAudio2 backend

add_library(xbox_dsound STATIC
    dsound_device.c
    wma_decoder.c
)

target_include_directories(xbox_dsound PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Links to APU emulation for mixer voices
target_link_libraries(xbox_dsound PUBLIC xbox_apu)

# platform carries the src/ include path (and on Linux the POSIX Win32
# primitives), so both hosts need it for "platform/xbox_winnt.h".
target_link_libraries(xbox_dsound PUBLIC platform)

# Windows uses the same Media Foundation stack as xbox_video so ordinary WMA
# assets can be decoded to PCM without adding another codec dependency.
if(WIN32)
    target_link_libraries(xbox_dsound PUBLIC mfplat mfreadwrite mfuuid mf ole32)
endif()

if(MSVC)
    target_compile_options(xbox_dsound PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_dsound PRIVATE -Wall -Wextra -O2)
endif()
