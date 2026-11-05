cmake_minimum_required(VERSION 3.20)
project(guest_unwind_test C)
# Cross-platform: the unwind and the guest CPU, ring and waits it reaches
# into are the same code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_unwind_test test_main.c)
target_include_directories(guest_unwind_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_unwind_test PRIVATE xboxrecomp)
if(WIN32)
    target_link_libraries(guest_unwind_test PRIVATE winmm)
endif()
enable_testing()
add_test(NAME guest_unwind_test COMMAND guest_unwind_test)
