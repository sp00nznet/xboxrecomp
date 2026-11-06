cmake_minimum_required(VERSION 3.20)
project(mmx_helpers_test C)
# The MMX helpers: on x86-64 they are SSE2 intrinsics whose results must
# match the scalar reference this test carries; on other hosts the header
# falls back to the scalar path and the comparison is trivially true, so
# the test reports a skip rather than a pass (see test_main.c).
set(CMAKE_C_STANDARD 11)
add_executable(mmx_helpers_test test_main.c)
target_include_directories(mmx_helpers_test PRIVATE ../../templates/runtime)
enable_testing()
add_test(NAME mmx_helpers COMMAND mmx_helpers_test)
