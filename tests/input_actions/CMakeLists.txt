# input_actions - the host actions (src/enhance/enhance_actions.c): the
# binding rules and their refusals, hold chords, chord consumption, the
# modifier chord (guide+a), chord_delay_ms, the flip tick that reads a pad
# the title stopped polling, arming by feature keys only, the mod API v2
# calls, and the pad filter seam in xbox_InputGetState (input_core.c),
# all against a fake input backend with its own clock. Pure C, no device:
#   cmake -S tests/input_actions -B build/input_actions
#   cmake --build build/input_actions && ctest --test-dir build/input_actions
# -DINPUT_ACTIONS_TSAN=ON builds it with -fsanitize=thread for the live
# rebuild case (a polling thread and a flip thread against rebuilds).
cmake_minimum_required(VERSION 3.20)
project(input_actions_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
option(INPUT_ACTIONS_TSAN "Build with ThreadSanitizer" OFF)
if(INPUT_ACTIONS_TSAN)
    # tests/posix_ctest.sh builds this variant on every host: one whose
    # compiler cannot link TSan reports n/a instead of failing.
    include(CheckCSourceCompiles)
    set(CMAKE_REQUIRED_FLAGS -fsanitize=thread)
    set(CMAKE_REQUIRED_LINK_OPTIONS -fsanitize=thread)
    check_c_source_compiles("int main(void) { return 0; }" XR_HAVE_TSAN)
    unset(CMAKE_REQUIRED_FLAGS)
    unset(CMAKE_REQUIRED_LINK_OPTIONS)
    if(NOT XR_HAVE_TSAN)
        message(FATAL_ERROR "input_actions_tsan: this compiler cannot build -fsanitize=thread")
    endif()
endif()

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(input_actions_test
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
target_include_directories(input_actions_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(input_actions_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(input_actions_test PRIVATE -Wall -Wextra)
endif()
if(INPUT_ACTIONS_TSAN)
    target_compile_options(input_actions_test PRIVATE -fsanitize=thread -g -O1)
    target_link_options(input_actions_test PRIVATE -fsanitize=thread)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(input_actions_test PRIVATE m Threads::Threads)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(input_actions_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME input_actions COMMAND input_actions_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
