# hud_capture - the HUD mapping capture (src/enhance/hud_capture.c; openspec
# change hud-anchors, 2.1, design A15) over a synthetic run: the keys, a
# table file with [capture] draws loaded and hooked by the capture itself,
# level 1 at hud_apply with the mod off (recorded, vertices untouched),
# level 0 at a draw entry with its callers found on a fake stack, the
# pictures' requests taken and refused as the walker would, and the JSONL
# lines with their order. Guest memory is a static buffer; the kernel side
# (walker idle, code range) is faked. Pure C, no device:
#   cmake -S tests/hud_capture -B build/hud_capture
#   cmake --build build/hud_capture && ctest --test-dir build/hud_capture
cmake_minimum_required(VERSION 3.20)
project(hud_capture_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(hud_capture_test
    test_main.c
    ${XR_SRC}/enhance/hud_capture.c
    ${XR_SRC}/enhance/hud_anchor.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/kernel/nv2a_aspect.c
    ${XR_SRC}/kernel/nv2a_vsh_cpu.c
    ${XR_SRC}/platform/recomp_env.c)
target_include_directories(hud_capture_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC})
# The capture's picture requests (nv2a_cap_*) exist in enhance builds only.
target_compile_definitions(hud_capture_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(hud_capture_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(hud_capture_test PRIVATE m Threads::Threads)
endif()
enable_testing()
add_test(NAME hud_capture COMMAND hud_capture_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
