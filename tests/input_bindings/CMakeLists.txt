# input_bindings - the keyboard-and-mouse source of port 0: the binding
# vocabulary (src/input/input_names.c), the runtime bindings and the
# composer with its ramp, diagonals and walk key (keyboard.c), the mouse
# model, wheel pulses and capture state machine (mouse.c), the kbm_script
# injector (kbm_script.c) and the port-0 merge (input_core.c) against a fake
# backend and a fake clock. No SDL and no window, so it builds and runs on
# every host:
#   cmake --build <build> --target input_bindings_test && ctest -R input_bindings
# Cross-built for Windows, ctest runs it through CMAKE_CROSSCOMPILING_EMULATOR
# (tests/proton_run.sh).

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(input_bindings_test
    test_main.c
    ${INPUT_SRC}/input_core.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/kbm_script.c)
target_include_directories(input_bindings_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_bindings_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(input_bindings_test PRIVATE m)
endif()
enable_testing()
add_test(NAME input_bindings COMMAND input_bindings_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
