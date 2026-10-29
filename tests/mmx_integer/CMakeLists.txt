cmake_minimum_required(VERSION 3.20)
project(mmx_integer_test C)
set(CMAKE_C_STANDARD 11)
add_executable(mmx_integer_test test_main.c)
# Shuffle and extract/insert need SSE. Preserve the existing flags for x86
# targets, but omit them for other targets and mixed-architecture builds.
if(NOT MSVC)
    include(CheckCSourceCompiles)
    check_c_source_compiles("
#if !defined(__i386__) && !defined(__x86_64__) && !defined(_M_IX86) && !defined(_M_X64)
#error not x86
#endif
int main(void) { return 0; }
" MMX_TEST_TARGET_X86)
    if(MMX_TEST_TARGET_X86)
        target_compile_options(mmx_integer_test PRIVATE -mmmx -msse)
    endif()
endif()
enable_testing()
add_test(NAME mmx_integer_test COMMAND mmx_integer_test)
set_tests_properties(mmx_integer_test PROPERTIES SKIP_RETURN_CODE 77)
