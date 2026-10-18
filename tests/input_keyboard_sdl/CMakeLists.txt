# input_keyboard_sdl - the SDL window's key and focus rules
# (src/input/keyboard_sdl.c) driven with synthetic events, POSIX only.
# Links the two key files and SDL2, not the presenter; SDL is initialised
# with the dummy video driver, so no display is needed:
#   cmake --build <build> --target input_keyboard_sdl_test && ctest -R input_keyboard_sdl

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
find_package(SDL2 REQUIRED)
add_executable(input_keyboard_sdl_test
    test_main.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/kbm_script.c
    ${INPUT_SRC}/keyboard_sdl.c
    ${INPUT_SRC}/mouse_sdl.c)
target_include_directories(input_keyboard_sdl_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_keyboard_sdl_test PRIVATE platform SDL2::SDL2)
if(NOT WIN32)
    target_link_libraries(input_keyboard_sdl_test PRIVATE m)
endif()
enable_testing()
add_test(NAME input_keyboard_sdl COMMAND input_keyboard_sdl_test)
set_tests_properties(input_keyboard_sdl PROPERTIES
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy")
