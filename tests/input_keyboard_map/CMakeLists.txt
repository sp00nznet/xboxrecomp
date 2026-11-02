# input_keyboard_map - keyboard.c's key map, resolved by the mapping rules
# (src/input/input_map.c), against a frozen copy of v0.14.0's resolver, and
# the Win32 window's key and mouse message decoding. No SDL and no window,
# so it builds and runs on every host. In-tree (src/input adds it), as
# keyboard.c needs the runtime's platform target:
#   cmake --build <build> --target input_keyboard_map_test && ctest -R input_keyboard_map

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(input_keyboard_map_test
    test_main.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/pointer.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/kbm_script.c)
target_include_directories(input_keyboard_map_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_keyboard_map_test PRIVATE platform xbox_input_map)
if(NOT WIN32)
    target_link_libraries(input_keyboard_map_test PRIVATE m)
endif()
add_test(NAME input_keyboard_map COMMAND input_keyboard_map_test)
