cmake_minimum_required(VERSION 3.20)
project(lf_attrs_test NONE)
# Every tracked file a game's package step wants LF (xboxrecomp-cli
# package/lib.py LF_ONLY) must keep LF in a Windows checkout with Git for
# Windows' default core.autocrlf=true: .gitattributes has to say eol=lf for
# it. Skipped (77) outside a git checkout.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
add_test(NAME lf_attrs
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/check_lf_attrs.py
            ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set_tests_properties(lf_attrs PROPERTIES SKIP_RETURN_CODE 77)
