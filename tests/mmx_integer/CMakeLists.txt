cmake_minimum_required(VERSION 3.20)
project(mmx_integer_test C)
set(CMAKE_C_STANDARD 11)
add_executable(mmx_integer_test test_main.c)
# _mm_shuffle_pi16 and the extract/insert forms are SSE, not baseline MMX.
# Only x86 compilers know the flags; elsewhere the test builds its stub and
# skips (exit 77).
# The processor name's case varies by host (AMD64 on Windows, amd64 on BSD).
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" mmx_cpu)
if(NOT MSVC AND mmx_cpu MATCHES "^(x86_64|amd64|i[3-6]86|x86)$")
    target_compile_options(mmx_integer_test PRIVATE -mmmx -msse)
endif()
enable_testing()
add_test(NAME mmx_integer_test COMMAND mmx_integer_test)
set_tests_properties(mmx_integer_test PROPERTIES SKIP_RETURN_CODE 77)
