cmake_minimum_required(VERSION 3.20)
project(host_nap_test C)
# The poll loop's nap (host_time.c) needs only the host's own clock and wait
# calls, so the test builds that file alone, on every host. The same exe run
# by hand on native Windows is the check that a 50 us nap is not a timer tick.
set(CMAKE_C_STANDARD 11)
add_executable(host_nap_test test_main.c ../../src/platform/host_time.c)
target_include_directories(host_nap_test PRIVATE ../../src/platform)
if(WIN32)
    # timeBeginPeriod(1), as the runtime's timer thread sets it.
    target_link_libraries(host_nap_test PRIVATE winmm)
endif()
enable_testing()
add_test(NAME host_nap_test COMMAND host_nap_test)
