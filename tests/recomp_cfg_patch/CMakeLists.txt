# recomp_cfg_patch - the menu's Save writer (recomp_cfg_patch in
# src/enhance/recomp_cfg.c): differences-only edits, new keys and tables,
# removals, [input.actions] arrays and [mods] names, CRLF, and the refusals.
# Pure C, no device:
#   cmake -S tests/recomp_cfg_patch -B build/recomp_cfg_patch
#   cmake --build build/recomp_cfg_patch && ctest --test-dir build/recomp_cfg_patch
cmake_minimum_required(VERSION 3.20)
project(recomp_cfg_patch_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(recomp_cfg_patch_test test_main.c ${XR_SRC}/enhance/recomp_cfg.c)
target_include_directories(recomp_cfg_patch_test PRIVATE ${XR_SRC}/enhance)
if(NOT MSVC)
    target_compile_options(recomp_cfg_patch_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(recomp_cfg_patch_test PRIVATE m)
endif()
enable_testing()
add_test(NAME recomp_cfg_patch COMMAND recomp_cfg_patch_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
