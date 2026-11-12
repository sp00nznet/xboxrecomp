# d3d11_hlsl_vkd3d - the Linux D3D11 host's HLSL compiler
# (src/d3d/d3d11_hlsl_vkd3d.c) on the emitters' output and, given a D3D11
# shader cache dir, on every shader in it; see test_main.c. Needs
# libvkd3d-shader (pkg-config libvkd3d-shader, or its header and library),
# so it refuses where there is none (macOS, Windows), and the posix ctest
# reports it n/a there.
#   cmake -S tests/d3d11_hlsl_vkd3d -B build/d3d11_hlsl_vkd3d
#   cmake --build build/d3d11_hlsl_vkd3d && ctest --test-dir build/d3d11_hlsl_vkd3d
#   build/d3d11_hlsl_vkd3d/d3d11_hlsl_vkd3d_test <cache dir>
cmake_minimum_required(VERSION 3.20)
project(d3d11_hlsl_vkd3d_test C)
if(WIN32)
    message(FATAL_ERROR "d3d11_hlsl_vkd3d: the Linux host's compiler; Windows uses d3dcompiler_47")
endif()
find_package(PkgConfig QUIET)
if(PKG_CONFIG_FOUND)
    pkg_check_modules(VKD3D_SHADER QUIET libvkd3d-shader)
endif()
if(NOT VKD3D_SHADER_FOUND)
    # Some distributions ship the library and header without the .pc file.
    find_path(VKD3D_SHADER_INCLUDE_DIRS vkd3d_shader.h PATH_SUFFIXES vkd3d)
    find_library(VKD3D_SHADER_LINK_LIBRARIES vkd3d-shader)
    if(NOT VKD3D_SHADER_INCLUDE_DIRS OR NOT VKD3D_SHADER_LINK_LIBRARIES)
        message(FATAL_ERROR "d3d11_hlsl_vkd3d: no libvkd3d-shader")
    endif()
endif()
set(CMAKE_C_STANDARD 11)
set(D ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
add_executable(d3d11_hlsl_vkd3d_test test_main.c
    ${D}/d3d11_hlsl_vkd3d.c
    ${D}/d3d8_vsh_parse.c ${D}/d3d8_combiners_parse.c
    ${D}/d3d8_vsh_hlsl.c ${D}/d3d8_combiners_hlsl.c)
target_include_directories(d3d11_hlsl_vkd3d_test PRIVATE ${D} ${VKD3D_SHADER_INCLUDE_DIRS})
target_link_libraries(d3d11_hlsl_vkd3d_test PRIVATE ${VKD3D_SHADER_LINK_LIBRARIES} m)
target_compile_options(d3d11_hlsl_vkd3d_test PRIVATE -Wall -Wextra -Wno-unused-parameter
    -Wno-unused-function)   # dot_cases.h's helpers this test does not use
enable_testing()
add_test(NAME d3d11_hlsl_vkd3d COMMAND d3d11_hlsl_vkd3d_test)
