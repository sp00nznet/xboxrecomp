cmake_minimum_required(VERSION 3.20)
project(platform C)

# Platform layer
#   xbox_winnt.h   - Win32 / NT type vocabulary (header only)
#   win32_compat.* - POSIX implementation of generic Win32 host primitives
#
# On Windows the real <windows.h> is used, so this is an INTERFACE library
# that only contributes the src/ include path. On Linux it is a real static
# library carrying the POSIX primitive implementations.

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# src/, the directory this one lives in. Resolved relative to here rather than
# via CMAKE_SOURCE_DIR, which points at the *consuming* project's root when a
# game project pulls xboxrecomp in with add_subdirectory() -- the documented
# integration pattern -- and would send "platform/xbox_winnt.h" looking in the
# game's own src/ tree. PROJECT_SOURCE_DIR is no good either: this file
# declares its own project().
get_filename_component(XBOXRECOMP_SRC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# recomp_env: every environment variable the runtime reads, read once
# (recomp_env.h). A real library on every host; platform carries it to all
# of the runtime's libraries, so the host clock (host_time.h) and the window
# exit hook (recomp_exit_hook.h) ride with it.
add_library(recomp_env STATIC recomp_env.c recomp_exe_dir.c recomp_exit_hook.c host_time.c)
target_include_directories(recomp_env PUBLIC
    ${XBOXRECOMP_SRC_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
)

if(WIN32)
    add_library(platform INTERFACE)
    target_link_libraries(platform INTERFACE recomp_env)
    target_include_directories(platform INTERFACE ${XBOXRECOMP_SRC_DIR})
else()
    add_library(platform STATIC win32_compat.c)
    target_include_directories(platform PUBLIC
        ${XBOXRECOMP_SRC_DIR}
        ${CMAKE_CURRENT_SOURCE_DIR}
    )
    find_package(Threads REQUIRED)
    target_link_libraries(platform PUBLIC Threads::Threads recomp_env)
    if(NOT APPLE)
        target_link_libraries(platform PUBLIC m)   # feclearexcept etc.; libSystem has them on macOS
    endif()
    target_compile_options(platform PRIVATE -Wall -Wextra -Wno-unused-parameter -O2)
    # glibc declares pthread_setname_np, pthread_[gs]etaffinity_np, the
    # CPU_* set macros and dladdr only under _GNU_SOURCE, and a header that
    # pulls in <pthread.h> first (win32_compat.h) settles it for the whole
    # file, so a #define in one .c is too late for the others. Every target
    # that links the platform layer gets it on the command line.
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_compile_definitions(platform PUBLIC _GNU_SOURCE)
    endif()
endif()
