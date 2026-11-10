cmake_minimum_required(VERSION 3.20)
project(kernel_path_test C)
# Cross-platform: the path layer called directly, so it covers the Win32
# backend on Windows/Proton and the POSIX one elsewhere.
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(kernel_path_test test_main.c)
target_include_directories(kernel_path_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_path_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_path_test COMMAND kernel_path_test)
