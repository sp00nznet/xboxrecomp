cmake_minimum_required(VERSION 3.20)
project(usb_pad_anchor_test C)
# Pad anchors behind a game's own XInputGetState: a step after an anchor
# waits for usb_gamepad_file_open, a late open gets the anchor's retry taps,
# and a record with pad_anchor writes the anchor lines. The functions are
# called directly, so this runs on every host. Its own binary, since the
# runtime reads RECOMP_DEBUG once per process.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)

find_package(Threads REQUIRED)
enable_testing()
# test_main: the replay and the record. test_stock: nothing set, an open
# does nothing. test_threads: two threads' opens all matched.
foreach(t main stock threads)
    add_executable(usb_pad_anchor_${t} test_${t}.c)
    target_include_directories(usb_pad_anchor_${t} PRIVATE
        ../../src/kernel ../../src/usb ../../src/input ../../src/platform)
    target_link_libraries(usb_pad_anchor_${t} PRIVATE xboxrecomp Threads::Threads)
    add_test(NAME usb_pad_anchor_${t} COMMAND usb_pad_anchor_${t})
endforeach()
