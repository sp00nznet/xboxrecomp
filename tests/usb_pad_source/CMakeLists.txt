cmake_minimum_required(VERSION 3.20)
project(usb_pad_source_test C)
# The emulated pad's game-source hook (usb_gamepad.h): the report a source
# fills, the pads it keeps plugged, its rumble, and the root hub plugging and
# unplugging pads to follow it. No guest memory and no trap: the model's
# functions are called directly, so this runs on every host.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)

add_executable(usb_pad_source_test test_main.c)
target_include_directories(usb_pad_source_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input)
target_link_libraries(usb_pad_source_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_pad_source_test COMMAND usb_pad_source_test)
