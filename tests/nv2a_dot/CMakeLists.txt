# nv2a_dot - the texture shader's dot-product modes and cube maps in the
# code both GPU backends share: the shader mode a DOTPRODUCT stage keeps with
# no texture, SET_DOT_RGBMAPPING's per-stage mapping, the cube-bind rule and
# the face stride, the packed shader key, and the HLSL and MSL text the
# combiner generators emit for Quantum Redshift's water program. API-free,
# every host.
#   cmake -S tests/nv2a_dot -B build/nv2a_dot
#   cmake --build build/nv2a_dot && ctest --test-dir build/nv2a_dot
cmake_minimum_required(VERSION 3.20)
project(nv2a_dot_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_dot_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c
    ../../src/d3d/d3d8_combiners_parse.c
    ../../src/d3d/d3d8_combiners_hlsl.c
    ../../src/d3d/d3d8_combiners_msl.c)
target_include_directories(nv2a_dot_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(nv2a_dot_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(nv2a_dot_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_dot COMMAND nv2a_dot_test)
