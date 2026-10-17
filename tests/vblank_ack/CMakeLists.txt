cmake_minimum_required(VERSION 3.20)
project(vblank_ack_test C)
# Cross-platform, as tests/kernel_bridge: the ack wait is the same code on
# both hosts, and it is timing, which the two hosts' wait primitives differ in.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(vblank_ack_test test_main.c)
target_include_directories(vblank_ack_test PRIVATE ../../src/kernel)
target_link_libraries(vblank_ack_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME vblank_ack_test COMMAND vblank_ack_test)
