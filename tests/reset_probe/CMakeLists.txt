cmake_minimum_required(VERSION 3.20)
project(reset_probe_test C)
# Cross-platform: the stop probe and the guest CPU it hooks are the same
# code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(reset_probe_test test_main.c)
target_include_directories(reset_probe_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(reset_probe_test PRIVATE xboxrecomp)
if(WIN32)
    target_link_libraries(reset_probe_test PRIVATE winmm)
endif()
enable_testing()
add_test(NAME reset_probe_test COMMAND reset_probe_test)
