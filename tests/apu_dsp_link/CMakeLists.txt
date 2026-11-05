# apu_dsp_link - the pinned dsp56300 archive (cmake/dsp56300.cmake) links on
# this host and its JIT runs: what XBOXRECOMP_APU_DSP=ON adds to the game's
# link. It downloads the archive (about 10-12 MB) into the build dir, or takes
# -DXBOXRECOMP_DSP56300_ARCHIVE=<tarball>; n/a on a host without an archive
# or without the network. Cross builds (llvm-mingw) configure and build it
# with the same toolchain file as the game, to check the link only:
#   cmake -S tests/apu_dsp_link -B build/apu_dsp_link
#   cmake --build build/apu_dsp_link && ctest --test-dir build/apu_dsp_link
cmake_minimum_required(VERSION 3.20)
project(apu_dsp_link_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/dsp56300.cmake)
xboxrecomp_dsp56300(dsp_target dsp_why)
if(NOT dsp_target)
    message(FATAL_ERROR "${dsp_why}")
endif()

add_executable(apu_dsp_link_test test_main.c)
target_link_libraries(apu_dsp_link_test PRIVATE ${dsp_target})
if(NOT MSVC)
    target_compile_options(apu_dsp_link_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME apu_dsp_link COMMAND apu_dsp_link_test)
