cmake_minimum_required(VERSION 3.20)
project(kernel_events_test C)
include(${CMAKE_CURRENT_LIST_DIR}/../xr_test.cmake)
if(NOT WIN32)
    xr_test_refuse("This harness checks the Win32 event bridges; run it on Windows or under Proton.")
    return()
endif()
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(kernel_events_test test_main.c)
target_include_directories(kernel_events_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_events_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switches are read once and cached, so they cannot be
# flipped inside a run.
add_test(NAME kernel_events_default COMMAND kernel_events_test default ${XBE})
add_test(NAME kernel_events_title_kevents COMMAND kernel_events_test title-kevents ${XBE})
