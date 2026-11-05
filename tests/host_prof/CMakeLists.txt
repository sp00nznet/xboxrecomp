cmake_minimum_required(VERSION 3.20)
project(host_prof_test C)
# The in-process sampler (src/platform/host_prof.c) needs only the host's
# thread calls, so the test builds the platform files it uses, on the hosts
# that have a sampler: Windows (and Wine) and macOS.
if(NOT WIN32 AND NOT APPLE)
    message(FATAL_ERROR "host_prof samples on Windows and macOS only")
endif()
set(CMAKE_C_STANDARD 11)
set(P ../../src/platform)
set(PROF_SRC ${P}/host_prof.c ${P}/recomp_env.c ${P}/recomp_exe_dir.c
    ${P}/recomp_exit_hook.c ${P}/host_time.c)
enable_testing()
# host_prof_test samples a spinner; host_prof_delay_test dumps during the
# start delay. One process each, since a dump ends the sampler for good.
foreach(t host_prof_test:test_main.c host_prof_delay_test:test_delay.c)
    string(REPLACE ":" ";" t ${t})
    list(GET t 0 name)
    list(GET t 1 src)
    add_executable(${name} ${src} ${PROF_SRC})
    target_include_directories(${name} PRIVATE ${P} ../../src)
    if(WIN32)
        target_link_libraries(${name} PRIVATE winmm)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endforeach()
