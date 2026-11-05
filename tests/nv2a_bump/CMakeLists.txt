# nv2a_bump - BUMPENVMAP and BUMPENVMAP_LUMINANCE in the shared code: the
# stage's BUMP_ENV_MAT, SCALE and OFFSET decode, their copy into the pixel
# constants, OTHER_STAGE_INPUT's stage selection, and the HLSL and MSL text
# the combiner generators emit for a bump stage. API-free, every host.
#   cmake -S tests/nv2a_bump -B build/nv2a_bump
#   cmake --build build/nv2a_bump && ctest --test-dir build/nv2a_bump
cmake_minimum_required(VERSION 3.20)
project(nv2a_bump_test C)
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_bump_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c
    ../../src/d3d/d3d8_combiners_parse.c
    ../../src/d3d/d3d8_combiners_hlsl.c
    ../../src/d3d/d3d8_combiners_msl.c)
target_include_directories(nv2a_bump_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(nv2a_bump_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(nv2a_bump_test PRIVATE m)
endif()
enable_testing()
add_test(NAME nv2a_bump COMMAND nv2a_bump_test)
