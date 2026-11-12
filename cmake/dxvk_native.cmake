# dxvk-native: DXVK's D3D11 and DXGI as Linux shared libraries, with the
# Windows headers they speak (zlib; the headers mingw-w64's), for the
# XBOXRECOMP_DXVK_NATIVE build (openspec linux-native-vulkan D2). The D3D11
# backend (nv2a_pb_d3d11.c) compiles against these headers and links these
# libraries, so Linux runs the backend Windows and Proton run, on Vulkan.
#
# The release's steamrt-sniper build is pinned by version and sha256, as
# cmake/dsp56300.cmake pins its archives: 3.1.1 is the DXVK GE-Proton11-7
# carries (v3.1-19), so the native build and Proton translate the same
# D3D11 calls. The sniper SDK sysroot's own dxvk-native (2.7.1) is not used:
# Proton parity is the point of this build. The HLSL compiler, vkd3d-shader,
# comes from that sysroot instead (2.1, Proton's d3dcompiler_47 version; see
# the SDK image digest in tools/linux/Dockerfile.sysroot).
#
#   xboxrecomp_dxvk_native(<root-var>)
#
# Defines the imported targets dxvk::d3d11 and dxvk::dxgi (headers in
# <root>/usr/include/dxvk) and sets <root-var> to the extracted tree, whose
# usr/lib holds the .so files a package ships beside the game. A failed
# download or a sha256 mismatch is a configure error.
#
# Cache variables:
#   XBOXRECOMP_DXVK_NATIVE_ARCHIVE  a local copy of the release tarball
#                                   (offline builds); still sha-checked
#   XBOXRECOMP_DXVK_NATIVE_CACHE    where the download and the extracted tree
#                                   go (default <build>/_deps/dxvk-native)
#
# The download, stamp and rename steps are dsp56300.cmake's helpers (the
# top-level CMakeLists includes it first): the same rules for a cache trees
# may share.

set(XBOXRECOMP_DXVK_NATIVE_VERSION 3.1.1)
set(_dxvk_native_name dxvk-native-${XBOXRECOMP_DXVK_NATIVE_VERSION}-steamrt-sniper)
set(_dxvk_native_url
    "https://github.com/doitsujin/dxvk/releases/download/v${XBOXRECOMP_DXVK_NATIVE_VERSION}/${_dxvk_native_name}.tar.gz")
set(_dxvk_native_sha256
    506fa1ea5bb6ce9a89e11436f7ab5191bc0196e8a577e0cf0b16a45bbc9991b7)

set(XBOXRECOMP_DXVK_NATIVE_ARCHIVE "" CACHE FILEPATH
    "Local dxvk-native release tarball (default: download it)")
set(XBOXRECOMP_DXVK_NATIVE_CACHE "${CMAKE_BINARY_DIR}/_deps/dxvk-native" CACHE PATH
    "Download and extraction directory for the dxvk-native release")

