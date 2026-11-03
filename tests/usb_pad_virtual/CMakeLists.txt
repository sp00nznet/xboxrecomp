cmake_minimum_required(VERSION 3.20)
project(usb_pad_virtual_test C)
# Millisecond pad script steps on the virtual clock (RECOMP_DEBUG=
# deterministic): the script's clock starts at the first poll even when that
# poll reads virtual 0 ms, and a step lands on the first vblank at or past
# its time. Driven through a game's own XInputGetState tap
# (usb_gamepad_game_press) with the clock stepped by hand, so no guest memory
# and no USB transfer: this runs on every host.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)

add_executable(usb_pad_virtual_test test_main.c)
target_include_directories(usb_pad_virtual_test PRIVATE
    ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
target_link_libraries(usb_pad_virtual_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME usb_pad_virtual_test COMMAND usb_pad_virtual_test)
