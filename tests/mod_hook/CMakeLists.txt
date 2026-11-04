# mod_hook - the mod API v4 (mod-connectivity-layer 4.1, 4.9, 5.4): v1 to
# v3 tables beside v4 ones, the [ENHANCE] mods= token (a stock run prints
# none; follows, a live toggle and a mod's own active state), and the hook
# slots (src/kernel/recomp_hook.c) as the generated code calls them, and a
# mod's HUD table (hud_anchor.c: the owner's listing, the keys' line): pre in
# table order and post in reverse, the entry ecx, edx and esp, a skip on a
# `ret 0x14` function and on one returning st0, and the refusals. Pure C,
# no device:
#   cmake -S tests/mod_hook -B build/mod_hook
#   cmake --build build/mod_hook && ctest --test-dir build/mod_hook
cmake_minimum_required(VERSION 3.20)
project(mod_hook_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(mod_hook_test
    test_main.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/enhance/enhance.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/kernel/nv2a_dbg_draw.c
    ${XR_SRC}/enhance/shader_profile.c
    ${XR_SRC}/enhance/shader_profile_req.c
    ${XR_SRC}/enhance/enhance_input.c
    ${XR_SRC}/enhance/enhance_actions.c
    ${XR_SRC}/enhance/mod.c
    ${XR_SRC}/enhance/hud_anchor.c
    ${XR_SRC}/enhance/hud_capture.c
    ${XR_SRC}/kernel/recomp_hook.c
    ${XR_SRC}/kernel/recomp_hook_run.c
    ${XR_SRC}/enhance/mod_describe.c
    ${XR_SRC}/input/input_core.c
    ${XR_SRC}/input/pad_map.c
    ${XR_SRC}/input/keyboard.c
    ${XR_SRC}/input/mouse.c
    ${XR_SRC}/input/pointer.c
    ${XR_SRC}/input/input_names.c
    ${XR_SRC}/input/input_map.c
    ${XR_SRC}/input/kbm_script.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c
    ${XR_SRC}/platform/recomp_exit_hook.c)
target_include_directories(mod_hook_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_hook_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(mod_hook_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(mod_hook_test PRIVATE m)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(mod_hook_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME mod_hook COMMAND mod_hook_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
