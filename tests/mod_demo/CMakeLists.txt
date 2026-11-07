# mod_demo - the template mod (templates/mod/demo_mod.c, the one
# docs/runtime/mod-howto.md walks through) built as a title builds it and run
# through xbox_enhance_init: off by default and silent, on from the user file,
# dormant and turned on from the menu's side, its setting clamped and
# delivered, its control registered and in the dump, its panel, and its hook
# through a slot declared as the generated code declares one. The same file
# is also built as a game without a hook, and with no API (the NULL accessor).
# Pure C, no device:
#   cmake -S tests/mod_demo -B build/mod_demo
#   cmake --build build/mod_demo && ctest --test-dir build/mod_demo
cmake_minimum_required(VERSION 3.20)
project(mod_demo_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(XR_SRC ${XR_ROOT}/src)
add_executable(mod_demo_test
    test_main.c
    ${XR_ROOT}/templates/mod/demo_mod.c
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
target_include_directories(mod_demo_test PRIVATE ${XR_ROOT}/templates/mod ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_demo_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE DEMO_MOD_HOOK_VA=0x00010000u)
if(NOT MSVC)
    target_compile_options(mod_demo_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(mod_demo_test PRIVATE m)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(mod_demo_test PRIVATE ${CMAKE_DL_LIBS})

# As a game builds the copy: the API, no DEMO_MOD_HOOK_VA.
add_library(mod_demo_nohook OBJECT ${XR_ROOT}/templates/mod/demo_mod.c)
target_include_directories(mod_demo_nohook PRIVATE ${XR_ROOT}/templates/mod ${XR_SRC}/enhance
    ${XR_SRC}/platform ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC}
    ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_demo_nohook PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS XBOXRECOMP_ENHANCE)

# No enhancements layer: no define, and no toolkit header on the path.
add_executable(mod_demo_null null_main.c ${XR_ROOT}/templates/mod/demo_mod.c)
target_include_directories(mod_demo_null PRIVATE ${XR_ROOT}/templates/mod)
if(NOT MSVC)
    target_compile_options(mod_demo_nohook PRIVATE -Wall -Wextra)
    target_compile_options(mod_demo_null PRIVATE -Wall -Wextra)
endif()

enable_testing()
add_test(NAME mod_demo COMMAND mod_demo_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME mod_demo_null COMMAND mod_demo_null)
