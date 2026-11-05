cmake_minimum_required(VERSION 3.20)
project(usb_pad_game_test C)
# The pad script and the pad record for a game that answers XInputGetState
# itself (usb_gamepad.h, usb_gamepad_game_*): the script presses on the
# game's state, the record writes it, and the record replays as a script to
# the same states. No guest memory and no USB transfer: the functions are
# called directly, so this runs on every host.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)

add_executable(usb_pad_game_test test_main.c)
target_include_directories(usb_pad_game_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_game_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_pad_game_test COMMAND usb_pad_game_test)
