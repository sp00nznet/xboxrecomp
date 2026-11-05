cmake_minimum_required(VERSION 3.20)
project(nv2a_tex_test C)
set(CMAKE_C_STANDARD 11)
# The shared texture helpers (nv2a_backend_common), with the format tables
# they read. Standalone: cmake -S tests/nv2a_tex -B build && ctest.
add_executable(nv2a_tex_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(nv2a_tex_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT WIN32)
    target_link_libraries(nv2a_tex_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_tex_test COMMAND nv2a_tex_test)
