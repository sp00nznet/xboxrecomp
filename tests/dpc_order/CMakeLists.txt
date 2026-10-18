cmake_minimum_required(VERSION 3.20)
project(dpc_order_test C)
# Cross-platform: the DPC queue, the one-CPU gate and the timer wake are the
# same code on both hosts, over each host's critical sections and events.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(dpc_order_test test_main.c)
target_include_directories(dpc_order_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(dpc_order_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME dpc_order_test COMMAND dpc_order_test)
