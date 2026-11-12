# d3d11_host_sdl - the D3D11 backend's Linux host (src/d3d/d3d11_host_sdl.c)
# against a fake SDL presenter and a fake compiler: the clocks, the window
# calls, the C ID3D10Blob and mkdir. No dxvk-native or vkd3d needed, so it
# runs on every POSIX host (Windows has its own host).
#   cmake -S tests/d3d11_host_sdl -B build/d3d11_host_sdl
#   cmake --build build/d3d11_host_sdl && ctest --test-dir build/d3d11_host_sdl
cmake_minimum_required(VERSION 3.20)
project(d3d11_host_sdl_test C)
if(WIN32)
    message(FATAL_ERROR "d3d11_host_sdl: the POSIX host; Windows uses d3d11_host_win32.c")
endif()
set(CMAKE_C_STANDARD 11)
set(S ${CMAKE_CURRENT_SOURCE_DIR}/../../src)
add_executable(d3d11_host_sdl_test test_main.c ${S}/d3d/d3d11_host_sdl.c)
target_include_directories(d3d11_host_sdl_test PRIVATE ${S}/d3d ${S})
target_compile_options(d3d11_host_sdl_test PRIVATE -Wall -Wextra -Wno-unused-parameter)
enable_testing()
add_test(NAME d3d11_host_sdl COMMAND d3d11_host_sdl_test)
