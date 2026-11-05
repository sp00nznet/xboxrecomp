# nv2a_clear_color - SET_COLOR_CLEAR_VALUE in the colour surface's format
# (nv2a_backend_common.h): a 16-bit RGB surface's value arrives packed, and
# the GPU backends widen it to A8R8G8B8 (JSRF's R5G6B5 logos). Standalone:
#   cmake -S tests/nv2a_clear_color -B build/nv2a_clear_color
#   cmake --build build/nv2a_clear_color && ctest --test-dir build/nv2a_clear_color
cmake_minimum_required(VERSION 3.20)
project(nv2a_clear_color_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_clear_color_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(nv2a_clear_color_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(nv2a_clear_color_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(nv2a_clear_color_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_clear_color COMMAND nv2a_clear_color_test)
