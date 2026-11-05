cmake_minimum_required(VERSION 3.20)
project(apu_dsp_ack_test C)
# RECOMP_APU_DSP_ACK=auto: the doorbell found from GPSADDR, and held off while
# GPRST keeps the GP in reset. No title tested writes GPRST, so this is the
# only place the gate runs.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_dsp_ack_test test_main.c)
target_include_directories(apu_dsp_ack_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_dsp_ack_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_dsp_ack COMMAND apu_dsp_ack_test)
set_tests_properties(apu_dsp_ack PROPERTIES ENVIRONMENT "RECOMP_APU_DSP_ACK=auto")

# The stub's path under test: in a build with the APU DSP the JIT is the
# run-time default, so every test here asks for the stub.
get_property(_stub_tests DIRECTORY PROPERTY TESTS)
set_property(TEST ${_stub_tests} APPEND PROPERTY ENVIRONMENT RECOMP_APU_DSP=0)
