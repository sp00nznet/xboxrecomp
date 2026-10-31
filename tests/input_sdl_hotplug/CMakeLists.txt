cmake_minimum_required(VERSION 3.20)
project(input_sdl_hotplug_regression C)

if(WIN32)
    message(FATAL_ERROR "The SDL controller path is POSIX-only")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)
enable_testing()

add_executable(input_sdl_hotplug_test
    test_main.c
    ../../src/input/xinput_device.c)
target_include_directories(input_sdl_hotplug_test PRIVATE
    fake_sdl
    ../../src/input
    ../../src)
target_link_libraries(input_sdl_hotplug_test PRIVATE Threads::Threads)

add_test(NAME input_sdl_hotplug COMMAND input_sdl_hotplug_test)
