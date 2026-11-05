# apu_dsp_key - RECOMP_APU_DSP and its interactions with apu_dsp_ack and
# apu_mixdown_all (src/platform/recomp_env.c), built four ways: without the
# APU DSP, with it but without its device (XBOXRECOMP_APU_DSP_DEVICE, which
# the device model defines), with both, and with both plus a game default of
# 0 (a game that opts out of the default JIT). Pure C, no dsp56300 needed:
#   cmake -S tests/apu_dsp_key -B build/apu_dsp_key
#   cmake --build build/apu_dsp_key && ctest --test-dir build/apu_dsp_key
cmake_minimum_required(VERSION 3.20)
project(apu_dsp_key_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
enable_testing()
# test_main.c includes recomp_env.c itself, for its held notes.
foreach(variant nodsp dsp device game_stub)
    add_executable(apu_dsp_key_${variant} test_main.c)
    target_include_directories(apu_dsp_key_${variant} PRIVATE ${XR_SRC}/platform)
    if(NOT MSVC)
        target_compile_options(apu_dsp_key_${variant} PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME apu_dsp_key_${variant} COMMAND apu_dsp_key_${variant})
endforeach()
target_compile_definitions(apu_dsp_key_dsp PRIVATE XBOXRECOMP_APU_DSP)
target_compile_definitions(apu_dsp_key_device PRIVATE
    XBOXRECOMP_APU_DSP XBOXRECOMP_APU_DSP_DEVICE)
target_compile_definitions(apu_dsp_key_game_stub PRIVATE
    XBOXRECOMP_APU_DSP XBOXRECOMP_APU_DSP_DEVICE TEST_GAME_STUB)
