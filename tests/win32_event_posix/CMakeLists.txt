cmake_minimum_required(VERSION 3.20)
project(win32_event_posix_test C)
# The Win32 event shim on a POSIX host. Only the platform library: no guest
# window and no kernel, since these are the host primitives the kernel sits on.
if(WIN32)
    message(FATAL_ERROR "This regression covers the POSIX event shim")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../../src/platform platform)
add_executable(win32_event_posix_test test_main.c)
target_link_libraries(win32_event_posix_test PRIVATE platform)
enable_testing()
add_test(NAME win32_event_posix_test COMMAND win32_event_posix_test)
