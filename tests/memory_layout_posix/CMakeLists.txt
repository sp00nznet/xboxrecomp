cmake_minimum_required(VERSION 3.20)
project(memory_layout_posix_test C)
# The POSIX counterpart to the Windows-only regressions beside it: this one
# exists to pin down what the Xbox memory model must do on a host whose page
# size is not 4 KB and whose low address space is not available.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX mapping path")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(memory_layout_posix_test test_main.c)
target_include_directories(memory_layout_posix_test PRIVATE ../../src/kernel)
target_link_libraries(memory_layout_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME memory_layout_posix_test
         COMMAND memory_layout_posix_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
