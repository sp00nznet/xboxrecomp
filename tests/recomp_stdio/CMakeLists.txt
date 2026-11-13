cmake_minimum_required(VERSION 3.20)
project(recomp_stdio_test C)
# The stdio buffering (src/platform/recomp_stdio.c): the policy (a buffer only
# on Windows, only off a console), that a buffered stream holds its lines
# until recomp_stdio_flush writes them, and that an unbuffered one writes at
# once and its flush is a no-op.
set(CMAKE_C_STANDARD 11)
set(P ../../src/platform)
add_executable(recomp_stdio_test test_main.c ${P}/recomp_stdio.c)
target_include_directories(recomp_stdio_test PRIVATE ${P} ../../src)
enable_testing()
add_test(NAME recomp_stdio_test COMMAND recomp_stdio_test)
