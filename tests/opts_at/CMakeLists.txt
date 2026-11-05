# opts_at - the RECOMP_DEBUG=opts_at parser (nv2a_opts_at.c): good entries,
# a bad flip, an unknown key, an empty value and the 32-entry cap.
#   cmake -S tests/opts_at -B build/opts_at
#   cmake --build build/opts_at && ctest --test-dir build/opts_at
cmake_minimum_required(VERSION 3.20)
project(opts_at_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(opts_at_test test_main.c ${XR_SRC}/kernel/nv2a_opts_at.c)
target_include_directories(opts_at_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}/d3d)
if(NOT MSVC)
    target_compile_options(opts_at_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME opts_at COMMAND opts_at_test)
