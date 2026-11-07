cmake_minimum_required(VERSION 3.20)
project(mmio_decode_test C)
set(CMAKE_C_STANDARD 11)
# Header-only decoder: no runtime to build, just the include path.
add_executable(mmio_decode_test test_main.c)
target_include_directories(mmio_decode_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME mmio_decode COMMAND mmio_decode_test)
set_tests_properties(mmio_decode PROPERTIES SKIP_RETURN_CODE 77)
