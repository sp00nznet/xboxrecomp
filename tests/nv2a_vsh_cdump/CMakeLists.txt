# nv2a_vsh_cdump - RECOMP_DEBUG=vsh_const_dump's flip/program filter and its
# record layout (src/kernel/nv2a_vsh_cdump.c). Cross-platform.
# Standalone:
#   cmake -S tests/nv2a_vsh_cdump -B build/nv2a_vsh_cdump
#   cmake --build build/nv2a_vsh_cdump && ctest --test-dir build/nv2a_vsh_cdump
cmake_minimum_required(VERSION 3.20)
project(nv2a_vsh_cdump_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_vsh_cdump_test test_main.c ../../src/kernel/nv2a_vsh_cdump.c)
target_include_directories(nv2a_vsh_cdump_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel)
if(NOT MSVC)
    target_compile_options(nv2a_vsh_cdump_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME nv2a_vsh_cdump COMMAND nv2a_vsh_cdump_test)
