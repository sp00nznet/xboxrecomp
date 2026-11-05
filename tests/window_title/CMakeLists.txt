# window_title - the host window's title bar rules (src/video/window_title.c):
# the game's name by default, FPS and draws once a second only with
# RECOMP_TRACE=title, and no rewrite while the name is unchanged. Standalone:
#   cmake -S tests/window_title -B build/window_title
#   cmake --build build/window_title && ctest --test-dir build/window_title
cmake_minimum_required(VERSION 3.20)
project(window_title_test C)
set(CMAKE_C_STANDARD 11)
add_executable(window_title_test test_main.c ../../src/video/window_title.c)
target_include_directories(window_title_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/video)
if(NOT MSVC)
    target_compile_options(window_title_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME window_title COMMAND window_title_test)
