cmake_minimum_required(VERSION 3.20)
project(kernel_memory_posix_test C)
# The kernel's memory APIs on a POSIX host. Separate from memory_layout_posix
# because these need no mapped guest window: they are the Mm* and Nt* calls a
# title makes, sitting on the Win32 shim rather than on the Xbox layout.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX memory-API path")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_memory_posix_test test_main.c)
target_include_directories(kernel_memory_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_memory_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_memory_posix_test COMMAND kernel_memory_posix_test)
