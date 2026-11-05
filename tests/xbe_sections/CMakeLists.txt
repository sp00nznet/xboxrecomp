cmake_minimum_required(VERSION 3.20)
project(xbe_sections_test C)
# XBE section paging (src/kernel/xbe_sections.c): refcounts, the fresh copy on
# 0 -> 1, shared-page counts and the resident tally. The module touches guest
# memory only through the pointer it is given, so this links it alone and
# runs on every host.
set(CMAKE_C_STANDARD 11)
add_executable(xbe_sections_test test_main.c ../../src/kernel/xbe_sections.c)
target_include_directories(xbe_sections_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME xbe_sections_test COMMAND xbe_sections_test)
