# render_scale - the enhancements' pure helpers in nv2a_backend_common:
# host target size, present rectangle, nearest upscale / box downscale,
# visibility-count scaling, host options. Standalone:
#   cmake -S tests/render_scale -B build/render_scale
#   cmake --build build/render_scale && ctest --test-dir build/render_scale
cmake_minimum_required(VERSION 3.20)
project(render_scale_test C)
set(CMAKE_C_STANDARD 11)
add_executable(render_scale_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(render_scale_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(render_scale_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(render_scale_test PRIVATE m)
endif()
enable_testing()
add_test(NAME render_scale COMMAND render_scale_test)
