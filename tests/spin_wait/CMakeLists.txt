cmake_minimum_required(VERSION 3.20)
project(spin_wait_test C)
# Cross-platform: the same kernel_pacing.c runs on both hosts, under each
# host's own clock and wait primitives.
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(spin_wait_test test_main.c)
target_include_directories(spin_wait_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(spin_wait_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME spin_wait_test COMMAND spin_wait_test)
