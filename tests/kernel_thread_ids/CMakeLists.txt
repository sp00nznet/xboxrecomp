cmake_minimum_required(VERSION 3.20)
project(kernel_thread_ids_test C)
# Cross-platform, like tests/memory_regressions: the bridge's thread creation
# and the TIB are the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_thread_ids_test test_main.c)
target_include_directories(kernel_thread_ids_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_thread_ids_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_thread_ids_test
         COMMAND kernel_thread_ids_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME kernel_thread_ids_spawn_test
         COMMAND kernel_thread_ids_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe spawn)
