# input_text - the menu's text ring (src/input/text_input.c): UTF-8 and
# UTF-16 with surrogate pairs (a lone one dropped), control characters
# dropped, overflow and its count, the Begin/End gate, the request
# generation a sync reads, a producer thread against a consumer thread, and
# Begin and End while a producer pushes and a reader reads the request.
# No SDL and no window, so it builds and runs on every host:
#   cmake --build <build> --target input_text_test && ctest -R input_text
# A Windows build adds the WM_CHAR path: the repeat count, and DBCS pairing
# under code pages 932, 936 and 949 named explicitly. Which code page Windows
# itself converts an ANSI window's WM_CHAR by (the input language's, or the
# system ACP, UTF-8 under the "Beta: UTF-8" setting) no test here shows;
# that is a hand check with a Japanese IME on the PC.

set(INPUT_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src/input)
find_package(Threads REQUIRED)
# test_main.c includes text_input.c, to read the tag a push wrote.
add_executable(input_text_test test_main.c)
target_include_directories(input_text_test PRIVATE ${INPUT_SRC})
target_link_libraries(input_text_test PRIVATE platform Threads::Threads)
if(WIN32)
    target_link_libraries(input_text_test PRIVATE user32)
endif()
enable_testing()
add_test(NAME input_text COMMAND input_text_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(input_text PROPERTIES TIMEOUT 60)
