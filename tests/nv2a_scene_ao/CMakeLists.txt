cmake_minimum_required(VERSION 3.20)
project(nv2a_scene_ao_test C)
# The scene stage's trigger and depth fit (nv2a_scene_ao.c) touch no GPU:
# draws and samples are arguments, so the test builds it alone, on every host.
set(CMAKE_C_STANDARD 11)
add_executable(nv2a_scene_ao_test test_main.c ../../src/kernel/nv2a_scene_ao.c)
target_include_directories(nv2a_scene_ao_test PRIVATE ../../src/kernel)
if(NOT MSVC)
    target_link_libraries(nv2a_scene_ao_test m)
endif()
enable_testing()
add_test(NAME nv2a_scene_ao_test COMMAND nv2a_scene_ao_test)
