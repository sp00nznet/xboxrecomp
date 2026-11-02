# mod_status - mod API version 7 (src/enhance/mod.c, enhance_mod_status.c,
# enhance_text.c): a mod's status panel set, replaced, cut and cleared,
# dropped when the mod is switched off, stacked in load order, hidden under
# the menu, drawn on NV2A_OVL_STATUS through the CPU reference raster;
# glyph_name for a source, an action, a control, a chord, an unbound ref,
# each flag and after a rebinding; a version 6 mod that still loads and
# arms nothing. Pure C, no device:
#   cmake -S tests/mod_status -B build/mod_status
#   cmake --build build/mod_status && ctest --test-dir build/mod_status
cmake_minimum_required(VERSION 3.20)
project(mod_status_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(XR_SRC ${XR_ROOT}/src)
set(XR_TP ${XR_ROOT}/third_party)
set(_fonts "${XR_TP}/fonts/atkinson-hyperlegible/AtkinsonHyperlegible-Regular.ttf"
           "${XR_TP}/lucide/lucide-subset.ttf" "${XR_TP}/kenney-input/xbr-input.ttf")
string(REPLACE ";" "|" _fonts_arg "${_fonts}")
set(_fonts_c "${CMAKE_CURRENT_BINARY_DIR}/nv2a_overlay_fonts.c")
add_custom_command(
    OUTPUT "${_fonts_c}"
    COMMAND ${CMAKE_COMMAND} "-DOUT=${_fonts_c}"
            "-DNAMES=nv2a_ovl_font_regular|nv2a_ovl_font_icons|nv2a_ovl_font_input"
            "-DFILES=${_fonts_arg}"
            -P "${XR_ROOT}/cmake/embed_files.cmake"
    DEPENDS ${_fonts} "${XR_ROOT}/cmake/embed_files.cmake"
    VERBATIM)

set(_vendored ${XR_TP}/microui/microui.c ${XR_SRC}/kernel/nv2a_overlay_stb.c)
set_source_files_properties(${_vendored} PROPERTIES COMPILE_OPTIONS "-w")
add_executable(mod_status_test
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
    ${XR_SRC}/enhance/enhance_menu_layout.c
    ${XR_SRC}/enhance/enhance_menu_input.c
    ${XR_SRC}/enhance/enhance_osk.c
    ${XR_SRC}/enhance/enhance_text.c
    ${XR_SRC}/enhance/enhance_mod_status.c
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
    ${XR_SRC}/input/text_input.c
    ${XR_SRC}/input/input_names.c
    ${XR_SRC}/input/input_map.c
    ${XR_SRC}/input/kbm_script.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/kernel/nv2a_overlay.c
    ${XR_SRC}/kernel/nv2a_overlay_ui.c
    ${XR_SRC}/kernel/nv2a_overlay_font.c
    ${XR_SRC}/kernel/nv2a_overlay_glyph.c
    ${XR_SRC}/kernel/nv2a_overlay_ref.c
    ${XR_SRC}/kernel/nv2a_stats.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c
    ${XR_SRC}/platform/recomp_exit_hook.c
    ${_vendored} "${_fonts_c}")
target_include_directories(mod_status_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC} ${XR_TP}/microui ${XR_TP}/stb ${XR_SRC}/../third_party/librashader)
target_compile_definitions(mod_status_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(mod_status_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(mod_status_test PRIVATE m Threads::Threads)
endif()
# enhance.c sets present.shader; the chain module dlopens librashader.
target_link_libraries(mod_status_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
add_test(NAME mod_status COMMAND mod_status_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
