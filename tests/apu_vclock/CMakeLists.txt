cmake_minimum_required(VERSION 3.20)
project(apu_vclock_test C)
set(CMAKE_C_STANDARD 11)
# apu_vclock.c is self-contained and takes the time as an argument; compile
# it in and drive it with a fake clock.
add_executable(apu_vclock_test test_main.c ../../src/apu/apu_vclock.c)
target_include_directories(apu_vclock_test PRIVATE ../../src/apu)
enable_testing()

# The APU's pacing loop (low/high marks) against the clock: 48000 frames a
# second exactly, over a fake hour.
add_test(NAME apu_vclock_rate COMMAND apu_vclock_test rate)
# Running dry plays silence; the next submit starts at once.
add_test(NAME apu_vclock_dry COMMAND apu_vclock_test dry)
# Positions between whole frames, time standing still, and a long dt.
add_test(NAME apu_vclock_edges COMMAND apu_vclock_test edges)
