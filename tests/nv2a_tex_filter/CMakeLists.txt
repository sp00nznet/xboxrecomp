cmake_minimum_required(VERSION 3.20)
project(nv2a_tex_filter_regression C)
enable_testing()
add_executable(nv2a-tex-filter-test nv2a_tex_filter_test.c
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
target_include_directories(nv2a-tex-filter-test PRIVATE ../../src/kernel ../../src)
if(MSVC)
    target_compile_definitions(nv2a-tex-filter-test PRIVATE _CRT_SECURE_NO_WARNINGS)
elseif(NOT WIN32)
    target_link_libraries(nv2a-tex-filter-test PRIVATE m pthread)
endif()

# The filter switches are read once per process: one run per mode and sampler.
set(modes nearest bilinear aniso8 aniso16)
set(env_nearest  "")
set(env_bilinear "RECOMP_TEX_BILINEAR=1")
set(env_aniso8   "RECOMP_TEX_ANISO=8")
set(env_aniso16  "RECOMP_TEX_ANISO=16")
foreach(mode IN LISTS modes)
    foreach(variant 2d xf rc)
        set(name nv2a-tex-filter-${mode}-${variant})
        add_test(NAME ${name} COMMAND nv2a-tex-filter-test ${mode} ${variant})
        if(NOT "${env_${mode}}" STREQUAL "")
            set_tests_properties(${name} PROPERTIES ENVIRONMENT "${env_${mode}}")
        endif()
    endforeach()
endforeach()
