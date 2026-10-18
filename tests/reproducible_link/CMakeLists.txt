cmake_minimum_required(VERSION 3.20)
project(reproducible_link C)
enable_testing()

# Builds a small game (an executable linking a static library and the
# toolkit's reproducible-build flags, cmake/reproducible.cmake) in two
# folders at different times and requires equal bytes: the program, and on
# Windows the PDB and the GUID that ties them. A packager trusts a rebuild
# only when it equals a clean build of the same inputs.
get_filename_component(TK_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(_args
    -DTK_ROOT=${TK_ROOT}
    -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixture
    "-DGEN=${CMAKE_GENERATOR}")

add_test(NAME reproducible-native
    COMMAND ${CMAKE_COMMAND} ${_args} -DMODE=native
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/native
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)

# llvm-mingw from LLVM_MINGW_ROOT (or on PATH); skipped without one.
add_test(NAME reproducible-mingw
    COMMAND ${CMAKE_COMMAND} ${_args} -DMODE=mingw
            -DWORK=${CMAKE_CURRENT_BINARY_DIR}/mingw
            -P ${CMAKE_CURRENT_SOURCE_DIR}/check.cmake)
set_tests_properties(reproducible-mingw PROPERTIES SKIP_RETURN_CODE 77)
