cmake_minimum_required(VERSION 3.20)
project(recomp_fatal_test C)
# The [FATAL] line (src/platform/recomp_fatal.c): its formatter and name
# tables on every host, and on POSIX that abort(), SIGILL and SIGFPE each
# print the line once to the log's descriptor and still end the process by
# the same signal, and that a handler installed first keeps its signal.
set(CMAKE_C_STANDARD 11)
set(P ../../src/platform)
add_executable(recomp_fatal_test test_main.c ${P}/recomp_fatal.c)
target_include_directories(recomp_fatal_test PRIVATE ${P} ../../src)
enable_testing()
add_test(NAME recomp_fatal_test COMMAND recomp_fatal_test)
