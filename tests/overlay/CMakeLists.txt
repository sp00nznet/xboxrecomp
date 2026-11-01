# overlay - the enhancements overlay (nv2a_overlay, nv2a_overlay_ui, the
# font, the CPU reference raster, nv2a_stats): draw-list and raster
# checksums of fixed frames, layout at 720 and 2160 lines, the layers'
# publish, hide and visible mask, toast expiry, two writers and a reader,
# the menu try-lock, the overflow cut, inert stats and the disarmed state.
# POSIX (pthreads):
#   cmake -S tests/overlay -B build/overlay
#   cmake --build build/overlay && ctest --test-dir build/overlay
# OVERLAY_REF_OUT=<dir> also writes the reference pictures as PPM, for the
# window-shot comparison of phase 3B.10.
cmake_minimum_required(VERSION 3.20)
project(overlay_test C)
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
add_executable(overlay_test test_main.c
    ${XR_SRC}/kernel/nv2a_overlay.c
    ${XR_SRC}/kernel/nv2a_overlay_ui.c
    ${XR_SRC}/kernel/nv2a_overlay_font.c
    ${XR_SRC}/kernel/nv2a_overlay_glyph.c
    ${XR_SRC}/kernel/nv2a_overlay_ref.c
    ${XR_SRC}/kernel/nv2a_stats.c
    ${_vendored} "${_fonts_c}")
target_include_directories(overlay_test PRIVATE ${XR_SRC}/kernel ${XR_TP}/microui ${XR_TP}/stb)
target_compile_definitions(overlay_test PRIVATE XBOXRECOMP_ENHANCE
    "GLYPHS_TXT=\"${XR_TP}/kenney-input/glyphs.txt\"")
if(NOT MSVC)
    # The pinned checksums hash float positions: no fused multiply-adds,
    # which arm64 clang makes by default and x86 compilers do not.
    target_compile_options(overlay_test PRIVATE -Wall -Wextra -ffp-contract=off)
endif()
target_link_libraries(overlay_test PRIVATE Threads::Threads)
if(NOT WIN32)
    target_link_libraries(overlay_test PRIVATE m)
endif()
enable_testing()
add_test(NAME overlay COMMAND overlay_test)
