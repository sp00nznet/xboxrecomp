# hex_tile - textures.detile's sampling math (src/d3d/nv2a_hex_tile.h): the
# triangle grid's weights and vertices, the offset hash, the blend, and the
# shader texts' constants against the C reference's.
# Standalone:
#   cmake -S tests/hex_tile -B build/hex_tile
#   cmake --build build/hex_tile && ctest --test-dir build/hex_tile
cmake_minimum_required(VERSION 3.20)
project(hex_tile_test C)
set(CMAKE_C_STANDARD 11)
add_executable(hex_tile_test test_main.c)
target_include_directories(hex_tile_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(hex_tile_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(hex_tile_test PRIVATE m)
endif()
enable_testing()
add_test(NAME hex_tile COMMAND hex_tile_test)
