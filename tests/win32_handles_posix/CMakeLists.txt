cmake_minimum_required(VERSION 3.20)
project(win32_handles_posix_test C)
# A HANDLE the POSIX shim issues has to survive being stored by a 32-bit
# guest. On Windows this is free -- HANDLEs are handle-table indices -- so
# there is nothing here for that host to check.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX handle representation")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(win32_handles_posix_test test_main.c)
target_include_directories(win32_handles_posix_test PRIVATE ../../src/kernel)
target_link_libraries(win32_handles_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME win32_handles_posix_test COMMAND win32_handles_posix_test)
