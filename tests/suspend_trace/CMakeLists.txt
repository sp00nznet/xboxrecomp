cmake_minimum_required(VERSION 3.20)
project(suspend_trace_test C)
# RECOMP_TRACE=suspend[=va]: NtSuspendThread and NtResumeThread
# through the thunk dispatcher against a mapped guest window (the synthetic
# XBE), on a running thread (the host path) and on one held at its start
# gate, then the [SUSPEND] line; and with the trace off, nothing counted.
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(suspend_trace_test test_main.c)
target_include_directories(suspend_trace_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(suspend_trace_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME suspend_trace
         COMMAND suspend_trace_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe on)
add_test(NAME suspend_trace_off
         COMMAND suspend_trace_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe off)
