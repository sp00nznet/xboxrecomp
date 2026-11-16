cmake_minimum_required(VERSION 3.20)
project(apu_trace_test C)
# apu_trace: RECOMP_APU_TRACE lines go through a logger thread, and a burst
# past its buffer is dropped and counted rather than waited out.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_trace_test test_main.c)
target_include_directories(apu_trace_test PRIVATE ../../src ../../src/apu)
# test_main.c defines every xa2_* function, so no host audio device is opened.
target_link_libraries(apu_trace_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_trace COMMAND apu_trace_test)
