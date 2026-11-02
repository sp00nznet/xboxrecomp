# input_pointer - the menu's absolute pointer (src/input/pointer.c): the
# Win32 messages' numbers (signed client coordinates, the wheel's high word
# and a high-resolution wheel's part notches, focus loss), SDL's points
# times the pixel density and its fractional wheel, the wheel taken once,
# a press and its release between two takes (and where it was), WM_MOUSEMOVE's
# MK_* flags letting go of a button released outside the window, seq on
# every event, and the seqlock against a writer thread.
# No SDL and no window, so it builds and runs on every host:
#   cmake --build <build> --target input_pointer_test && ctest -R input_pointer

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
find_package(Threads REQUIRED)
add_executable(input_pointer_test test_main.c ${INPUT_SRC}/pointer.c)
target_include_directories(input_pointer_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_pointer_test PRIVATE Threads::Threads)
if(NOT MSVC)
    target_link_libraries(input_pointer_test PRIVATE m)
endif()
enable_testing()
add_test(NAME input_pointer COMMAND input_pointer_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(input_pointer PROPERTIES TIMEOUT 60)
