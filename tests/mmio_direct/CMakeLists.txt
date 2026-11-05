cmake_minimum_required(VERSION 3.20)
project(mmio_direct_test C)
# The direct APU register path (xbox_MmioRead/xbox_MmioWrite) against a live
# memory layout. POSIX hosts with a trapped-MMIO decoder only (arm64, x86-64
# Linux): the layout init and the stderr capture are POSIX, and elsewhere the
# POSIX hook is an untraced stub, so the trace checks cannot pass. The Win32
# direct path is covered by the Proton bench runs (fails=0) until this
# harness is ported.
string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" mmio_cpu)
if(WIN32 OR NOT (mmio_cpu MATCHES "^(arm64|aarch64)$"
                 OR (CMAKE_SYSTEM_NAME STREQUAL "Linux"
                     AND mmio_cpu MATCHES "^(x86_64|amd64)$")))
    message(FATAL_ERROR "This regression runs on arm64 POSIX and x86-64 Linux hosts")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(mmio_direct_test test_main.c)
target_include_directories(mmio_direct_test PRIVATE
    ../../src ../../src/kernel ../../src/apu ../../src/nv2a ../../src/platform)
target_link_libraries(mmio_direct_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME mmio_direct
         COMMAND mmio_direct_test
                 ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
