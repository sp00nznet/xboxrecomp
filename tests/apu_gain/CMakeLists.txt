cmake_minimum_required(VERSION 3.20)
project(apu_gain_test C)
set(CMAKE_C_STANDARD 11)
# apu_gain.c has no dependencies: compile it in and feed it sample blocks.
add_executable(apu_gain_test test_main.c ../../src/apu/apu_gain.c)
target_include_directories(apu_gain_test PRIVATE ../../src/apu)
if(NOT WIN32)
    target_link_libraries(apu_gain_test PRIVATE m)
endif()
enable_testing()

# 100% (the default) leaves every sample bit-identical.
add_test(NAME apu_gain_unity COMMAND apu_gain_test unity)
# The level clamps to 0..100; 0 is silence; the curve is the square.
add_test(NAME apu_gain_clamp COMMAND apu_gain_test clamp)
# Scaling: monotonic in the level, symmetric, full scale stays in int16.
add_test(NAME apu_gain_scale COMMAND apu_gain_test scale)
