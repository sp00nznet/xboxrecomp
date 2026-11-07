# present_shaders_auto - present.shader reaches a game with no line of its own
# (cmake/librashader.cmake). A small game project is configured and built at
# test time, since what is tested is what CMake does with its targets.
cmake_minimum_required(VERSION 3.20)
project(present_shaders_auto C)
enable_testing()
get_filename_component(_toolkit "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
add_test(NAME present_shaders_auto
         COMMAND ${CMAKE_COMMAND}
                 -DTOOLKIT=${_toolkit}
                 -DFIXTURE=${CMAKE_CURRENT_SOURCE_DIR}/fixture
                 -DWORK=${CMAKE_CURRENT_BINARY_DIR}/work
                 -DCC=${CMAKE_C_COMPILER}
                 -DGEN=${CMAKE_GENERATOR}
                 -P ${CMAKE_CURRENT_SOURCE_DIR}/run.cmake)
