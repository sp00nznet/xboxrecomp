# input_keyboard - the key table and the one key map (src/input/keyboard.c)
# and the README table generated from it. No SDL and no Win32 window, so it
# builds and runs on every host:
#   cmake --build <build> --target input_keyboard_test && ctest -R input_keyboard
# Cross-built for Windows, ctest runs it through CMAKE_CROSSCOMPILING_EMULATOR
# (tests/proton_run.sh).
# INPUT_KEYBOARD_README_OUT=<file> writes the table block the README should hold.

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(input_keyboard_test
    test_main.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/kbm_script.c)
target_include_directories(input_keyboard_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_keyboard_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(input_keyboard_test PRIVATE m)
endif()
# The README beside the binary, as tests/input_map does (Proton runs the
# test from its own directory).
configure_file(${INPUT_SRC}/README.md ${CMAKE_CURRENT_BINARY_DIR}/input_README.md COPYONLY)
enable_testing()
add_test(NAME input_keyboard COMMAND input_keyboard_test input_README.md
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
