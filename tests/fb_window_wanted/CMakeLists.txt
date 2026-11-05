# fb_window_wanted - when RECOMP_FB_WINDOW opens the GDI framebuffer window
# (src/video/fb_window_wanted.c): never without the key, and not beside the
# D3D11 backend, which presents in its own window (a second window showed the
# same frame twice). Cross-platform; the Proton standalone list runs it too.
# Standalone:
#   cmake -S tests/fb_window_wanted -B build/fb_window_wanted
#   cmake --build build/fb_window_wanted && ctest --test-dir build/fb_window_wanted
cmake_minimum_required(VERSION 3.20)
project(fb_window_wanted_test C)
set(CMAKE_C_STANDARD 11)
add_executable(fb_window_wanted_test test_main.c ../../src/video/fb_window_wanted.c)
target_include_directories(fb_window_wanted_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/video)
if(NOT MSVC)
    target_compile_options(fb_window_wanted_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME fb_window_wanted COMMAND fb_window_wanted_test)
