cmake_minimum_required(VERSION 3.20)
project(guest_cpu_test C)
# Cross-platform: the guest CPU is the same code on both hosts, over each
# host's critical sections and condition variables.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_cpu_test test_main.c)
target_include_directories(guest_cpu_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_cpu_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_cpu_test COMMAND guest_cpu_test)
