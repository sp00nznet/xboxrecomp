# enhance_mod - the enhancements layer's mods (src/enhance/mod.c): the
# title's table run in order by xbox_enhance_init, a failed init dropping
# what it registered, the frame dispatch and the reverse shutdown; the batch
# filter (nv2a_backend_common.c) through a stub backend's on_draw; and the
# input capture (enhance_input.c into mouse.c and the port-0 read in
# input_core.c) against a fake input backend. Pure C, no device:
#   cmake -S tests/enhance_mod -B build/enhance_mod
#   cmake --build build/enhance_mod && ctest --test-dir build/enhance_mod
cmake_minimum_required(VERSION 3.20)
project(enhance_mod_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(enhance_mod_test
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
target_include_directories(enhance_mod_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(enhance_mod_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(enhance_mod_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(enhance_mod_test PRIVATE m)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(enhance_mod_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME enhance_mod COMMAND enhance_mod_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
