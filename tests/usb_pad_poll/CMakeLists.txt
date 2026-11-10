cmake_minimum_required(VERSION 3.20)
project(usb_pad_poll_test C)
# The poll clock (RECOMP_DEBUG=pad_clock=poll, g<poll> steps) behind a game's
# own XInputGetState, and RECOMP_DEBUG=pad_quit. The run quits the process,
# so the test forks it: POSIX only.
include(${CMAKE_CURRENT_LIST_DIR}/../xr_test.cmake)
if(WIN32)
    xr_test_refuse("usb_pad_poll forks its run: POSIX hosts only")
    return()
endif()
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()

add_executable(usb_pad_poll_test test_main.c)
target_include_directories(usb_pad_poll_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_poll_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_pad_poll_test COMMAND usb_pad_poll_test)
