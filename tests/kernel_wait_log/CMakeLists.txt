cmake_minimum_required(VERSION 3.20)
project(kernel_wait_log_test C)
# Cross-platform: the wait functions are called directly, so it covers the
# Win32 backend on Windows/Proton and the POSIX shims elsewhere.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_wait_log_test test_main.c)
target_include_directories(kernel_wait_log_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_wait_log_test PRIVATE xboxrecomp)
enable_testing()
# Off by default: the variable unset writes no line.
add_test(NAME kernel_wait_log_unset COMMAND kernel_wait_log_test)
# On: one line for each wait that timed out.
add_test(NAME kernel_wait_log_set COMMAND kernel_wait_log_test)
set_tests_properties(kernel_wait_log_set PROPERTIES ENVIRONMENT "RECOMP_WAIT_LOG=1")
