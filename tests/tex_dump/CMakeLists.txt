# tex_dump - the runtime texture dump (src/kernel/nv2a_tex_dump.c): synthetic
# uploads into a temporary directory write <hash>.xtd and <hash>.json once
# per hash, repeats are counted, a linear texture gets its sidecar only, an
# address past the volatile limit stops writing, render-target binds and
# oversize uploads are counted. Pure C, no device:
#   cmake -S tests/tex_dump -B build/tex_dump
#   cmake --build build/tex_dump && ctest --test-dir build/tex_dump
cmake_minimum_required(VERSION 3.20)
project(tex_dump_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(tex_dump_test test_main.c
    ${XR_SRC}/kernel/nv2a_tex_dump.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exit_hook.c)
target_include_directories(tex_dump_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}/d3d
    ${XR_SRC}/platform ${XR_SRC})
if(NOT MSVC)
    target_compile_options(tex_dump_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(tex_dump_test PRIVATE m Threads::Threads)
endif()
enable_testing()
add_test(NAME tex_dump COMMAND tex_dump_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
