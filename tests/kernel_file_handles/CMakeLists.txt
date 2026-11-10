cmake_minimum_required(VERSION 3.20)
project(kernel_file_handles_test C)
# The POSIX file handles: fd-backed win32_compat objects, the open-file
# limit, and what the file HLE answers for a handle that is not one.
include(${CMAKE_CURRENT_LIST_DIR}/../xr_test.cmake)
if(WIN32)
    xr_test_refuse("This regression covers the POSIX file-handle shim")
    return()
endif()
set(CMAKE_C_STANDARD 11)
# The whole runtime is instrumented, so the flags go in before it is added.
option(XBOX_FILE_HANDLES_ASAN "Instrument the fixture and the runtime" OFF)
if(XBOX_FILE_HANDLES_ASAN)
    # In the shared build (tests/CMakeLists.txt) the runtime is already
    # added without the flags, and a half-instrumented program misreports.
    if(XR_TEST_SHARED_RUNTIME)
        message(FATAL_ERROR "XBOX_FILE_HANDLES_ASAN needs its own runtime build: configure tests/kernel_file_handles on its own")
    endif()
    string(APPEND CMAKE_C_FLAGS " -fsanitize=address -fno-omit-frame-pointer")
    string(APPEND CMAKE_CXX_FLAGS " -fsanitize=address -fno-omit-frame-pointer")
    string(APPEND CMAKE_EXE_LINKER_FLAGS " -fsanitize=address")
endif()
find_package(Threads REQUIRED)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(kernel_file_handles_test test_main.c)
target_include_directories(kernel_file_handles_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_file_handles_test PRIVATE xboxrecomp Threads::Threads)
enable_testing()
add_test(NAME kernel_file_handles_test COMMAND kernel_file_handles_test)
