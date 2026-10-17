cmake_minimum_required(VERSION 3.20)
project(kernel_guest_cpu_test C)
# Cross-platform: the core choice is the same code on both hosts; the pin
# itself is checked where the host has thread affinity (Linux).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_guest_cpu_test test_main.c)
target_include_directories(kernel_guest_cpu_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(kernel_guest_cpu_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_guest_cpu_test COMMAND kernel_guest_cpu_test)
