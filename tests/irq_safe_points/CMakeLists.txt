cmake_minimum_required(VERSION 3.20)
project(irq_safe_points_test C)
# Cross-platform: the one-CPU gate and the interrupt post are the same code
# on both hosts, over each host's critical sections and events.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(irq_safe_points_test test_main.c)
target_include_directories(irq_safe_points_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(irq_safe_points_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME irq_safe_points_test COMMAND irq_safe_points_test)
