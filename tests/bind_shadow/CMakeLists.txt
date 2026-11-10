# bind_shadow - the per-encoder binding shadow the Metal backend's draw path
# uses (src/kernel/nv2a_bind_shadow.h, openspec change
# pb-batch-cost D4): against a model encoder that starts empty and keeps
# its bindings, every draw sees exactly the objects it asked for, while
# repeated objects on one encoder are not bound again; a new encoder, the
# shadow off and a bind behind the shadow (drop) all rebind. Pure C:
#   cmake -S tests/bind_shadow -B build/bind_shadow
#   cmake --build build/bind_shadow && ctest --test-dir build/bind_shadow
cmake_minimum_required(VERSION 3.20)
project(bind_shadow_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(bind_shadow_test test_main.c)
target_include_directories(bind_shadow_test PRIVATE ${XR_SRC}/kernel)
if(NOT MSVC)
    target_compile_options(bind_shadow_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
add_test(NAME bind_shadow COMMAND bind_shadow_test)
