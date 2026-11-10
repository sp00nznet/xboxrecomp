# Mirrors SRC (assets/shaders) into DST (<exe dir>/shaders), writing only the
# files whose content differs; run by cmake/librashader.cmake at every build.
#
#   cmake -DSRC=<dir> -DDST=<dir> -P present_shaders_copy.cmake
#
# copy_directory rewrote all of shaders/ on every build, so a no-op build
# still touched every staged file and packaging saw them all as changed.
# Globbing here, at build time, also picks up a preset added since the last
# configure. Files in DST that SRC lacks are kept: a player may drop presets
# of their own beside the bundled ones.
if(NOT SRC OR NOT DST)
    message(FATAL_ERROR "present_shaders_copy: SRC and DST are required")
endif()
# A moved assets dir would otherwise glob nothing and stage nothing silently.
if(NOT IS_DIRECTORY "${SRC}")
    message(FATAL_ERROR "present_shaders_copy: ${SRC} is not a directory")
endif()
# FOLLOW_SYMLINKS as copy_directory did: without it a linked directory is
# listed as one entry, and configure_file refuses a directory.
file(GLOB_RECURSE _files FOLLOW_SYMLINKS RELATIVE "${SRC}" "${SRC}/*")
foreach(_f IN LISTS _files)
    # COPYONLY writes the output only when its content changes, so an
    # unchanged staged file keeps its timestamp.
    configure_file("${SRC}/${_f}" "${DST}/${_f}" COPYONLY)
endforeach()
