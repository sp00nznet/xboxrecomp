cmake_minimum_required(VERSION 3.20)
project(memory_layout_posix_test C)
# The POSIX counterpart to the Windows-only regressions beside it: this one
# exists to pin down what the Xbox memory model must do on a host whose page
# size is not 4 KB and whose low address space is not available.
include(${CMAKE_CURRENT_LIST_DIR}/../xr_test.cmake)
if(WIN32)
    xr_test_refuse("This regression covers the POSIX mapping path")
    return()
endif()
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(memory_layout_posix_test test_main.c)
target_include_directories(memory_layout_posix_test PRIVATE ../../src/kernel)
target_link_libraries(memory_layout_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME memory_layout_posix_test
         COMMAND memory_layout_posix_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
