cmake_minimum_required(VERSION 3.20)
project(gr_notify_test C)
# The PGRAPH notify and the run-ahead's ack rule (nv2a_gr_notify.h) are
# pure functions over a register block, so the test builds them alone, on
# every host.
set(CMAKE_C_STANDARD 11)
add_executable(gr_notify_test test_main.c)
target_include_directories(gr_notify_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME gr_notify_test COMMAND gr_notify_test)
