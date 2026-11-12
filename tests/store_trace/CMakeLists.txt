cmake_minimum_required(VERSION 3.20)
project(store_trace_test C)
# The store-trace runtime (src/kernel/recomp_store_trace.c) against the hooks
# the lifter emits under --trace-stores (templates/runtime/recomp_store_trace.h).
set(CMAKE_C_STANDARD 11)
# Under tests/CMakeLists.txt the whole suite shares one runtime build.
if(NOT TARGET xboxrecomp)
    add_subdirectory(../.. runtime)
endif()
add_executable(store_trace_test test_main.c)
target_include_directories(store_trace_test PRIVATE ../../templates/runtime ../../src/kernel ../../src)
target_link_libraries(store_trace_test PRIVATE xboxrecomp)
if(NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(store_trace_test PRIVATE Threads::Threads)
endif()
enable_testing()
add_test(NAME store_trace_test COMMAND store_trace_test ${CMAKE_CURRENT_BINARY_DIR}/store_trace_test.bin)
