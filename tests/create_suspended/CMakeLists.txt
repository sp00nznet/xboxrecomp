cmake_minimum_required(VERSION 3.20)
project(create_suspended_test C)
# PsCreateSystemThreadEx's CreateSuspended without guest_sched, through the
# thunk dispatcher against a mapped guest window (the synthetic XBE): the new
# thread waits at its start gate until NtResumeThread on its handle, with the
# guest lock off and on.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(create_suspended_test test_main.c)
target_include_directories(create_suspended_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(create_suspended_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME create_suspended
         COMMAND create_suspended_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
add_test(NAME create_suspended_lock
         COMMAND create_suspended_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe lock)
