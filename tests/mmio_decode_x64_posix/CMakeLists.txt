cmake_minimum_required(VERSION 3.20)
project(mmio_decode_x64_posix_test C)
set(CMAKE_C_STANDARD 11)
# Header-only decoder: no runtime to build, just the include path. The test
# faults for real (PROT_NONE page + sigaction), so it needs an x86-64 Linux
# host; elsewhere it exits 77 (skipped). tests/mmio_decode covers the same
# decoder on Windows, without a fault.
add_executable(mmio_decode_x64_posix_test test_main.c)
target_include_directories(mmio_decode_x64_posix_test PRIVATE ../../src/platform)
enable_testing()
add_test(NAME mmio_decode_x64_posix COMMAND mmio_decode_x64_posix_test)
set_tests_properties(mmio_decode_x64_posix PROPERTIES SKIP_RETURN_CODE 77)
