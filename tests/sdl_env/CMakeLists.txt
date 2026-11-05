# sdl_env - the SDL2 -> SDL3 driver variable aliases (src/platform/sdl_env.h):
# an old name is copied when the new one is unset, and a new name already set
# wins. POSIX only (Windows links no SDL). Standalone:
#   cmake -S tests/sdl_env -B build/sdl_env
#   cmake --build build/sdl_env && ctest --test-dir build/sdl_env
cmake_minimum_required(VERSION 3.20)
project(sdl_env_test C)
if(WIN32)
    return()
endif()
set(CMAKE_C_STANDARD 11)
find_package(SDL3 3.2 CONFIG REQUIRED)
add_executable(sdl_env_test test_main.c)
target_include_directories(sdl_env_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/platform)
target_link_libraries(sdl_env_test PRIVATE SDL3::SDL3)
target_compile_options(sdl_env_test PRIVATE -Wall -Wextra)
enable_testing()
add_test(NAME sdl_env COMMAND sdl_env_test)
