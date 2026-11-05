# rt_alias - render-target ownership in nv2a_backend_common: a GPU backend's
# target goes stale when the title rewrites its guest memory, and is then
# neither sampled nor written back (openspec rt-stale-alias). A buffer stands
# in for guest memory. Standalone:
#   cmake -S tests/rt_alias -B build/rt_alias
#   cmake --build build/rt_alias && ctest --test-dir build/rt_alias
cmake_minimum_required(VERSION 3.20)
project(rt_alias_test C)
set(CMAKE_C_STANDARD 11)
add_executable(rt_alias_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(rt_alias_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(rt_alias_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(rt_alias_test PRIVATE m)
endif()
enable_testing()
add_test(NAME rt_alias COMMAND rt_alias_test)
