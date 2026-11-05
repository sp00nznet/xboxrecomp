cmake_minimum_required(VERSION 3.20)
project(apu_irq_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_irq_test test_main.c)
target_include_directories(apu_irq_test PRIVATE ../../src ../../src/apu ../../src/nv2a)
# The whole runtime, as apu_mixdown: the APU delivers its interrupt through
# the kernel. This tests the device half only (ISTS, GINTSTS, the trap).
target_link_libraries(apu_irq_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_irq COMMAND apu_irq_test)

# The stub's path under test: in a build with the APU DSP the JIT is the
# run-time default, so every test here asks for the stub.
get_property(_stub_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_stub_tests} APPEND PROPERTY ENVIRONMENT RECOMP_APU_DSP=0)
