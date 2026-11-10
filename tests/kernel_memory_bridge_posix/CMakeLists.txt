cmake_minimum_required(VERSION 3.20)
project(kernel_memory_bridge_posix_test C)
# Kernel memory and file bridges, driven through the thunk dispatcher against
# a real mapped guest window (xbox_MemoryLayoutInit on the synthetic XBE), so
# the contiguous window at 0x80000000 exists. POSIX-only because it points the
# save directory at a temporary one through XDG_DATA_HOME.
include(${CMAKE_CURRENT_LIST_DIR}/../xr_test.cmake)
if(WIN32)
    xr_test_refuse("This regression covers the POSIX host")
    return()
endif()
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(kernel_memory_bridge_posix_test test_main.c)
target_include_directories(kernel_memory_bridge_posix_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_memory_bridge_posix_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_memory_bridge_posix_test
         COMMAND kernel_memory_bridge_posix_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# The same checks through upstream's RECOMP_HEAP_RECLAIM path.
add_test(NAME kernel_memory_bridge_posix_reclaim
         COMMAND kernel_memory_bridge_posix_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
set_tests_properties(kernel_memory_bridge_posix_reclaim
                     PROPERTIES ENVIRONMENT RECOMP_HEAP_RECLAIM=1)
