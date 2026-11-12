# pb_backend - the executor backend's one selector (src/platform/pb_backend.c):
# RECOMP_PB_BACKEND when set, else the build's default (d3d11 in the
# dxvk-native build, cpu otherwise), and a d3d11 run demoted to cpu when it
# has no window. Built twice, as each build defines it. Pure C,
# cross-platform:
#   cmake -S tests/pb_backend -B build/pb_backend
#   cmake --build build/pb_backend && ctest --test-dir build/pb_backend
cmake_minimum_required(VERSION 3.20)
project(pb_backend_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
enable_testing()
foreach(v stock dxvk)
    add_executable(pb_backend_${v}_test test_main.c
        ${XR_SRC}/platform/pb_backend.c
        ${XR_SRC}/platform/recomp_env.c
        ${XR_SRC}/platform/recomp_exe_dir.c
        ${XR_SRC}/platform/recomp_fatal.c)
    target_include_directories(pb_backend_${v}_test PRIVATE ${XR_SRC}/platform ${XR_SRC})
    if(NOT MSVC)
        target_compile_options(pb_backend_${v}_test PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME pb_backend_${v} COMMAND pb_backend_${v}_test)
endforeach()
target_compile_definitions(pb_backend_dxvk_test PRIVATE XBOXRECOMP_DXVK_NATIVE)
