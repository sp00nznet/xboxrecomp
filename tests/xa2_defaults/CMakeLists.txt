cmake_minimum_required(VERSION 3.20)
project(xa2_defaults_test C)
set(CMAKE_C_STANDARD 11)
# The device defaults, the priority parse and the pacing marks are inline in
# apu_xaudio2.h, so they are checked here on every host; tests/xaudio2 needs
# XAudio2 itself and runs only on Windows or under Proton.
add_executable(xa2_defaults_test test_main.c)
target_include_directories(xa2_defaults_test PRIVATE ../../src/apu)
enable_testing()
add_test(NAME xa2_defaults COMMAND xa2_defaults_test)
