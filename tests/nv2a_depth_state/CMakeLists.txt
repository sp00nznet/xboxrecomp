# nv2a_depth_state - W-buffering and polygon offset as the GPU backends
# translate them (nv2a_backend_common.h): which batches take the W path,
# the host depth bias for the rest, and the W variant's fragment
# constants (clip range, cull or clamp, 1/zmax). Standalone:
#   cmake -S tests/nv2a_depth_state -B build/nv2a_depth_state
#   cmake --build build/nv2a_depth_state && ctest --test-dir build/nv2a_depth_state
cmake_minimum_required(VERSION 3.20)
project(nv2a_depth_state_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_depth_state_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(nv2a_depth_state_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(nv2a_depth_state_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(nv2a_depth_state_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_depth_state COMMAND nv2a_depth_state_test)
