# mod_guest_call - the mod API v6 (entity-mod-api D1; src/enhance/mod.c and
# src/kernel/recomp_guest_call.c): guest_call from a frame callback and from
# a hook's pre, with every register in recomp_regs.h put back bit for bit,
# the arguments where the callee reads them, the frame 0x100 below esp with
# the first argument 16-byte aligned, the callee's eax returned; the
# refusals (off a guest callback, a host thread inside one, no lifted
# function, too many arguments, no guest stack); guest_alloc's one block per mod. Pure C, no device:
#   cmake -S tests/mod_guest_call -B build/mod_guest_call
#   cmake --build build/mod_guest_call && ctest --test-dir build/mod_guest_call
cmake_minimum_required(VERSION 3.20)
project(mod_guest_call_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(mod_guest_call_test
    test_main.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/enhance/enhance.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/enhance/shader_profile.c
    ${XR_SRC}/enhance/shader_profile_req.c
    ${XR_SRC}/enhance/enhance_input.c
    ${XR_SRC}/enhance/enhance_actions.c
    ${XR_SRC}/enhance/mod.c
    ${XR_SRC}/enhance/hud_anchor.c
    ${XR_SRC}/enhance/hud_capture.c
    ${XR_SRC}/kernel/recomp_hook.c
    ${XR_SRC}/kernel/recomp_hook_run.c
    ${XR_SRC}/kernel/recomp_guest_call.c
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
target_include_directories(mod_guest_call_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_guest_call_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(mod_guest_call_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(mod_guest_call_test PRIVATE m)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(mod_guest_call_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME mod_guest_call COMMAND mod_guest_call_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
