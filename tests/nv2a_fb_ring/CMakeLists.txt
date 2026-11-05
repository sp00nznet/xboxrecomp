# nv2a_fb_ring - RECOMP_DEBUG=fb_dump_prog's pre-trigger ring and trigger
# range (src/kernel/nv2a_fb_ring.c). Cross-platform.
# Standalone:
#   cmake -S tests/nv2a_fb_ring -B build/nv2a_fb_ring
#   cmake --build build/nv2a_fb_ring && ctest --test-dir build/nv2a_fb_ring
cmake_minimum_required(VERSION 3.20)
project(nv2a_fb_ring_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_fb_ring_test test_main.c ../../src/kernel/nv2a_fb_ring.c)
target_include_directories(nv2a_fb_ring_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
if(NOT MSVC)
    target_compile_options(nv2a_fb_ring_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME nv2a_fb_ring COMMAND nv2a_fb_ring_test)