function(xboxrecomp_dxvk_native out_root)
    set(name ${_dxvk_native_name})
    set(sha ${_dxvk_native_sha256})
    set(cache "${XBOXRECOMP_DXVK_NATIVE_CACHE}")
    set(tarball "${cache}/${name}.tar.gz")
    set(root "${cache}/${name}")
    set(stamp "${root}/.xboxrecomp-sha256")
    if(TARGET dxvk::d3d11)
        set(${out_root} "${root}" PARENT_SCOPE)
        return()
    endif()
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux"
       OR NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
        message(FATAL_ERROR "XBOXRECOMP_DXVK_NATIVE: dxvk-native ${XBOXRECOMP_DXVK_NATIVE_VERSION} "
            "is pinned for x86_64 Linux only, not ${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR}")
    endif()

    _xboxrecomp_dsp56300_stamp("${stamp}" have)
    if(NOT have STREQUAL sha)
        file(MAKE_DIRECTORY "${cache}")
        set(downloaded FALSE)
        if(XBOXRECOMP_DXVK_NATIVE_ARCHIVE)
            set(tarball "${XBOXRECOMP_DXVK_NATIVE_ARCHIVE}")
        elseif(EXISTS "${tarball}")
            file(SHA256 "${tarball}" got)
            if(NOT got STREQUAL sha)
                message(STATUS "dxvk-native: ${tarball} has sha256 ${got}, "
                    "not the pin; downloading it again")
                file(REMOVE "${tarball}")
            endif()
        endif()
        if(NOT XBOXRECOMP_DXVK_NATIVE_ARCHIVE AND NOT EXISTS "${tarball}")
            string(RANDOM LENGTH 8 tag)
            set(part "${tarball}.part-${tag}")
            message(STATUS "dxvk-native: downloading ${name}.tar.gz")
            file(DOWNLOAD "${_dxvk_native_url}" "${part}"
                STATUS st TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
            list(GET st 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE "${part}")
                list(GET st 1 msg)
                message(FATAL_ERROR "dxvk-native: downloading ${name}.tar.gz failed "
                    "(${msg}); set XBOXRECOMP_DXVK_NATIVE_ARCHIVE to a local copy of "
                    "${_dxvk_native_url}")
            endif()
            _xboxrecomp_dsp56300_place_tarball("${part}" "${tarball}" "${sha}")
            set(downloaded TRUE)
        endif()
        file(SHA256 "${tarball}" got)
        if(NOT got STREQUAL sha)
            if(downloaded)
                file(REMOVE "${tarball}")
            endif()
            message(FATAL_ERROR "dxvk-native: ${tarball} has sha256 ${got}, "
                "expected ${sha} (the pin in cmake/dxvk_native.cmake)")
        endif()
        # The tarball's top level is usr/: extract into a private directory,
        # stamp it, then rename it into place whole.
        string(RANDOM LENGTH 8 tag)
        set(tmp "${cache}/.extract-${name}-${tag}")
        file(REMOVE_RECURSE "${tmp}")
        file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${tmp}")
        if(NOT EXISTS "${tmp}/usr/lib/libdxvk_d3d11.so.0"
           OR NOT EXISTS "${tmp}/usr/include/dxvk/d3d11.h")
            file(REMOVE_RECURSE "${tmp}")
            message(FATAL_ERROR "dxvk-native: ${tarball} has no usr/lib/libdxvk_d3d11.so.0 "
                "or usr/include/dxvk/d3d11.h")
        endif()
        file(WRITE "${tmp}/.xboxrecomp-sha256" "${sha}\n")
        _xboxrecomp_dsp56300_stamp("${stamp}" have)
        if(NOT have STREQUAL sha)
            if(EXISTS "${root}")
                file(REMOVE_RECURSE "${root}")
            endif()
            file(RENAME "${tmp}" "${root}" RESULT moved)
            _xboxrecomp_dsp56300_stamp("${stamp}" have)
            if(NOT have STREQUAL sha)
                file(REMOVE_RECURSE "${tmp}")
                message(FATAL_ERROR "dxvk-native: could not move the extracted "
                    "${name} to ${root} (${moved})")
            endif()
        endif()
        file(REMOVE_RECURSE "${tmp}")
    endif()

    foreach(lib d3d11 dxgi)
        add_library(dxvk::${lib} SHARED IMPORTED GLOBAL)
        set_target_properties(dxvk::${lib} PROPERTIES
            IMPORTED_LOCATION "${root}/usr/lib/libdxvk_${lib}.so.0"
            IMPORTED_SONAME "libdxvk_${lib}.so.0"
            INTERFACE_INCLUDE_DIRECTORIES "${root}/usr/include/dxvk")
    endforeach()
    message(STATUS "dxvk-native: ${name} (${root})")
    set(${out_root} "${root}" PARENT_SCOPE)
endfunction()
