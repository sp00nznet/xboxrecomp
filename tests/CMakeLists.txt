# The test projects that link the whole runtime, in one build over one copy of
# it. Each of them adds the runtime itself (add_subdirectory(../.. runtime))
# under if(NOT TARGET xboxrecomp), so it still builds on its own; here the
# runtime is added first and the guard skips it. One build of the runtime is
# about 75 MB, and the suite had about 60 of them: 5 GB a ctest tree.
#
#   cmake -S tests -B build/tests && cmake --build build/tests
#   ctest --test-dir build/tests/<name>
#
# tests/posix_ctest.sh passes XR_TEST_SHARED_DIRS; with none, every dir with
# the guard joins. A dir that sets runtime options before adding the runtime
# (kernel_language, template_main) keeps the unguarded add and its own build.
# A harness that cannot run on this host calls xr_test_refuse (xr_test.cmake)
# and is listed in refused.txt in the build dir.
cmake_minimum_required(VERSION 3.20)
project(xboxrecomp_tests C)
set(CMAKE_C_STANDARD 11)

set(XR_TEST_SHARED_RUNTIME ON)
include(${CMAKE_CURRENT_LIST_DIR}/xr_test.cmake)

if(NOT DEFINED XR_TEST_SHARED_DIRS)
    file(GLOB _lists RELATIVE ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/*/CMakeLists.txt)
    set(XR_TEST_SHARED_DIRS)
    foreach(_list IN LISTS _lists)
        file(STRINGS ${CMAKE_CURRENT_LIST_DIR}/${_list} _guard REGEX "^if\\(NOT TARGET xboxrecomp\\)")
        if(_guard)
            get_filename_component(_dir ${_list} DIRECTORY)
            list(APPEND XR_TEST_SHARED_DIRS ${_dir})
        endif()
    endforeach()
endif()

add_subdirectory(.. runtime)
enable_testing()
foreach(_dir IN LISTS XR_TEST_SHARED_DIRS)
    add_subdirectory(${_dir} ${_dir})
endforeach()

file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/refused.txt "")
get_property(_refused GLOBAL PROPERTY XR_TEST_REFUSED)
foreach(_name IN LISTS _refused)
    get_property(_msg GLOBAL PROPERTY XR_TEST_REFUSED_${_name})
    file(APPEND ${CMAKE_CURRENT_BINARY_DIR}/refused.txt "${_name}\t${_msg}\n")
endforeach()
