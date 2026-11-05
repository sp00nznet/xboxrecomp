cmake_minimum_required(VERSION 3.20)
project(kernel_user_apc_test C)
# The deferred file-I/O APC queue (kernel_user_apc.c) holds guest values and
# runs nothing itself, so the test builds that file alone, on every host.
set(CMAKE_C_STANDARD 11)
find_package(Threads REQUIRED)
add_executable(kernel_user_apc_test test_main.c ../../src/kernel/kernel_user_apc.c)
target_include_directories(kernel_user_apc_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_user_apc_test PRIVATE Threads::Threads)
enable_testing()
add_test(NAME kernel_user_apc_test COMMAND kernel_user_apc_test)
