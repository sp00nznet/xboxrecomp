cmake_minimum_required(VERSION 3.20)
project(nv2a_ffp_backend_regression C)
enable_testing()
set(NV2A_BACKEND_TEST_SOURCES
    ../../src/kernel/nv2a_pb_exec.c
    ../../src/kernel/nv2a_vsh_interp.c
    ../../src/kernel/nv2a_combiner.c
    ../../src/kernel/nv2a_flip_hold.c)
add_executable(nv2a-ffp-backend-test nv2a_ffp_backend_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-present-sync-test nv2a_present_sync_test.c ${NV2A_BACKEND_TEST_SOURCES})
add_executable(nv2a-clear-present-test nv2a_clear_present_test.c ${NV2A_BACKEND_TEST_SOURCES})
foreach(target nv2a-ffp-backend-test nv2a-present-sync-test nv2a-clear-present-test)
    target_include_directories(${target} PRIVATE ../../src/kernel ../../src)
    if(MSVC)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    elseif(NOT WIN32)
        target_link_libraries(${target} PRIVATE m pthread)
    endif()
endforeach()
add_test(NAME nv2a-ffp-backend COMMAND nv2a-ffp-backend-test)
add_test(NAME nv2a-clear-present COMMAND nv2a-clear-present-test)
add_test(NAME nv2a-present-sync COMMAND nv2a-present-sync-test
         "${CMAKE_CURRENT_BINARY_DIR}/nv2a-present-sync-")
set_tests_properties(nv2a-present-sync PROPERTIES ENVIRONMENT
    "RECOMP_FB_DUMP_FLIPS=1;RECOMP_FB_DUMP=${CMAKE_CURRENT_BINARY_DIR}/nv2a-present-sync-")
# Triangle strips alternate their winding only with RECOMP_STRIP_ALT_WINDING; the same test
# checks the unchanged order without it (the test above) and the swapped one with it.
add_test(NAME nv2a-ffp-backend-strip-winding COMMAND nv2a-ffp-backend-test)
set_tests_properties(nv2a-ffp-backend-strip-winding PROPERTIES ENVIRONMENT "RECOMP_STRIP_ALT_WINDING=1")
