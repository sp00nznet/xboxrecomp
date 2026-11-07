cmake_minimum_required(VERSION 3.20)
project(present_shaders_fixture C)
option(XBOXRECOMP_ENHANCE "" OFF)
# The toolkit comes in as a subdirectory, as every game adds it, so the scan
# is deferred from there onto this top-level directory.
add_subdirectory(toolkit)
add_subdirectory(game)
