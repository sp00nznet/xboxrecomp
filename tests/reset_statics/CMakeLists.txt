cmake_minimum_required(VERSION 3.20)
project(reset_statics_test NONE)
# The in-window reset rests on docs/reset-statics.md classifying every
# mutable static in the tree: a new static without a row would survive a
# reset unseen. The check is stdlib-only, so it runs in every ctest pass;
# the scanner's own unit tests need pytest and skip (77) without it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
enable_testing()
set(root ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME reset_statics_doc
    COMMAND ${Python3_EXECUTABLE} ${root}/tools/reset_statics.py check ${root})
add_test(NAME reset_statics_unit
    COMMAND ${Python3_EXECUTABLE} -c
        "import sys\ntry:\n import pytest\nexcept ImportError:\n sys.exit(77)\nsys.exit(pytest.main(['-q', '-p', 'no:cacheprovider', sys.argv[1]]))"
        ${root}/tools/test_reset_statics.py
    WORKING_DIRECTORY ${root})
set_tests_properties(reset_statics_unit PROPERTIES SKIP_RETURN_CODE 77)
