cmake_minimum_required(VERSION 3.20)
project(kernel_regressions_test C)
if(NOT WIN32)
    message(FATAL_ERROR "This synthetic-memory regression uses Windows VirtualAlloc")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_regressions_test test_main.c)
target_include_directories(kernel_regressions_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_regressions_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_regressions_test COMMAND kernel_regressions_test)

# The 2^31-call counter check takes tens of seconds; opt in with
# -DXBOX_SLOW_TESTS=ON, or run the executable with the argument "wrap".
option(XBOX_SLOW_TESTS "Also run the slow 2^31 kernel-call counter check" OFF)
if(XBOX_SLOW_TESTS)
    add_test(NAME kernel_regressions_wrap_test COMMAND kernel_regressions_test wrap)
endif()
