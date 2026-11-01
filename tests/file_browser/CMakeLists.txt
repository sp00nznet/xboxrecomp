# file_browser - the in-overlay file browser's model (src/enhance/
# file_browser.c; openspec change overlay-revamp, D6 and FB.4): roots and
# the exports root made on open, sorting, filters, hidden rows, ".." stopping
# at the root, a link out of the root listed but refused, an unreadable
# folder, the listing callback's error, name sanitising (each refused name),
# default and unique names, the import size cap, temp+rename writes and the
# export's overwrite confirm, and the Home pick under Wine. Everything runs
# in a fresh temp folder; HOME points into it. Pure C, no device:
#   cmake -S tests/file_browser -B build/file_browser
#   cmake --build build/file_browser && ctest --test-dir build/file_browser
cmake_minimum_required(VERSION 3.20)
project(file_browser_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(file_browser_test
    test_main.c
    ${XR_SRC}/enhance/file_browser.c)
target_include_directories(file_browser_test PRIVATE ${XR_SRC}/enhance)
if(NOT MSVC)
    target_compile_options(file_browser_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME file_browser COMMAND file_browser_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
