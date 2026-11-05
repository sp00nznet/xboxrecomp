# shader_cache - the D3D11 backend's on-disk bytecode cache (src/d3d/shader_cache.c):
# the key, a round trip, damaged and stale entries, a key collision and the
# store's temporary files. Plain file I/O, no D3D, so it runs on every host.
#   cmake --build <build> --target shader_cache_test
add_executable(shader_cache_test test_main.c)
target_link_libraries(shader_cache_test PRIVATE xbox_d3d8)
# Native on POSIX; cross-built for Windows, ctest runs it through
# CMAKE_CROSSCOMPILING_EMULATOR (tests/proton_run.sh) when one is configured.
enable_testing()
add_test(NAME shader_cache_test COMMAND shader_cache_test)
