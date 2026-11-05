# present_crop - display.overscan's source rectangle (nv2a_present_crop in
# nv2a_backend_common) and what it hands nv2a_present_rect.
# Standalone:
#   cmake -S tests/present_crop -B build/present_crop
#   cmake --build build/present_crop && ctest --test-dir build/present_crop
cmake_minimum_required(VERSION 3.20)
project(present_crop_test C)
set(CMAKE_C_STANDARD 11)
add_executable(present_crop_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c
    ../../src/kernel/nv2a_aspect.c
    ../../src/kernel/nv2a_vsh_cpu.c)
target_include_directories(present_crop_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(present_crop_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(present_crop_test PRIVATE m)
endif()
enable_testing()
add_test(NAME present_crop COMMAND present_crop_test)
