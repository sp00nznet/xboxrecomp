cmake_minimum_required(VERSION 3.20)
project(kernel_summary_report_test C)
# Cross-platform: the bridge driven through its dispatcher over a plain buffer
# (as tests/kernel_bridge does), so the exit hooks are the Win32 ones under
# Proton and the POSIX ones elsewhere.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_summary_report_test test_main.c)
target_include_directories(kernel_summary_report_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_summary_report_test PRIVATE xboxrecomp)
enable_testing()
# Twice, because both gates are read once and cached: the default run, and
# one with RECOMP_TRACE=file,kernel_summary set before the first call.
add_test(NAME kernel_summary_report_test COMMAND kernel_summary_report_test)
add_test(NAME kernel_summary_report_traced COMMAND kernel_summary_report_test --traced)
