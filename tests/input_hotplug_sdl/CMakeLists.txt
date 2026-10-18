# input_hotplug_sdl - SDL game controller hot-plug (src/input/xinput_device.c,
# the SDL backend) through the public API (src/input/input_core.c), with
# SDL's virtual joysticks: attach, detach and reattach; the port each pad
# gets, the connected mask with its insert/remove masks,
# ERROR_DEVICE_NOT_CONNECTED after a removal, and polls racing hot-plug on
# another thread. POSIX only; no window, no sound, no real pad (the host
# joystick drivers SDL has hints for are turned off; Linux evdev has none, so
# the test skips if a host joystick still shows, or when SDL is older than
# 2.24 or has no virtual joystick driver):
#   cmake --build <build> --target input_hotplug_sdl_test && ctest -R input_hotplug_sdl

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
find_package(SDL2 REQUIRED)
add_executable(input_hotplug_sdl_test
    test_main.c
    ${INPUT_SRC}/xinput_device.c
    ${INPUT_SRC}/input_core.c
    ${INPUT_SRC}/input_map.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/kbm_script.c)
target_include_directories(input_hotplug_sdl_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_hotplug_sdl_test PRIVATE platform SDL2::SDL2 m)
enable_testing()
add_test(NAME input_hotplug_sdl COMMAND input_hotplug_sdl_test)
set_tests_properties(input_hotplug_sdl PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SDL_VIDEODRIVER=dummy;SDL_AUDIODRIVER=dummy;SDL_JOYSTICK_HIDAPI=0;SDL_JOYSTICK_IOKIT=0;SDL_JOYSTICK_MFI=0")
