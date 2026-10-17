cmake_minimum_required(VERSION 3.20)
project(apu_monitor_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_monitor_test test_main.c)
target_include_directories(apu_monitor_test PRIVATE ../../src ../../src/apu)
# The whole runtime, as apu_mixdown: the APU delivers its interrupt and
# resolves physical addresses through the kernel. test_main.c defines every
# xa2_* function, so the archive's apu_xaudio2.o is never pulled in and the
# monitor submits to the fake device below.
target_link_libraries(apu_monitor_test PRIVATE xboxrecomp)
enable_testing()
# The voice trace (and the host accounting the monitor runs with it) is on
# only under RECOMP_APU_TRACE, the ring monitor only under apu_ring: the
# plain run checks both stay silent, and so does the run with the switch
# and both trace-list keys =0. The argument says which the test should
# expect, so the check does not come from the reader under test.
add_test(NAME apu_monitor COMMAND apu_monitor_test on)
add_test(NAME apu_monitor_quiet COMMAND apu_monitor_test unset)
add_test(NAME apu_monitor_off COMMAND apu_monitor_test off)
set_tests_properties(apu_monitor PROPERTIES ENVIRONMENT
                     "RECOMP_APU_TRACE=1;RECOMP_TRACE=apu_ring,audio_host")
set_tests_properties(apu_monitor_off PROPERTIES ENVIRONMENT
                     "RECOMP_APU_TRACE=0;RECOMP_TRACE=apu_ring=0,audio_host=0")
