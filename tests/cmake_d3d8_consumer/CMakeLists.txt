cmake_minimum_required(VERSION 3.20)
project(cmake_d3d8_consumer C)

set(XBOXRECOMP_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../.." CACHE PATH
    "Path to the xboxrecomp source tree")
set(XBOXRECOMP_D3D8_TEST_MODE "DISABLED" CACHE STRING
    "D3D8 consumer mode: DISABLED or EXTERNAL")
set_property(CACHE XBOXRECOMP_D3D8_TEST_MODE PROPERTY STRINGS DISABLED EXTERNAL)

set(CMAKE_DISABLE_FIND_PACKAGE_PkgConfig ON CACHE BOOL
    "The fixture must not discover libepoxy through pkg-config" FORCE)

if(XBOXRECOMP_D3D8_TEST_MODE STREQUAL "DISABLED")
    set(XBOXRECOMP_BUILD_D3D8 OFF CACHE BOOL "" FORCE)
elseif(XBOXRECOMP_D3D8_TEST_MODE STREQUAL "EXTERNAL")
    add_library(xbox_d3d8 INTERFACE)
else()
    message(FATAL_ERROR
        "XBOXRECOMP_D3D8_TEST_MODE must be DISABLED or EXTERNAL")
endif()

add_subdirectory("${XBOXRECOMP_SOURCE_DIR}" xboxrecomp)

if(XBOXRECOMP_D3D8_TEST_MODE STREQUAL "DISABLED" AND TARGET xbox_d3d8)
    message(FATAL_ERROR "The bundled xbox_d3d8 target was not disabled")
endif()
if(XBOXRECOMP_D3D8_TEST_MODE STREQUAL "EXTERNAL" AND NOT TARGET xbox_d3d8)
    message(FATAL_ERROR "The external xbox_d3d8 target was not retained")
endif()

add_executable(cmake_d3d8_consumer main.c)
target_link_libraries(cmake_d3d8_consumer PRIVATE xboxrecomp)
