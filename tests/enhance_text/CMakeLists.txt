# enhance_text - menu text with input glyphs (src/enhance/enhance_text.c,
# overlay-revamp GL.4): the {pad:}, {key:}, {mouse:} and {act:} tokens
# against the real input names and the overlay's glyph fonts, an action's
# binding per device from a stubbed registry, unknown tokens as text, and
# widths that match what is drawn. No device, no window:
#   cmake -S tests/enhance_text -B build/enhance_text
#   cmake --build build/enhance_text && ctest --test-dir build/enhance_text
cmake_minimum_required(VERSION 3.20)
project(enhance_text_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
find_package(Threads REQUIRED)

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
add_executable(enhance_text_test test_main.c
    ${XR_SRC}/enhance/enhance_text.c
    ${XR_SRC}/input/input_map.c
    ${XR_SRC}/input/input_names.c
    ${XR_SRC}/input/pad_map.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c
    ${XR_SRC}/kernel/nv2a_overlay.c
    ${XR_SRC}/kernel/nv2a_overlay_ui.c
    ${XR_SRC}/kernel/nv2a_overlay_font.c
    ${XR_SRC}/kernel/nv2a_overlay_glyph.c
    ${XR_SRC}/kernel/nv2a_stats.c
    ${_vendored} "${_fonts_c}")
target_include_directories(enhance_text_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/input
    ${XR_SRC}/kernel ${XR_SRC}/platform ${XR_SRC} ${XR_TP}/microui ${XR_TP}/stb)
target_compile_definitions(enhance_text_test PRIVATE XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(enhance_text_test PRIVATE -Wall -Wextra)
endif()
target_link_libraries(enhance_text_test PRIVATE Threads::Threads)
if(NOT WIN32)
    target_link_libraries(enhance_text_test PRIVATE m)
endif()
enable_testing()
add_test(NAME enhance_text COMMAND enhance_text_test)
