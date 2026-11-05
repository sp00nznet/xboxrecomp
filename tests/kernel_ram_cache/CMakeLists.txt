cmake_minimum_required(VERSION 3.20)
project(kernel_ram_cache_test C)
# Cross-platform: the xbox_Nt* file functions called directly, so the store
# runs behind the Win32 backend on Windows/Proton and the POSIX one elsewhere.
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
add_executable(kernel_ram_cache_test test_main.c)
target_include_directories(kernel_ram_cache_test PRIVATE ../../src/kernel)
target_link_libraries(kernel_ram_cache_test PRIVATE xboxrecomp)
enable_testing()
add_test(NAME kernel_ram_cache_test COMMAND kernel_ram_cache_test)
# The store is on by default; these turn it off, one way per process since
# the store fixes on or off at its first use: the variable at 0 and at false,
# and xbox_RamCacheEnable(0) with the variable unset (what enhance.c does for
# `ram_cache = false`). The last one gives the variable a word that is not a
# bool: a warning, and the store stays on. over_cap seeds a Cache dir past
# the cap, under_cap one that fits, dir_over_cap a directory past it.
foreach(v off_0 off_false off_enable bad_value over_cap under_cap dir_over_cap)
    add_executable(kernel_ram_cache_${v}_test test_main.c)
    target_include_directories(kernel_ram_cache_${v}_test PRIVATE ../../src/kernel)
    target_link_libraries(kernel_ram_cache_${v}_test PRIVATE xboxrecomp)
    add_test(NAME kernel_ram_cache_${v}_test COMMAND kernel_ram_cache_${v}_test)
endforeach()
target_compile_definitions(kernel_ram_cache_off_0_test PRIVATE RAMCACHE_OFF_VALUE="0")
target_compile_definitions(kernel_ram_cache_off_false_test PRIVATE RAMCACHE_OFF_VALUE="false")
target_compile_definitions(kernel_ram_cache_off_enable_test PRIVATE RAMCACHE_OFF_ENABLE=1)
target_compile_definitions(kernel_ram_cache_bad_value_test PRIVATE RAMCACHE_BAD_VALUE="maybe")
target_compile_definitions(kernel_ram_cache_over_cap_test PRIVATE RAMCACHE_OVER_CAP=1)
target_compile_definitions(kernel_ram_cache_under_cap_test PRIVATE RAMCACHE_UNDER_CAP=1)
target_compile_definitions(kernel_ram_cache_dir_over_cap_test PRIVATE RAMCACHE_DIR_OVER_CAP=1)
