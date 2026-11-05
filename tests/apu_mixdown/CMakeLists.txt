cmake_minimum_required(VERSION 3.20)
project(apu_mixdown_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_mixdown_test test_main.c)
target_include_directories(apu_mixdown_test PRIVATE ../../src ../../src/apu)
# The whole runtime: the APU delivers its interrupt and resolves physical
# addresses through the kernel.
target_link_libraries(apu_mixdown_test PRIVATE xboxrecomp)
enable_testing()
# Registered once per mode, on purpose. mcpx_apu_mixdown_mode caches its
# getenv in a static, so one process can only ever exercise one arm. The
# two-bin arm is the negative control: it must show the bins above 1 really
# are discarded without the wider mixdown, or the other arms prove nothing.
# The expectation is RECOMP_-prefixed so tests/proton_run.sh, which forwards
# only RECOMP_* and HLSL_* into Proton, passes it on.
add_test(NAME apu_mixdown_staged COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_staged PROPERTIES
                     ENVIRONMENT "RECOMP_DEBUG=;RECOMP_TEST_MIXDOWN_EXPECT=1")
add_test(NAME apu_mixdown_unity COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_unity PROPERTIES
                     ENVIRONMENT "RECOMP_DEBUG=apu_mixdown_all=2;RECOMP_TEST_MIXDOWN_EXPECT=2")
add_test(NAME apu_mixdown_two_bins COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_two_bins PROPERTIES
                     ENVIRONMENT "RECOMP_DEBUG=apu_mixdown_all=0;RECOMP_TEST_MIXDOWN_EXPECT=0")

# The stub's path under test: in a build with the APU DSP the JIT is the
# run-time default, so every test here asks for the stub.
get_property(_stub_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_stub_tests} APPEND PROPERTY ENVIRONMENT RECOMP_APU_DSP=0)
