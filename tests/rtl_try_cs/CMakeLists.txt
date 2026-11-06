cmake_minimum_required(VERSION 3.20)
project(rtl_try_cs_test C)
# Cross-platform: the bridge and the critical-section shadows are the same
# code on both hosts.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(rtl_try_cs_test test_main.c)
target_include_directories(rtl_try_cs_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(rtl_try_cs_test PRIVATE xboxrecomp)
if(WIN32)
    target_link_libraries(rtl_try_cs_test PRIVATE winmm)
endif()
enable_testing()
add_test(NAME rtl_try_cs_test COMMAND rtl_try_cs_test)
