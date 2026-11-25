# nv2a_tex_signed - SET_TEXTURE_FILTER's signed channels (bits 28-31) in the
# shared code: the stage decode and the pixel-shader key, the remap of a
# fetched byte to 2 s / 255 against the hardware goldens' values, and the
# HLSL and MSL the combiner generators emit for a signed stage and for a bump
# stage reading one. On macOS the MSL is also compiled with Metal.
#   cmake -S tests/nv2a_tex_signed -B build/nv2a_tex_signed
#   cmake --build build/nv2a_tex_signed && ctest --test-dir build/nv2a_tex_signed
cmake_minimum_required(VERSION 3.20)
project(nv2a_tex_signed_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_tex_signed_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c
    ../../src/d3d/d3d8_combiners_parse.c
    ../../src/d3d/d3d8_combiners_hlsl.c
    ../../src/d3d/d3d8_combiners_msl.c)
target_include_directories(nv2a_tex_signed_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(APPLE)
    enable_language(OBJC)
    target_sources(nv2a_tex_signed_test PRIVATE msl_compile.m)
    target_compile_definitions(nv2a_tex_signed_test PRIVATE MSL_HAVE_METAL=1)
    target_link_libraries(nv2a_tex_signed_test PRIVATE "-framework Metal" "-framework Foundation")
endif()
if(NOT MSVC)
    target_compile_options(nv2a_tex_signed_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(nv2a_tex_signed_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_tex_signed COMMAND nv2a_tex_signed_test)
