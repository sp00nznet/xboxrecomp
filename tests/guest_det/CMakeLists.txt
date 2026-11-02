cmake_minimum_required(VERSION 3.20)
project(guest_det_test C)
# Cross-platform: the deterministic scheduler is the same code on both
# hosts, over each host's critical sections and condition variables.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_det_test test_main.c)
target_include_directories(guest_det_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_det_test PRIVATE xboxrecomp)
if(WIN32)
    target_link_libraries(guest_det_test PRIVATE winmm)
endif()
enable_testing()
add_test(NAME guest_det_test COMMAND guest_det_test)
