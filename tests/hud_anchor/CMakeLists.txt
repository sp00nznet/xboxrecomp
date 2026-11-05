# hud_anchor - the HUD layer (src/enhance/hud_anchor.c; openspec change
# hud-anchors, 1.9): the table's parse and refusals, the edge offsets with
# display.hud_aspect and display.hud_margin, the kinds (left, right,
# proportional, centre, world with park, stretch), the 4:3 window's clip
# and park, skewed and skipped calls with their trace lines, the wrappers'
# call chain, the canvas (view, top-left, memory with its fallback),
# byte-exact and nested restore, the backends' lift flag, and on POSIX two
# threads sharing a group's reference with chains of their own. Guest
# memory is a static buffer. Pure C, no device:
#   cmake -S tests/hud_anchor -B build/hud_anchor
#   cmake --build build/hud_anchor && ctest --test-dir build/hud_anchor
cmake_minimum_required(VERSION 3.20)
project(hud_anchor_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(hud_anchor_test
    test_main.c
    ${XR_SRC}/enhance/hud_anchor.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/kernel/nv2a_aspect.c
    ${XR_SRC}/kernel/nv2a_vsh_cpu.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(hud_anchor_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC})
target_compile_definitions(hud_anchor_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS)
if(NOT MSVC)
    target_compile_options(hud_anchor_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(hud_anchor_test PRIVATE m Threads::Threads)
endif()
enable_testing()
add_test(NAME hud_anchor COMMAND hud_anchor_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
