# dsp56300_cache - the shared download cache of xboxrecomp_dsp56300
# (cmake/dsp56300.cmake): a cached tarball whose sha256 is not the pin is
# fetched again, and .part-*/.extract-* debris older than an hour is swept
# while a young one (a live configure's) stays. run.cmake configures a small
# fixture against a scratch cache, "downloading" by file:// from the suite's
# filled cache (XBOXRECOMP_DSP56300_CACHE, which posix_ctest.sh passes), so
# no network. Skipped when that cache has no tarball for this host.
#   cmake -S tests/dsp56300_cache -B build/dsp56300_cache \
#       -DXBOXRECOMP_DSP56300_CACHE=<a cache holding the tarball>
#   ctest --test-dir build/dsp56300_cache
cmake_minimum_required(VERSION 3.20)
project(dsp56300_cache_test NONE)
enable_testing()
add_test(NAME dsp56300_cache
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE=${CMAKE_CURRENT_LIST_DIR}/fixture
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
        "-DSOURCE=${XBOXRECOMP_DSP56300_CACHE}"
        -P ${CMAKE_CURRENT_LIST_DIR}/run.cmake)
set_tests_properties(dsp56300_cache PROPERTIES SKIP_RETURN_CODE 77)
