# occ_reports - the visibility-test tracker in nv2a_backend_common (nv2a_occ)
# with fake query ops and a fake clock: the settle, the retired
# list, the last count per report address, the delay and settle knobs.
# Standalone:
#   cmake -S tests/occ_reports -B build/occ_reports
#   cmake --build build/occ_reports && ctest --test-dir build/occ_reports
cmake_minimum_required(VERSION 3.20)
project(occ_reports_test C)
set(CMAKE_C_STANDARD 11)
add_executable(occ_reports_test test_main.c
    ../../src/kernel/nv2a_backend_common.c
    ../../src/kernel/nv2a_tex_pack.c)
target_include_directories(occ_reports_test PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/kernel
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/d3d)
if(NOT MSVC)
    target_compile_options(occ_reports_test PRIVATE -Wall -Wextra)
endif()
if(NOT WIN32)
    target_link_libraries(occ_reports_test PRIVATE m)
endif()
enable_testing()
add_test(NAME occ_reports COMMAND occ_reports_test)
