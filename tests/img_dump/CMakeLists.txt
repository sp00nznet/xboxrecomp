# img_dump - the debug image writer (src/platform/img_dump.c): PNG round
# trips through an independent decoder (stb_image), the old BMP layouts, the
# alpha rule `frames compact` uses, the writer threads' queue and flush, and
# the per-run byte budget. Pure C:
#   cmake -S tests/img_dump -B build/img_dump
#   cmake --build build/img_dump && ctest --test-dir build/img_dump
# build/img_dump/img_dump_test FILE.bmp also times a real dump as PNG.
cmake_minimum_required(VERSION 3.20)
project(img_dump_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
find_package(Threads REQUIRED)
enable_testing()
# img_dump_sync_test: the same tests built with IMG_DUMP_WRITERS 0, the path
# taken when no writer thread starts (every PNG written by the call).
foreach(t img_dump_test img_dump_sync_test)
    add_executable(${t} test_main.c
        ${XR_SRC}/platform/img_dump.c
        ${XR_SRC}/platform/recomp_env.c
        ${XR_SRC}/platform/recomp_fatal.c
        ${XR_SRC}/platform/recomp_exit_hook.c)
    target_include_directories(${t} PRIVATE ${XR_SRC}/platform ${XR_SRC}
        ${CMAKE_CURRENT_LIST_DIR}/../../third_party/stb)
    if(NOT MSVC)
        target_compile_options(${t} PRIVATE -Wall -Wextra -O2)
    endif()
    target_link_libraries(${t} PRIVATE Threads::Threads)
    if(NOT WIN32)
        target_link_libraries(${t} PRIVATE m)
    endif()
endforeach()
target_compile_definitions(img_dump_sync_test PRIVATE IMG_DUMP_WRITERS=0 IMG_DUMP_TEST_SYNC)
add_test(NAME img_dump COMMAND img_dump_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
add_test(NAME img_dump_sync COMMAND img_dump_sync_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sync)
file(MAKE_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/sync)
