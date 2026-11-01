# enhance_menu - the in-game menu (src/enhance/enhance_menu.c) as a title
# runs it, through xbox_enhance_init and xbox_InputGetState against a fake
# input backend with its own clock, drawn by the real overlay: navigation,
# the neutral pad, a Mods toggle through on_enable at xbox_mod_frame, the
# Controls capture and its refusals, Save of an action binding and of an
# item (config.toml holds only the changed keys), the Mouse group, the
# pack's miss snapshot, the Shaders page's parameter rows and their Save,
# and each page's draw list at 1080 lines against a
# pinned checksum. Pure C, no device:
#   cmake -S tests/enhance_menu -B build/enhance_menu
#   cmake --build build/enhance_menu && ctest --test-dir build/enhance_menu
cmake_minimum_required(VERSION 3.20)
project(enhance_menu_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(XR_SRC ${XR_ROOT}/src)
set(XR_TP ${XR_ROOT}/third_party)
set(_fonts "${XR_TP}/fonts/atkinson-hyperlegible/AtkinsonHyperlegible-Regular.ttf")
string(REPLACE ";" "|" _fonts_arg "${_fonts}")
set(_fonts_c "${CMAKE_CURRENT_BINARY_DIR}/nv2a_overlay_fonts.c")
add_custom_command(
    OUTPUT "${_fonts_c}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${_fonts_c}"
            "-DNAMES=nv2a_ovl_font_regular" "-DFILES=${_fonts_arg}"
            -P "${XR_ROOT}/cmake/embed_files.cmake"
    DEPENDS ${_fonts} "${XR_ROOT}/cmake/embed_files.cmake"
    VERBATIM)

set(_vendored ${XR_TP}/microui/microui.c ${XR_SRC}/kernel/nv2a_overlay_stb.c)
set_source_files_properties(${_vendored} PROPERTIES COMPILE_OPTIONS "-w")
add_executable(enhance_menu_test
    test_main.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/enhance/enhance.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/enhance/shader_profile.c
    ${XR_SRC}/enhance/shader_profile_req.c
    ${XR_SRC}/enhance/shader_profile_edit.c
    ${XR_SRC}/enhance/shader_param_kinds.c
    ${XR_SRC}/enhance/enhance_input.c
    ${XR_SRC}/enhance/enhance_actions.c
    ${XR_SRC}/enhance/enhance_menu.c
    ${XR_SRC}/enhance/mod.c
    ${XR_SRC}/enhance/hud_anchor.c
    ${XR_SRC}/enhance/hud_capture.c
    ${XR_SRC}/kernel/recomp_hook.c
    ${XR_SRC}/enhance/mod_describe.c
    ${XR_SRC}/input/input_core.c
    ${XR_SRC}/input/pad_map.c
    ${XR_SRC}/input/keyboard.c
    ${XR_SRC}/input/mouse.c
    ${XR_SRC}/input/input_names.c
    ${XR_SRC}/input/input_map.c
    ${XR_SRC}/input/kbm_script.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/kernel/nv2a_overlay.c
    ${XR_SRC}/kernel/nv2a_overlay_ui.c
    ${XR_SRC}/kernel/nv2a_overlay_font.c
    ${XR_SRC}/kernel/nv2a_stats.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c
    ${XR_SRC}/platform/recomp_exit_hook.c
    ${_vendored} "${_fonts_c}")
target_include_directories(enhance_menu_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_TP}/microui ${XR_TP}/stb ${XR_SRC}/../third_party/librashader)
target_compile_definitions(enhance_menu_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    # The pinned checksums hash float positions: no fused multiply-adds,
    # which arm64 clang makes by default and x86 compilers do not.
    target_compile_options(enhance_menu_test PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(enhance_menu_test PRIVATE m Threads::Threads)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(enhance_menu_test PRIVATE ${CMAKE_DL_LIBS})
# A librashader stand-in with a fixed parameter table (the present_chain
# test's), for the Shaders page.
add_library(fake_lrs_params MODULE ${CMAKE_CURRENT_LIST_DIR}/../present_chain/fake_lrs.c)
target_compile_definitions(fake_lrs_params PRIVATE FAKE_ABI=2 FAKE_API=5 FAKE_PARAMS)
set_target_properties(fake_lrs_params PROPERTIES PREFIX "")
add_dependencies(enhance_menu_test fake_lrs_params)
target_compile_definitions(enhance_menu_test PRIVATE
    FAKE_LRS_PARAMS="$<TARGET_FILE:fake_lrs_params>")
enable_testing()
add_test(NAME enhance_menu COMMAND enhance_menu_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
