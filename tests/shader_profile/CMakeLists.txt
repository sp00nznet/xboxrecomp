# shader_profile - shader profiles (src/enhance/shader_profile.c): the
# profile file's parse and save, slugs, the folder's list, stage references,
# the shared-value rule and the composer that turns a chain of presets into
# one .slangp, plus enhance_cfg_user_dir and the start's choice between
# present.profile and present.shader (shader_profile_req.c, design D7). Pure
# C, no device and no library:
#   cmake -S tests/shader_profile -B build/shader_profile
#   cmake --build build/shader_profile && ctest --test-dir build/shader_profile
# The bundled-preset case reads assets/shaders. Also works from a parent
# project with add_subdirectory().
cmake_minimum_required(VERSION 3.20)
project(shader_profile_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
add_executable(shader_profile_test
    test_main.c
    ${XR_SRC}/enhance/shader_profile.c
    ${XR_SRC}/enhance/shader_profile_req.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/enhance/enhance_cfg.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/platform/recomp_env.c
    ${XR_SRC}/platform/recomp_exe_dir.c)
target_include_directories(shader_profile_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/platform
    ${XR_SRC}/kernel ${XR_SRC} ${XR_SRC}/../third_party/librashader)
target_compile_definitions(shader_profile_test PRIVATE RECOMP_ENV_HAVE_ENHANCE_KEYS
    XBOXRECOMP_ENHANCE)
if(NOT MSVC)
    target_compile_options(shader_profile_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(shader_profile_test PRIVATE m)
endif()
# The preset key comes from the chain module, which dlopens librashader.
target_link_libraries(shader_profile_test PRIVATE ${CMAKE_DL_LIBS})
enable_testing()
set(_shaders ${CMAKE_CURRENT_LIST_DIR}/../../assets/shaders)
foreach(_case names parse save list stages rehome compose bundled paths user_dir start)
    add_test(NAME shader_profile_${_case} COMMAND shader_profile_test ${_case} ${_shaders}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
# Cases share the work folder; run them one at a time.
set_tests_properties(shader_profile_names shader_profile_parse shader_profile_save
    shader_profile_list shader_profile_stages shader_profile_rehome shader_profile_compose
    shader_profile_bundled shader_profile_paths shader_profile_user_dir shader_profile_start
    PROPERTIES RESOURCE_LOCK shader_profile_work)
