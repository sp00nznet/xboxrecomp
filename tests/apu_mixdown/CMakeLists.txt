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
# Registered TWICE, on purpose. mcpx_apu_mixdown_all caches its getenv in a
# static, so one process can only ever exercise one arm -- and an arm that
# cannot fail is not a test. The off arm is the negative control: it must show
# the effect bins really are discarded without this change, or the on arm
# proves nothing.
# The expectation is RECOMP_-prefixed so tests/proton_run.sh, which forwards
# only RECOMP_* and HLSL_* into Proton, passes it on.
add_test(NAME apu_mixdown_all COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_all PROPERTIES
                     ENVIRONMENT "RECOMP_DEBUG=apu_mixdown_all=1;RECOMP_TEST_MIXDOWN_EXPECT=1")
add_test(NAME apu_mixdown_two_bins COMMAND apu_mixdown_test)
set_tests_properties(apu_mixdown_two_bins PROPERTIES
                     ENVIRONMENT "RECOMP_DEBUG=apu_mixdown_all=0;RECOMP_TEST_MIXDOWN_EXPECT=0")
