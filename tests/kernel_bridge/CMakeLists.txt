cmake_minimum_required(VERSION 3.20)
project(kernel_bridge_test C)
# Cross-platform, unlike the _posix projects beside it: nothing here is
# POSIX-specific, and the bridge is the same code on both hosts. The one thing
# that differs is xbox_kernel_bridge_init()'s VirtualProtect over the thunk
# region -- real on Windows, shimmed elsewhere -- which is exactly why it is
# worth running in both places.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_bridge_test test_main.c)
target_include_directories(kernel_bridge_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_bridge_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_bridge_test COMMAND kernel_bridge_test)
