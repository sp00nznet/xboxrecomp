cmake_minimum_required(VERSION 3.20)
project(guest_sched_test C)
# Cross-platform: the guest CPU's priority order is the same code on both
# hosts, over each host's critical sections and condition variables.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_sched_test test_main.c)
target_include_directories(guest_sched_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_sched_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_sched_test COMMAND guest_sched_test)
