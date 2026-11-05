# dsp56300_migrate - xboxrecomp_dsp56300_migrate (cmake/dsp56300.cmake): a
# build tree configured when XBOXRECOMP_APU_DSP defaulted OFF turns ON once,
# and every OFF chosen on purpose is kept. run.cmake configures a small
# fixture project in a scratch tree, as the old toolkit and then the new
# one would. No compiler, no download:
#   cmake -S tests/dsp56300_migrate -B build/dsp56300_migrate
#   ctest --test-dir build/dsp56300_migrate
cmake_minimum_required(VERSION 3.20)
project(dsp56300_migrate_test NONE)
enable_testing()
add_test(NAME dsp56300_migrate
    COMMAND ${CMAKE_COMMAND}
        -DFIXTURE=${CMAKE_CURRENT_LIST_DIR}/fixture
        -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
        -P ${CMAKE_CURRENT_LIST_DIR}/run.cmake)
