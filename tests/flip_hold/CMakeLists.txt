cmake_minimum_required(VERSION 3.20)
project(flip_hold_test C)
# The flip hold's rule (nv2a_flip_hold.c) is pure: counts and times are
# arguments, so the test builds it alone, on every host.
set(CMAKE_C_STANDARD 11)
add_executable(flip_hold_test test_main.c ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(flip_hold_test PRIVATE ../../src/kernel)
enable_testing()
add_test(NAME flip_hold_test COMMAND flip_hold_test)
