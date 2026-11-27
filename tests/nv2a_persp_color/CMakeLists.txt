cmake_minimum_required(VERSION 3.20)
project(nv2a_persp_color_regression C)
enable_testing()
add_executable(nv2a-persp-color-test nv2a_persp_color_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-persp-color-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-persp-color-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-persp-color-test PRIVATE m pthread)
endif()
add_test(NAME nv2a-persp-color COMMAND nv2a-persp-color-test)
