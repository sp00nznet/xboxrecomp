# tex_pack - the texture pack reader (src/kernel/nv2a_tex_pack.c): a pack the
# test writes, loaded, looked up and read back; every refusal; and the
# nv2a_tex_hash vector the CLI's Python port checks too. Pure C, no device:
#   cmake -S tests/tex_pack -B build/tex_pack
#   cmake --build build/tex_pack && ctest --test-dir build/tex_pack
cmake_minimum_required(VERSION 3.20)
project(tex_pack_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(tex_pack_test test_main.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/kernel/nv2a_backend_common.c)
target_include_directories(tex_pack_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}/d3d)
if(NOT MSVC)
    target_compile_options(tex_pack_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(tex_pack_test PRIVATE m)
endif()
enable_testing()
add_test(NAME tex_pack COMMAND tex_pack_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
