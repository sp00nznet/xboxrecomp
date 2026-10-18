# host_opts_live - live host options (nv2a_backend_common): a request from
# another thread applied at a fake flip, the changed-field mask, the options
# and pack generations, merging and no-op requests. POSIX (pthreads):
#   cmake -S tests/host_opts_live -B build/host_opts_live
#   cmake --build build/host_opts_live && ctest --test-dir build/host_opts_live
cmake_minimum_required(VERSION 3.20)
project(host_opts_live_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(host_opts_live_test test_main.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c)
target_include_directories(host_opts_live_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}/d3d)
if(NOT MSVC)
    target_compile_options(host_opts_live_test PRIVATE -Wall -Wextra)
endif()
target_link_libraries(host_opts_live_test PRIVATE Threads::Threads)
if(NOT WIN32)
    target_link_libraries(host_opts_live_test PRIVATE m)
endif()
enable_testing()
add_test(NAME host_opts_live COMMAND host_opts_live_test)
