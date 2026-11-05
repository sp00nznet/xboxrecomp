cmake_minimum_required(VERSION 3.20)
project(apu_lifecycle_test C)
# mcpx_apu_init_standalone / mcpx_apu_shutdown more than once in a process:
# the DSP doorbell gate and its configuration start fresh on every init, and
# shutdown releases the GP/EP state once, after the frame thread has stopped.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_lifecycle_test test_main.c)
target_include_directories(apu_lifecycle_test PRIVATE ../../src ../../src/apu)
# test_main.c defines every xa2_* function, so no host audio device is opened.
target_link_libraries(apu_lifecycle_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_lifecycle COMMAND apu_lifecycle_test)

# The stub's path under test: in a build with the APU DSP the JIT is the
# run-time default, so every test here asks for the stub.
get_property(_stub_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_stub_tests} APPEND PROPERTY ENVIRONMENT RECOMP_APU_DSP=0)
