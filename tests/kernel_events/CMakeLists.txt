cmake_minimum_required(VERSION 3.20)
project(kernel_events_test C)
# Windows only: the event bridges wrap Win32 events.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_events_test test_main.c)
target_include_directories(kernel_events_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_events_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switches are read once and cached, so they cannot be
# flipped inside a run.
add_test(NAME kernel_events_default COMMAND kernel_events_test default ${XBE})
add_test(NAME kernel_events_title_kevents COMMAND kernel_events_test title-kevents ${XBE})
