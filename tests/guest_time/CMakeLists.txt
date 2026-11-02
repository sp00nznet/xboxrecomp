cmake_minimum_required(VERSION 3.20)
project(guest_time_test C)
# Cross-platform: guest_time.c and the timed waits in kernel_sync.c and
# kernel_thread.c run on both hosts, each under its own wait primitives.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(guest_time_test test_main.c)
target_include_directories(guest_time_test PRIVATE ../../src/kernel ../../src)
target_link_libraries(guest_time_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME guest_time_test COMMAND guest_time_test)
