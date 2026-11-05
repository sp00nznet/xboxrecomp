cmake_minimum_required(VERSION 3.20)
project(apu_dsp_boot_test C)
# The stub's boot ROM copy: GPRST/EPRST going to 3 in the MCPX window copies
# the start of scratch into P, as KUF: Heroes' DirectSound checks.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(apu_dsp_boot_test test_main.c)
target_include_directories(apu_dsp_boot_test PRIVATE ../../src ../../src/apu)
target_link_libraries(apu_dsp_boot_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_dsp_boot COMMAND apu_dsp_boot_test)
