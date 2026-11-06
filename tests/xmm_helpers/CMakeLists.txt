cmake_minimum_required(VERSION 3.20)
project(xmm_helpers_test C)
# The XMM helpers: on x86-64 they are SSE intrinsics whose results must
# match the scalar reference this test carries -- lane for lane, bit for
# bit, across NaN, infinity and denormal inputs (the compare predicates'
# unordered behaviour and RCP/RSQRT's NaN pass-through live exactly
# there). On other hosts the header falls back to the scalar path and the
# comparison is trivially true, so the test reports a skip rather than a
# pass (see test_main.c).
set(CMAKE_C_STANDARD 11)
add_executable(xmm_helpers_test test_main.c)
target_include_directories(xmm_helpers_test PRIVATE ../../templates/runtime)
if(UNIX)
    target_link_libraries(xmm_helpers_test PRIVATE m)
endif()
enable_testing()
add_test(NAME xmm_helpers COMMAND xmm_helpers_test)
