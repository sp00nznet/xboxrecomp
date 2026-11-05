# STAGE=old declares the option as the toolkit did before the JIT default; any other
# stage is today's toolkit: the migration, then option(). HAS_ARCHIVE stands
# in for _xboxrecomp_dsp56300_triple. The result goes to result.txt.
cmake_minimum_required(VERSION 3.20)
project(dsp56300_migrate_fixture NONE)
include(${CMAKE_CURRENT_LIST_DIR}/../../../cmake/dsp56300.cmake)
if(STAGE STREQUAL "old")
    option(XBOXRECOMP_APU_DSP "${_dsp56300_old_help}" OFF)
else()
    if(NOT DEFINED HAS_ARCHIVE)
        set(HAS_ARCHIVE ON)
    endif()
    xboxrecomp_dsp56300_migrate(${HAS_ARCHIVE})
    option(XBOXRECOMP_APU_DSP "${XBOXRECOMP_APU_DSP_HELP}" ${HAS_ARCHIVE})
endif()
file(WRITE ${CMAKE_BINARY_DIR}/result.txt "${XBOXRECOMP_APU_DSP}")
