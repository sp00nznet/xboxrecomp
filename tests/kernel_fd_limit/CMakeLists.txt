cmake_minimum_required(VERSION 3.20)
project(kernel_fd_limit_test C)
# The POSIX open-file limit: raised at kernel init, and what the file HLE
# answers when it runs out anyway.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX open-file limit")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_fd_limit_test test_main.c)
target_include_directories(kernel_fd_limit_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_fd_limit_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_fd_limit_test COMMAND kernel_fd_limit_test)
