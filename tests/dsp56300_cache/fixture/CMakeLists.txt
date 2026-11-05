# Calls xboxrecomp_dsp56300 with the release URL pointed at SOURCE, a local
# directory holding the release tarball.
cmake_minimum_required(VERSION 3.20)
project(dsp56300_cache_fixture NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/dsp56300.cmake)
set(_dsp56300_url_base "file://${SOURCE}")
xboxrecomp_dsp56300(target why)
if(NOT target)
    message(FATAL_ERROR "no dsp56300 target: ${why}")
endif()
