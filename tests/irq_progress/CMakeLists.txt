cmake_minimum_required(VERSION 3.20)
project(irq_progress_test C)
# Cross-platform: interrupt delivery, the DPC drains and the guest CPU are the
# same code on both hosts, over each host's critical sections and events.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(irq_progress_test test_main.c)
target_include_directories(irq_progress_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(irq_progress_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME irq_progress_test COMMAND irq_progress_test)
