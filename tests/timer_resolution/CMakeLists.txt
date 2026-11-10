cmake_minimum_required(VERSION 3.20)
project(timer_resolution_test C)
# The 1 ms timer period xbox_kernel_init asks for (host_time.c,
# xbox_HostTimerResolutionInit). It needs only the host's own wait calls, so
# the test builds that file alone, on every host. The check that matters is
# native Windows, where a process that never asks waits in 15.6 ms steps;
# Wine and POSIX honour short waits either way. The same exe run by hand on
# native Windows is the check that the APU's pacing wait is not a tick.
set(CMAKE_C_STANDARD 11)
add_executable(timer_resolution_test test_main.c ../../src/platform/host_time.c)
target_include_directories(timer_resolution_test PRIVATE ../../src/platform)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(timer_resolution_test PRIVATE Threads::Threads)
endif()
enable_testing()
add_test(NAME timer_resolution_test COMMAND timer_resolution_test)
