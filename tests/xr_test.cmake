# Helpers for the standalone test projects under tests/.
#
# xr_test_refuse(<message>): this harness cannot run on this host. Follow it
# with return(). On its own the project stops with a configure error, which
# posix_ctest.sh reports as n/a. Inside the shared build (tests/CMakeLists.txt)
# an error would stop every other test's configure too, so the refusal is
# only recorded there and the return() skips this one directory.
include_guard(GLOBAL)

function(xr_test_refuse msg)
    if(XR_TEST_SHARED_RUNTIME)
        get_filename_component(_name "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
        # The name and the message apart: a message may hold a semicolon.
        set_property(GLOBAL APPEND PROPERTY XR_TEST_REFUSED ${_name})
        set_property(GLOBAL PROPERTY XR_TEST_REFUSED_${_name} "${msg}")
    else()
        message(FATAL_ERROR "${msg}")
    endif()
endfunction()
