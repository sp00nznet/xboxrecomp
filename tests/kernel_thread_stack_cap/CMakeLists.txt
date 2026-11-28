cmake_minimum_required(VERSION 3.20)
project(kernel_thread_stack_cap_test C)
# A worker past the stack cap stops the run with a message, instead of running
# inline on its caller (which deadlocks once the worker idles).
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_thread_stack_cap_test test_main.c)
target_include_directories(kernel_thread_stack_cap_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_thread_stack_cap_test PRIVATE xboxrecomp)
enable_testing()

add_test(NAME kernel_thread_stack_cap
         COMMAND kernel_thread_stack_cap_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The process ends inside the 33rd PsCreateSystemThreadEx, so the pass is the
# message, and anything printed after it is a failure.
set_tests_properties(kernel_thread_stack_cap PROPERTIES
    PASS_REGULAR_EXPRESSION "no stack for worker 0x00010000: cap reached \\(32 live, cap 32\\); stopping"
    FAIL_REGULAR_EXPRESSION "FAIL"
    TIMEOUT 60)
