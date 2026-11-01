# present_chain - present.shader's loader and request state
# (src/kernel/nv2a_present_chain.c): a missing library, a library with the
# wrong ABI, a stand-in with the right one, the preset lookup beside the
# executable, and the runtime-parameter store (present.shader_params) with
# its overrides, save state and the descriptor dump's preset parse, and
# profile requests (present.profile): rows tagged by stage, the profile
# scope and the profile error. The stand-ins export only librashader's two version calls, so
# the test needs neither the real library nor a GPU. Pure C:
#   cmake -S tests/present_chain -B build/present_chain
#   cmake --build build/present_chain && ctest --test-dir build/present_chain
cmake_minimum_required(VERSION 3.20)
project(present_chain_test C)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(XR_SRC ${CMAKE_CURRENT_LIST_DIR}/../../src)
set(XR_TP ${CMAKE_CURRENT_LIST_DIR}/../../third_party)

add_executable(present_chain_test
    test_main.c
    ${XR_SRC}/kernel/nv2a_present_chain.c
    ${XR_SRC}/platform/recomp_exe_dir.c)
target_include_directories(present_chain_test PRIVATE ${XR_SRC}/kernel ${XR_SRC}
    ${XR_SRC}/platform ${XR_TP}/librashader)
target_link_libraries(present_chain_test PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(present_chain_test PRIVATE
    MOD_SUFFIX="${CMAKE_SHARED_MODULE_SUFFIX}")

# Stand-ins for librashader: the ABI and API it reports.
foreach(v good:2:5 old_api:2:4 bad_abi:1:5)
    string(REPLACE ":" ";" f ${v})
    list(GET f 0 name)
    list(GET f 1 abi)
    list(GET f 2 api)
    add_library(fake_lrs_${name} MODULE fake_lrs.c)
    target_compile_definitions(fake_lrs_${name} PRIVATE FAKE_ABI=${abi} FAKE_API=${api})
    set_target_properties(fake_lrs_${name} PROPERTIES PREFIX "")
    add_dependencies(present_chain_test fake_lrs_${name})
endforeach()
add_library(fake_lrs_params MODULE fake_lrs.c)
target_compile_definitions(fake_lrs_params PRIVATE FAKE_ABI=2 FAKE_API=5 FAKE_PARAMS)
set_target_properties(fake_lrs_params PROPERTIES PREFIX "")
add_dependencies(present_chain_test fake_lrs_params)
add_library(fake_lrs_no_exports MODULE fake_lrs.c)
target_compile_definitions(fake_lrs_no_exports PRIVATE FAKE_NO_EXPORTS)
set_target_properties(fake_lrs_no_exports PROPERTIES PREFIX "")
add_dependencies(present_chain_test fake_lrs_no_exports)

if(NOT MSVC)
    target_compile_options(present_chain_test PRIVATE -Wall -Wextra)
endif()
enable_testing()
foreach(c request rate preset missing bad_abi old_api no_exports good params params_key
        describe describe_nolib profile params_cap revert)
    add_test(NAME present_chain_${c} COMMAND present_chain_test ${c}
             $<TARGET_FILE_DIR:fake_lrs_good>
             WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endforeach()
