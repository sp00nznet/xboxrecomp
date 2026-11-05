cmake_minimum_required(VERSION 3.20)
project(apu_dsp_warm_test C)
set(CMAKE_C_STANDARD 11)
# apu_dsp_warm.h is the warm-up schedule apu_dsp_run.c follows for the GP and
# EP after a bootstrap, apart from the worker threads and the JIT; drive it
# with a fake worker that takes a long first frame.
add_executable(apu_dsp_warm_test test_main.c)
target_include_directories(apu_dsp_warm_test PRIVATE ../../src/apu)
if(NOT MSVC)
    target_compile_options(apu_dsp_warm_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME apu_dsp_warm COMMAND apu_dsp_warm_test)
