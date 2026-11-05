cmake_minimum_required(VERSION 3.20)
project(mem_stats_test C)
# The [MEM] line (src/platform/mem_stats.c): its formatter and parser, a live
# reading of this process, and the SIGINT path a bench's stop takes. Builds
# the platform files it uses, on every host.
set(CMAKE_C_STANDARD 11)
set(P ../../src/platform)
add_executable(mem_stats_test test_main.c ${P}/mem_stats.c ${P}/recomp_env.c
    ${P}/recomp_exe_dir.c ${P}/recomp_exit_hook.c ${P}/host_time.c)
target_include_directories(mem_stats_test PRIVATE ${P} ../../src)
if(WIN32)
    target_link_libraries(mem_stats_test PRIVATE winmm)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(mem_stats_test PRIVATE Threads::Threads)
endif()
enable_testing()
add_test(NAME mem_stats_test COMMAND mem_stats_test)
