# dbg_draw - the mods' 3D debug draw lists (src/kernel/nv2a_dbg_draw.c; mod
# API version 8, openspec change collision-overlay, D3 and D4): the clip
# transform and its pull, the segments the flags pick, the colour bytes, the
# list limit, and the triple buffer between the guest's publish and the
# flip's take, a producer and a consumer thread included. Pure C, no device:
#   cmake -S tests/dbg_draw -B build/dbg_draw
#   cmake --build build/dbg_draw && ctest --test-dir build/dbg_draw
cmake_minimum_required(VERSION 3.20)
project(dbg_draw_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(dbg_draw_test test_main.c ${XR_SRC}/kernel/nv2a_dbg_draw.c)
target_include_directories(dbg_draw_test PRIVATE ${XR_SRC}/kernel)
target_compile_definitions(dbg_draw_test PRIVATE XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(dbg_draw_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(dbg_draw_test PRIVATE m Threads::Threads)
endif()
enable_testing()
add_test(NAME dbg_draw COMMAND dbg_draw_test)
