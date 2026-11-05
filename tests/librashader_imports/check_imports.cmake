# cmake -DOBJDUMP=<objdump> -DDLL=<librashader.dll> -P check_imports.cmake
# Passes when DLL imports no dll a stock Windows 10/11 lacks. An empty DLL
# (the zip could not be downloaded) skips.
if(NOT DLL)
    message("librashader_imports: skipped, no librashader.dll (download failed)")
    return()
endif()
execute_process(COMMAND ${OBJDUMP} -p ${DLL}
                OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OBJDUMP} -p ${DLL} failed (${rc})")
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" names "${dump}")
set(bad "")
foreach(n IN LISTS names)
    string(REPLACE "DLL Name: " "" n "${n}")
    string(TOUPPER "${n}" n)
    # D3DX9: DirectX June 2010 runtime. MSVCP/VCRUNTIME: the VC++
    # Redistributable. LIBC++/LIBUNWIND: llvm-mingw's runtime dlls, which
    # nothing ships beside the game.
    if(n MATCHES "^(D3DX|MSVCP|VCRUNTIME|LIBC\\+\\+|LIBUNWIND|LIBGCC|LIBSTDC\\+\\+|LIBWINPTHREAD)")
        list(APPEND bad "${n}")
    endif()
endforeach()
list(LENGTH names count)
message(STATUS "librashader.dll imports ${count} dlls")
# Every Windows dll imports at least kernel32: none parsed means objdump's
# output changed shape, and an empty list must not pass as clean.
if(count EQUAL 0)
    message(FATAL_ERROR "no DLL Name lines in ${OBJDUMP} -p ${DLL}")
endif()
if(bad)
    message(FATAL_ERROR "librashader.dll imports ${bad}, absent on a stock Windows")
endif()
