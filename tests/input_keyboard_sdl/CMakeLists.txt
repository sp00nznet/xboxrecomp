cmake_minimum_required(VERSION 3.20)
project(input_keyboard_sdl_regression C)
# The SDL window's key and focus rules (src/input/keyboard_sdl.c) driven
# with synthetic events, POSIX only. Links the two key files and SDL2, not
# the presenter; SDL runs on its dummy video driver, so no display is needed.
if(WIN32)
    message(FATAL_ERROR "The SDL keyboard path is POSIX-only")
endif()
set(CMAKE_C_STANDARD 11)
find_package(SDL2 REQUIRED)
enable_testing()
add_executable(input_keyboard_sdl_test test_main.c
    ../../src/input/keyboard.c ../../src/input/keyboard_sdl.c
    ../../src/input/input_map.c)
target_include_directories(input_keyboard_sdl_test PRIVATE ../../src/input ../../src)
target_link_libraries(input_keyboard_sdl_test PRIVATE SDL2::SDL2 m)
add_test(NAME input_keyboard_sdl COMMAND input_keyboard_sdl_test)
set_tests_properties(input_keyboard_sdl PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
