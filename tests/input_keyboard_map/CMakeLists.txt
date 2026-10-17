cmake_minimum_required(VERSION 3.20)
project(input_keyboard_map_regression C)
# keyboard.c's key map, resolved by the mapping rules (src/input/input_map.c),
# against a frozen copy of v0.14.0's resolver, and the Win32 window's key and
# mouse message decoding. No SDL and no window, so it builds and runs on
# every host.
set(CMAKE_C_STANDARD 11)
enable_testing()
add_executable(input_keyboard_map_test test_main.c
    ../../src/input/keyboard.c ../../src/input/input_map.c)
target_include_directories(input_keyboard_map_test PRIVATE ../../src/input ../../src)
if(NOT MSVC)
    target_link_libraries(input_keyboard_map_test PRIVATE m)
endif()
add_test(NAME input_keyboard_map COMMAND input_keyboard_map_test)
