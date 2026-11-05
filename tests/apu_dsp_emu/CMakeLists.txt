# apu_dsp_emu - the GP/EP device model (src/apu/apu_dsp_emu.c) on the pinned
# dsp56300 JIT: bootstrap, the DMA engine and its status bits, the mixbuf at
# X:0x1400, P changes made through the window. Needs the archive as
# apu_dsp_link does; n/a on a host without one.
#   cmake -S tests/apu_dsp_emu -B build/apu_dsp_emu
#   cmake --build build/apu_dsp_emu && ctest --test-dir build/apu_dsp_emu
cmake_minimum_required(VERSION 3.20)
project(apu_dsp_emu_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/dsp56300.cmake)
xboxrecomp_dsp56300(dsp_target dsp_why)
if(NOT dsp_target)
    message(FATAL_ERROR "${dsp_why}")
endif()

set(apu_dir ${CMAKE_CURRENT_LIST_DIR}/../../src/apu)
add_executable(apu_dsp_emu_test test_main.c ${apu_dir}/apu_dsp_emu.c)
target_include_directories(apu_dsp_emu_test PRIVATE ${apu_dir})
target_link_libraries(apu_dsp_emu_test PRIVATE ${dsp_target})
if(NOT MSVC)
    target_compile_options(apu_dsp_emu_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME apu_dsp_emu COMMAND apu_dsp_emu_test)
