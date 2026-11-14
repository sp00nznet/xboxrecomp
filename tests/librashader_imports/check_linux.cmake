# cmake -DOBJDUMP=<objdump> -DSO=<librashader.so> [-DREQUIRED=1] \
#       -P check_linux.cmake
# Passes when the x86-64 Linux librashader.so loads in the Steam Linux
# Runtime sniper beside a dxvk-native game: NEEDED only glibc's libraries,
# libgcc_s and libvkd3d-shader.so.1 (the bundle's lib/), no GLIBC_ version
# above sniper's 2.31 and no GLIBCXX_ or CXXABI_ (libstdc++ is linked in
# statically). An empty SO (the zip could not be downloaded) skips, unless
# REQUIRED: a dxvk-native build stages this library into its bundle, and a
# skip there would let a bundle ship without the library its README names.
if(NOT SO)
    if(REQUIRED)
        message(FATAL_ERROR "no librashader.so: the x86_64-linux zip was not"
                " downloaded, so a dxvk-native bundle would ship without it")
    endif()
    message("librashader_linux_needs: skipped, no librashader.so (download failed)")
    return()
endif()
execute_process(COMMAND ${OBJDUMP} -p ${SO}
                OUTPUT_VARIABLE dump RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "${OBJDUMP} -p ${SO} failed (${rc})")
endif()
set(allowed libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1
    ld-linux-x86-64.so.2 libgcc_s.so.1 libvkd3d-shader.so.1)
string(REGEX MATCHALL "NEEDED +[^ \r\n]+" needed "${dump}")
set(bad "")
set(names "")
foreach(n IN LISTS needed)
    string(REGEX REPLACE "NEEDED +" "" n "${n}")
    list(APPEND names "${n}")
    if(NOT n IN_LIST allowed)
        list(APPEND bad "${n}")
    endif()
endforeach()
# Every shared library needs libc: none parsed means objdump's output
# changed shape, and an empty list must not pass as clean.
if(NOT "libc.so.6" IN_LIST names)
    message(FATAL_ERROR "no NEEDED libc.so.6 in ${OBJDUMP} -p ${SO}")
endif()
if(NOT "libvkd3d-shader.so.1" IN_LIST names)
    message(FATAL_ERROR "${SO} does not need libvkd3d-shader.so.1: no D3DCompile")
endif()
if(bad)
    message(FATAL_ERROR "librashader.so needs ${bad}, neither glibc's nor bundled")
endif()
string(REGEX MATCHALL "(GLIBC|GLIBCXX|CXXABI)_[0-9.]+" vers "${dump}")
list(REMOVE_DUPLICATES vers)
set(max "")
foreach(v IN LISTS vers)
    if(v MATCHES "^(GLIBCXX|CXXABI)_")
        list(APPEND bad "${v}")
        continue()
    endif()
    string(REPLACE "GLIBC_" "" n "${v}")
    if(n VERSION_GREATER 2.31)
        list(APPEND bad "${v}")
    endif()
    if(NOT max OR n VERSION_GREATER max)
        set(max "${n}")
    endif()
endforeach()
if(bad)
    message(FATAL_ERROR "librashader.so asks for ${bad}: above sniper's glibc 2.31,"
            " or a shared libstdc++")
endif()
list(JOIN names " " names)
message(STATUS "librashader.so needs ${names}; newest glibc symbol GLIBC_${max}")
