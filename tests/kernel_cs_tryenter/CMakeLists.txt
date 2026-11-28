cmake_minimum_required(VERSION 3.20)
project(kernel_cs_tryenter_test C)
# A critical section taken with RtlTryEnterCriticalSection and released with
# RtlLeaveCriticalSection is free for the next thread, in each lock mode.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_cs_tryenter_test test_main.c)
target_include_directories(kernel_cs_tryenter_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_cs_tryenter_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: RECOMP_CS_MODE is read once.
add_test(NAME kernel_cs_tryenter_shadow COMMAND kernel_cs_tryenter_test shadow ${XBE})
add_test(NAME kernel_cs_tryenter_guest COMMAND kernel_cs_tryenter_test guest ${XBE})
set_tests_properties(kernel_cs_tryenter_shadow kernel_cs_tryenter_guest
                     PROPERTIES TIMEOUT 60)
