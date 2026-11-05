# pad_map - the host -> Duke mapping table (src/input/pad_map.c), the
# deadzone helper, the README table and the connection cache with its probe
# backoff (src/input/input_core.c), against a fake backend. No device, no SDL
# and no XInput, so it builds and runs on every host:
#   cmake --build <build> --target pad_map_test && ctest -R pad_map
# Cross-built for Windows, ctest runs it through CMAKE_CROSSCOMPILING_EMULATOR
# (tests/proton_run.sh).
# PAD_MAP_README_OUT=<file> writes the table block the README should hold.

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
add_executable(pad_map_test
    test_main.c
    ${INPUT_SRC}/pad_map.c
    ${INPUT_SRC}/input_core.c
    ${INPUT_SRC}/keyboard.c
    ${INPUT_SRC}/mouse.c
    ${INPUT_SRC}/pointer.c
    ${INPUT_SRC}/input_names.c
    ${INPUT_SRC}/input_map.c
    ${INPUT_SRC}/kbm_script.c)
target_include_directories(pad_map_test PRIVATE ${INPUT_SRC})
target_link_libraries(pad_map_test PRIVATE platform)
if(NOT WIN32)
    target_link_libraries(pad_map_test PRIVATE m)
endif()
# The README beside the binary: under Proton the test runs from its own
# directory and a POSIX path would not resolve. configure_file re-copies it
# whenever it changes.
configure_file(${INPUT_SRC}/README.md ${CMAKE_CURRENT_BINARY_DIR}/input_README.md COPYONLY)
enable_testing()
add_test(NAME pad_map COMMAND pad_map_test input_README.md
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
