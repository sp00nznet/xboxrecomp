# apu_sdl_quit - the SDL presenter's window-close teardown
# (src/video/present_sdl_quit.h) while a thread feeds and queries the audio
# stream the way the APU frame thread does (src/apu/apu_sdl.c). The stream
# lives on SDL's disk driver, written to /dev/null: a real SDL stream drained
# in real time, unlike the dummy driver's virtual clock, and no sound. Before
# the fix the presenter called SDL_Quit, which freed the stream under the
# thread and crashed in SDL_GetAudioStreamQueued. POSIX only:
#   cmake -S tests/apu_sdl_quit -B build/apu_sdl_quit
#   cmake --build build/apu_sdl_quit && ctest --test-dir build/apu_sdl_quit
cmake_minimum_required(VERSION 3.20)
project(apu_sdl_quit_test C)
if(WIN32)
    return()
endif()
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
find_package(SDL3 3.2 CONFIG REQUIRED)
find_package(Threads REQUIRED)
add_executable(apu_sdl_quit_test
    test_main.c
    ${XR_SRC}/apu/apu_sdl.c
    ${XR_SRC}/apu/apu_vclock.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(apu_sdl_quit_test PRIVATE ${XR_SRC}/apu ${XR_SRC}/video
    ${XR_SRC}/platform ${XR_SRC})
target_compile_options(apu_sdl_quit_test PRIVATE -Wall -Wextra)
target_link_libraries(apu_sdl_quit_test PRIVATE SDL3::SDL3 Threads::Threads m)
enable_testing()
add_test(NAME apu_sdl_quit COMMAND apu_sdl_quit_test)
set_tests_properties(apu_sdl_quit PROPERTIES
    SKIP_RETURN_CODE 77
    ENVIRONMENT "SDL_VIDEO_DRIVER=dummy;SDL_AUDIO_DRIVER=disk;SDL_AUDIO_DISK_OUTPUT_FILE=/dev/null")
