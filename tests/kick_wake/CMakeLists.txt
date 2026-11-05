cmake_minimum_required(VERSION 3.20)
project(kick_wake_test C)
# The walker's event wait (nv2a_kick_wake.c) over plain words and the host
# waker (host_time.c): no runtime, so it builds alone, on every host. A
# mingw build run under tests/proton_run.sh covers both Windows waker forms
# (the native one included: Wine implements its event and timer).
set(CMAKE_C_STANDARD 11)
if(MSVC)
    # The test and nv2a_kick_wake.c use the __atomic builtins: gcc or clang
    # (mingw for Windows), not cl.exe.
    message(FATAL_ERROR "kick_wake_test needs gcc or clang (mingw on Windows)")
endif()
add_executable(kick_wake_test test_main.c
    ../../src/kernel/nv2a_kick_wake.c ../../src/platform/host_time.c)
target_include_directories(kick_wake_test PRIVATE ../../src ../../src/kernel ../../src/platform)
if(WIN32)
    # timeBeginPeriod(1) in host_time.c, as the runtime's timer thread sets it.
    target_link_libraries(kick_wake_test PRIVATE winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(kick_wake_test PRIVATE Threads::Threads)
endif()
enable_testing()
add_test(NAME kick_wake_test COMMAND kick_wake_test)
