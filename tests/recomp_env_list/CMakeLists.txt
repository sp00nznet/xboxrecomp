# recomp_env_list - RECOMP_TRACE/RECOMP_DEBUG list parsing
# (src/platform/recomp_env.c): values with commas in them, unknown keys, and
# the comma-key table against the key table. Pure C, cross-platform:
#   cmake -S tests/recomp_env_list -B build/recomp_env_list
#   cmake --build build/recomp_env_list && ctest --test-dir build/recomp_env_list
cmake_minimum_required(VERSION 3.20)
project(recomp_env_list_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
# test_main.c includes recomp_env.c itself, for its static tables.
add_executable(recomp_env_list_test test_main.c)
target_include_directories(recomp_env_list_test PRIVATE ${XR_SRC}/platform)
if(NOT MSVC)
    target_compile_options(recomp_env_list_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME recomp_env_list COMMAND recomp_env_list_test)
