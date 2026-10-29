cmake_minimum_required(VERSION 3.20)
project(mmx_integer_test C)
set(CMAKE_C_STANDARD 11)
add_executable(mmx_integer_test test_main.c)
# 32-bit x86 needs SSE enabled for shuffle and extract/insert. Check the
# compiler target rather than the build host so cross-compilation works too.
if(NOT MSVC)
    include(CheckCSourceCompiles)
    check_c_source_compiles("
#if !defined(__i386__) && !defined(_M_IX86)
#error not 32-bit x86
#endif
int main(void) { return 0; }
" MMX_TEST_TARGET_X86_32)
    if(MMX_TEST_TARGET_X86_32)
        target_compile_options(mmx_integer_test PRIVATE -mmmx -msse)
    endif()
endif()
enable_testing()
add_test(NAME mmx_integer_test COMMAND mmx_integer_test)
set_tests_properties(mmx_integer_test PROPERTIES SKIP_RETURN_CODE 77)
