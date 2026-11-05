# present_chain_lrs - a composed shader profile (src/enhance/shader_profile.c)
# read by the real librashader: fsr then crt-lottes composed into one preset
# lists every parameter of both, with the values the wrappers set. The
# library is the pinned prebuilt release (cmake/librashader.cmake), loaded at
# run time as the runtime does; no GPU, only the preset parser.
#   cmake -S tests/present_chain_lrs -B build/present_chain_lrs
#   cmake --build build/present_chain_lrs && ctest --test-dir build/present_chain_lrs
# Skips where the release has no library (Linux) or the download failed.
cmake_minimum_required(VERSION 3.20)
project(present_chain_lrs_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/librashader.cmake)
_xboxrecomp_librashader_fetch(_lrs_lib _lrs_notices)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(present_chain_lrs_test
    test_main.c
    ${XR_SRC}/enhance/shader_profile.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c)
target_include_directories(present_chain_lrs_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/kernel ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(present_chain_lrs_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(present_chain_lrs_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(present_chain_lrs_test PRIVATE m)
endif()
target_link_libraries(present_chain_lrs_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
# An empty library path: the test exits 77, a skip.
add_test(NAME present_chain_lrs
         COMMAND present_chain_lrs_test "${_lrs_lib}"
                 ${CMAKE_CURRENT_LIST_DIR}/../../assets/shaders
         WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
set_tests_properties(present_chain_lrs PROPERTIES SKIP_RETURN_CODE 77)
