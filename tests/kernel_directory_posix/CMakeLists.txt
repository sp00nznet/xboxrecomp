cmake_minimum_required(VERSION 3.20)
project(kernel_directory_posix C)
if(WIN32)
    message(FATAL_ERROR "This fixture exercises the POSIX directory-search backend")
endif()
set(CMAKE_C_STANDARD 11)
# Build only real file/path and platform code: no game, renderer, guest-memory
# layout or #189 prerequisite is needed to test directory-handle lifetime.
add_subdirectory(../../src/platform platform)
add_executable(kernel_directory_posix_test test_main.c
    ../../src/kernel/kernel_file.c ../../src/kernel/kernel_ramfs.c
    ../../src/kernel/kernel_path.c)
target_include_directories(kernel_directory_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_directory_posix_test PRIVATE platform)
option(XBOX_DIRECTORY_ASAN "Instrument the fixture and its platform library" OFF)
if(XBOX_DIRECTORY_ASAN)
    target_compile_options(platform PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_compile_options(kernel_directory_posix_test PRIVATE -fsanitize=address -fno-omit-frame-pointer)
    target_link_options(kernel_directory_posix_test PRIVATE -fsanitize=address)
endif()
enable_testing()
add_test(NAME kernel_directory_posix COMMAND kernel_directory_posix_test)
