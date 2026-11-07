cmake_minimum_required(VERSION 3.20)
project(fb_present_test C)
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(fb_present_test test_main.c)
target_compile_definitions(fb_present_test PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_include_directories(fb_present_test PRIVATE
    ../../src
    ../../src/input
    ../../src/platform
    ../../src/video
)
target_link_libraries(fb_present_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME fb_present COMMAND fb_present_test)
set_tests_properties(fb_present PROPERTIES SKIP_RETURN_CODE 77)
