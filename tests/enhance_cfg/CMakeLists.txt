# enhance_cfg - the TOML-subset reader (src/enhance/recomp_cfg.c) and the
# env-over-file layering (src/enhance/enhance_cfg.c), xbox_enhance_init
# (src/enhance/enhance.c), the [input] keys (src/enhance/enhance_input.c,
# into src/input's binding table) and recomp_exe_dir. Pure C, no device:
#   cmake -S tests/enhance_cfg -B build/enhance_cfg
#   cmake --build build/enhance_cfg && ctest --test-dir build/enhance_cfg
# Also works from a parent project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(enhance_cfg_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(enhance_cfg_test
    test_main.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/enhance/enhance.c
    ${XR_SRC}/enhance/enhance_input.c
    ${XR_SRC}/input/keyboard.c
    ${XR_SRC}/input/mouse.c
    ${XR_SRC}/input/input_names.c
    ${XR_SRC}/input/kbm_script.c
    ${XR_SRC}/kernel/nv2a_backend_common.c
    ${XR_SRC}/kernel/nv2a_tex_pack.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c
    ${XR_SRC}/platform/recomp_exit_hook.c)
target_include_directories(enhance_cfg_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/input
    ${XR_SRC}/kernel ${XR_SRC}/d3d ${XR_SRC})
target_compile_definitions(enhance_cfg_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS)
if(NOT MSVC)
    target_compile_options(enhance_cfg_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(enhance_cfg_test PRIVATE m)
endif()
enable_testing()
add_test(NAME enhance_cfg COMMAND enhance_cfg_test
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
