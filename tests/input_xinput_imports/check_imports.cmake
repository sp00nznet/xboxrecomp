# cmake -DOBJDUMP=<objdump> -DEXE=<exe> -P check_imports.cmake
# Passes when EXE imports XINPUT1_4.dll and no other XInput DLL.
execute_process(COMMAND ${OBJDUMP} -p ${EXE}
                OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OBJDUMP} -p ${EXE} failed (${rc})")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" names "${dump}")
set(xinput "")
foreach(n IN LISTS names)
    string(REPLACE "DLL Name: " "" n "${n}")
    string(TOUPPER "${n}" n)
    if(n MATCHES "^XINPUT")
        list(APPEND xinput "${n}")
    endif()
endforeach()
message(STATUS "XInput imports: ${xinput}")
if(NOT xinput STREQUAL "XINPUT1_4.DLL")
    message(FATAL_ERROR "expected only XINPUT1_4.DLL, got '${xinput}': "
        "XINPUT1_3.dll is the DirectX redistributable, absent on stock Windows")
endif()
