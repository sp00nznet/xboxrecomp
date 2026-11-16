cmake_minimum_required(VERSION 3.20)
project(pgraph_trap_test C)
# Windows only in substance: the trap's write-1-to-clear page is a Windows
# vectored exception handler. Elsewhere the test checks that traps stay off.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(pgraph_trap_test test_main.c)
target_include_directories(pgraph_trap_test PRIVATE ../../src/kernel)
target_link_libraries(pgraph_trap_test PRIVATE xboxrecomp)
enable_testing()

set(XBE ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/conformance/test.xbe)
# One process per mode: the switches are read once and cached.
add_test(NAME pgraph_trap_on COMMAND pgraph_trap_test on ${XBE})
add_test(NAME pgraph_trap_off COMMAND pgraph_trap_test off ${XBE})
