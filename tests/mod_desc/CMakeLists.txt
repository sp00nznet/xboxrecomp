# mod_desc - the mod descriptor (mod-settings D1-D4, D9; src/enhance/mod.c
# and mod_describe.c): validation and its refusals, the settings' precedence
# rows with clamp, snap and the pending rewrites, the init rule, the
# deferred on_setting, the ACTION controls, unknown keys, and the
# RECOMP_MODS_DESCRIBE dump against expected.json byte for byte. Pure C, no
# device:
#   cmake -S tests/mod_desc -B build/mod_desc
#   cmake --build build/mod_desc && ctest --test-dir build/mod_desc
cmake_minimum_required(VERSION 3.20)
project(mod_desc_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(mod_desc_test
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
target_include_directories(mod_desc_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_desc_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE MOD_DESC_EXPECTED="${CMAKE_CURRENT_LIST_DIR}/expected.json")
if(NOT MSVC)
    target_compile_options(mod_desc_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(mod_desc_test PRIVATE m)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(mod_desc_test PRIVATE ${CMAKE_DL_LIBS})
# A librashader stand-in with a fixed parameter table, for the dump's
# shaders field.
add_library(fake_lrs_params MODULE ${CMAKE_CURRENT_LIST_DIR}/../present_chain/fake_lrs.c)
target_compile_definitions(fake_lrs_params PRIVATE FAKE_ABI=2 FAKE_API=5 FAKE_PARAMS)
set_target_properties(fake_lrs_params PROPERTIES PREFIX "")
add_dependencies(mod_desc_test fake_lrs_params)
target_compile_definitions(mod_desc_test PRIVATE
    FAKE_LRS_PARAMS="$<TARGET_FILE:fake_lrs_params>")
enable_testing()
add_test(NAME mod_desc COMMAND mod_desc_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
