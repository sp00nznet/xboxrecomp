cmake_minimum_required(VERSION 3.20)
project(repro_game C)

# A game's shape: its sources, a toolkit library from another folder, the
# umbrella interface target carrying the flags.
add_library(xr INTERFACE)
include(${TK_ROOT}/cmake/reproducible.cmake)
xboxrecomp_reproducible(xr ${FAKE_TK})
add_subdirectory(${FAKE_TK} ${CMAKE_BINARY_DIR}/tk)
add_executable(repro_game main.c)
target_link_libraries(repro_game PRIVATE tk_lib xr)
if(MINGW)
    # As a game asks for its PDB: lld names it after the output.
    target_link_options(repro_game PRIVATE -Wl,--pdb=)
endif()
