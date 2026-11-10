cmake_minimum_required(VERSION 3.20)
project(apu_phys_test C)
# Which copy of a physical page the APU reads, when the heap and the
# contiguous window give the same physical number to two different pages.
# Cross-platform: the resolver and the heap are the same code on both hosts.
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(apu_phys_test test_main.c)
target_include_directories(apu_phys_test PRIVATE ../../src ../../src/apu ../../src/kernel)
target_link_libraries(apu_phys_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME apu_phys
         COMMAND apu_phys_test ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
