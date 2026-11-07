# Stands in for the toolkit: the layer, the umbrella every game links, and
# the module under test.
if(XBOXRECOMP_ENHANCE)
    add_library(xbox_enhance INTERFACE)
endif()
add_library(xboxrecomp INTERFACE)
if(TARGET xbox_enhance)
    target_link_libraries(xboxrecomp INTERFACE xbox_enhance)
endif()
include(${TOOLKIT}/cmake/librashader.cmake)
# One of the toolkit's own test harnesses: it links the umbrella but has no
# menu, so it is left alone unless named.
add_executable(harness ${CMAKE_CURRENT_SOURCE_DIR}/../game/main.c)
target_link_libraries(harness PRIVATE xboxrecomp)
if(CALL_HARNESS)
    xboxrecomp_present_shaders(harness)
endif()
