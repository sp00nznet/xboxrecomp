# shader_param_kinds - how the Shaders page shows a preset parameter
# (src/enhance/shader_param_kinds.c): every shipped shader's parameters, read
# from assets/shaders, through the rules with the shipped sidecar (the 0..1
# toggles, FSR_GRAINCOLOR's label, the MASK and shadowMask choices, sliders
# for the rest); the inferred choice, a profile's [param_kinds] over the
# sidecar, a declaration that does not fit, the value mapping; and the
# declaring file the chain sets on each row (src/kernel/nv2a_present_chain.c:
# #reference, #include, the first pass of two). A librashader stand-in
# supplies the parameter table, so no GPU. Pure C:
#   cmake -S tests/shader_param_kinds -B build/shader_param_kinds
#   cmake --build build/shader_param_kinds && ctest --test-dir build/shader_param_kinds
cmake_minimum_required(VERSION 3.20)
project(shader_param_kinds_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_ROOT ${CMAKE_CURRENT_LIST_DIR}/../..)
set(XR_SRC ${XR_ROOT}/src)
set(XR_TP ${XR_ROOT}/third_party)

add_executable(shader_param_kinds_test
    test_main.c
    ${XR_SRC}/enhance/shader_param_kinds.c
    ${XR_SRC}/enhance/recomp_cfg.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/platform/recomp_exe_dir.c)
target_include_directories(shader_param_kinds_test PRIVATE ${XR_SRC}/enhance ${XR_SRC}/kernel
    ${XR_SRC} ${XR_SRC}/platform ${XR_TP}/librashader)
target_link_libraries(shader_param_kinds_test PRIVATE ${CMAKE_DL_LIBS})
if(NOT WIN32)
    target_link_libraries(shader_param_kinds_test PRIVATE m)
endif()
target_compile_definitions(shader_param_kinds_test PRIVATE
    XR_SHADERS="${XR_ROOT}/assets/shaders")

add_library(fake_lrs_params MODULE ${CMAKE_CURRENT_LIST_DIR}/../present_chain/fake_lrs.c)
target_compile_definitions(fake_lrs_params PRIVATE FAKE_ABI=2 FAKE_API=5 FAKE_PARAMS)
set_target_properties(fake_lrs_params PROPERTIES PREFIX "")
add_dependencies(shader_param_kinds_test fake_lrs_params)
target_compile_definitions(shader_param_kinds_test PRIVATE
    FAKE_LRS_PARAMS="$<TARGET_FILE:fake_lrs_params>")

if(NOT MSVC)
    target_compile_options(shader_param_kinds_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
foreach(c shipped rules profile files)
    add_test(NAME shader_param_kinds_${c} COMMAND shader_param_kinds_test ${c}
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
