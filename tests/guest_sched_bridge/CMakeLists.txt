cmake_minimum_required(VERSION 3.20)
project(guest_sched_bridge_test C)
# PsCreateSystemThreadEx's CreateSuspended with guest_sched, through the thunk
# dispatcher against a mapped guest window (the synthetic XBE): the guest CPU
# holds the new thread at its join until NtResumeThread on its handle.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_sched_bridge_test test_main.c)
target_include_directories(guest_sched_bridge_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_sched_bridge_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_sched_bridge
         COMMAND guest_sched_bridge_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
