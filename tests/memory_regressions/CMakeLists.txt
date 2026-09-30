cmake_minimum_required(VERSION 3.20)
project(memory_regressions_test C)
# Cross-platform, like tests/kernel_bridge: the heap and the memory bridge are
# the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(memory_regressions_test test_main.c)
target_include_directories(memory_regressions_test PRIVATE ../../src/kernel)
target_link_libraries(memory_regressions_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switches are read once and cached, so they cannot be
# flipped inside a run.
add_test(NAME memory_regressions_default COMMAND memory_regressions_test default ${XBE})
add_test(NAME memory_regressions_heap_reclaim COMMAND memory_regressions_test heap-reclaim ${XBE})
add_test(NAME memory_regressions_ext_vma COMMAND memory_regressions_test ext-vma ${XBE})
add_test(NAME memory_regressions_ext_vma_128 COMMAND memory_regressions_test ext-vma-128 ${XBE})
