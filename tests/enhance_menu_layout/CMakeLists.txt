# enhance_menu_layout - the in-game menu's geometry (src/enhance/
# enhance_menu_layout.c, D2): the panel in each size mode at the D2 output
# sizes and ui scales, the toolbar folded and unfolded, the 16:9 cap, the
# hit test round trip and the dialog. Pure C, standalone:
#   cmake -S tests/enhance_menu_layout -B build/enhance_menu_layout
#   cmake --build build/enhance_menu_layout && ctest --test-dir build/enhance_menu_layout
cmake_minimum_required(VERSION 3.20)
project(enhance_menu_layout_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
add_executable(enhance_menu_layout_test test_main.c
    ${CMAKE_CURRENT_LIST_DIR}/../../src/enhance/enhance_menu_layout.c)
target_include_directories(enhance_menu_layout_test PRIVATE
    ${CMAKE_CURRENT_LIST_DIR}/../../src/enhance)
if(NOT MSVC)
    target_compile_options(enhance_menu_layout_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME enhance_menu_layout COMMAND enhance_menu_layout_test)
