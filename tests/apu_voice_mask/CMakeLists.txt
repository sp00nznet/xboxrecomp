cmake_minimum_required(VERSION 3.20)
project(apu_voice_mask_test C)
# A front-end method from the title's thread and the frame thread's voice
# processing change bits of the same voice word without losing either write.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_voice_mask_test test_main.c)
target_include_directories(apu_voice_mask_test PRIVATE ../../src ../../src/apu ../../src/nv2a)
# test_main.c defines every xa2_* function, so no host audio device is opened.
target_link_libraries(apu_voice_mask_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_voice_mask COMMAND apu_voice_mask_test)
