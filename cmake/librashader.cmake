# present.shader (src/kernel/nv2a_present_chain.c): the bundled slang
# presets and the librashader library, staged beside a game's executable.
#
#   xboxrecomp_present_shaders(<exe-target>)
#
# A game calls it after add_executable to offer present.shader. After each
# build of <exe-target> its directory holds shaders/ (assets/shaders, the
# PD/MIT presets) and, with XBOXRECOMP_LIBRASHADER on, librashader.dll or
# librashader.dylib. The runtime loads the library at run time and never
# links it (the D3D11 runtime does not build with MinGW), so the game builds
# and runs without it: present.shader then says the library is missing and
# the present stays stock.
#
# The library is a prebuilt release, never built here (no Rust toolchain on
# the build machines), pinned by sha256 like cmake/dsp56300.cmake. macOS and
# arm64 Windows take the upstream release. x86-64 Windows takes our build of
# the same source (third_party/librashader/build-windows.sh), hosted on the
# toolkit fork's releases: the upstream dll imports D3DX9_43.dll and the
# VC++ runtime, which a stock Windows 11 lacks (LoadLibrary error 126).
# It is MPL-2.0: LICENSES/MPL-2.0-librashader.txt is staged as
# shaders/LICENSE-librashader.txt, and shaders/README.md has the source link.
# Our zip also holds THIRDPARTY.txt, the licences of the crates and C/C++
# libraries linked into the dll, staged as shaders/THIRDPARTY-librashader.txt.
# Upstream's macOS zips carry no such file, so the same notices for its
# dylib are generated once (third_party/librashader/notices-macos.sh),
# committed as third_party/librashader/THIRDPARTY-macos.txt and staged under
# the same name.
#
# Cache variables:
#   XBOXRECOMP_LIBRASHADER          ON (default): download and stage the library
#   XBOXRECOMP_LIBRASHADER_ARCHIVE  a local copy of the release zip for this
#                                   target (offline builds); still sha-checked
#   XBOXRECOMP_LIBRASHADER_CACHE    download and extraction directory
#                                   (default <build>/_deps/librashader)

# INTERNAL cache entries, not plain variables: the toolkit includes this
# file from its own directory, and the game calls the function from its own.
set(XBOXRECOMP_LIBRASHADER_VERSION 0.12.0 CACHE INTERNAL "")
set(_lrs_url_base
    "https://github.com/SnowflakePowered/librashader/releases/download/librashader-v${XBOXRECOMP_LIBRASHADER_VERSION}"
    CACHE INTERNAL "")
# Our D3D11-only x86-64 Windows build: no D3DX9 or VC++ runtime imports.
set(_lrs_url_x86_64-windows
    "https://github.com/apexJCL/xboxrecomp/releases/download/librashader-v${XBOXRECOMP_LIBRASHADER_VERSION}-xbr1"
    CACHE INTERNAL "")
set(_lrs_zip_x86_64-windows
    "librashader-x86_64-windows-d3d11-v${XBOXRECOMP_LIBRASHADER_VERSION}-xbr1.zip"
    CACHE INTERNAL "")
set(_lrs_notices_x86_64-windows THIRDPARTY.txt CACHE INTERNAL "")
# The macOS notices, from the repository rather than the zip.
get_filename_component(_lrs_macos_notices
    "${CMAKE_CURRENT_LIST_DIR}/../third_party/librashader/THIRDPARTY-macos.txt" ABSOLUTE)
set(_lrs_repo_notices_aarch64-macos "${_lrs_macos_notices}" CACHE INTERNAL "")
set(_lrs_repo_notices_x86_64-macos "${_lrs_macos_notices}" CACHE INTERNAL "")
# sha256 of each release zip: upstream's from the release's asset digests
# (the Mac one checked against a downloaded copy), ours from
# third_party/librashader/build-windows.sh with its pinned Rust, llvm-mingw
# and cargo-about (two runs on macOS arm64 gave these bytes).
set(_lrs_sha256_aarch64-macos
    49808004a4904f6a99e0231092dcfdfe52b7b61f68430a4c9f1e165749c4c90e CACHE INTERNAL "")
set(_lrs_sha256_x86_64-macos
    8b2a50cefacf4073e8fa4757bec30242a788068c4096a580d94430688c184767 CACHE INTERNAL "")
set(_lrs_sha256_x86_64-windows
    c01a5ed3c3d9b53f1b21bfbca54ad8e012670416241477b1773ef93f16314e90 CACHE INTERNAL "")
set(_lrs_sha256_aarch64-windows
    acea3871d7c1cf345ae9da47378b399d89a1e05496160a652103ba5b7b5eb775 CACHE INTERNAL "")

option(XBOXRECOMP_LIBRASHADER
    "Stage the prebuilt librashader beside the executable for present.shader" ON)
set(XBOXRECOMP_LIBRASHADER_ARCHIVE "" CACHE FILEPATH
    "Local librashader release zip for this target (default: download it)")
set(XBOXRECOMP_LIBRASHADER_CACHE "${CMAKE_BINARY_DIR}/_deps/librashader" CACHE PATH
    "Download and extraction directory for the librashader release")

# The release's platform name for this build, or empty.
function(_xboxrecomp_librashader_platform out)
    set(plat "")
    if(CMAKE_OSX_ARCHITECTURES)
        set(proc "${CMAKE_OSX_ARCHITECTURES}")
    else()
        set(proc "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    string(TOLOWER "${proc}" proc)
    if(proc MATCHES "^(arm64|aarch64)$")
        set(arch aarch64)
    elseif(proc MATCHES "^(x86_64|amd64)$")
        set(arch x86_64)
    else()
        set(arch "")              # also a universal build: "arm64;x86_64"
    endif()
    if(arch AND CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND NOT IOS)
        set(plat ${arch}-macos)
    elseif(arch AND WIN32)
        set(plat ${arch}-windows)
    endif()
    set(${out} "${plat}" PARENT_SCOPE)
endfunction()

# The library file for this build into <out-var>, or empty with a message,
# and the zip's third-party notices into <notices-var>, or empty when the
# zip has none.
function(_xboxrecomp_librashader_fetch out notices)
    _xboxrecomp_librashader_platform(plat)
    if(NOT plat)
        set(${out} "" PARENT_SCOPE)
        set(${notices} "" PARENT_SCOPE)
        message(STATUS "librashader: no prebuilt release for this target; present.shader"
                       " will say the library is missing")
        return()
    endif()
    _xboxrecomp_librashader_fetch_plat(${plat} lib note)
    set(${out} "${lib}" PARENT_SCOPE)
    set(${notices} "${note}" PARENT_SCOPE)
endfunction()

# The same for release platform <plat> (aarch64-macos, x86_64-windows, ...),
# whatever this build targets: tests/librashader_imports reads the Windows
# library from a macOS or Linux build.
function(_xboxrecomp_librashader_fetch_plat plat out notices)
    set(${out} "" PARENT_SCOPE)
    set(${notices} "" PARENT_SCOPE)
    if(plat MATCHES "-windows$")
        set(libname librashader.dll)
    else()
        set(libname librashader.dylib)
    endif()
    set(zipname "librashader-${plat}-v${XBOXRECOMP_LIBRASHADER_VERSION}-optimized.zip")
    set(url "${_lrs_url_base}")
    if(DEFINED _lrs_zip_${plat})
        set(zipname "${_lrs_zip_${plat}}")
        set(url "${_lrs_url_${plat}}")
    endif()
    set(sha "${_lrs_sha256_${plat}}")
    set(dir "${XBOXRECOMP_LIBRASHADER_CACHE}/${plat}-${XBOXRECOMP_LIBRASHADER_VERSION}")
    set(lib "${dir}/${libname}")
    set(want "${libname}")
    set(note "")
    if(DEFINED _lrs_notices_${plat})
        list(APPEND want "${_lrs_notices_${plat}}")
        set(note "${dir}/${_lrs_notices_${plat}}")
    elseif(DEFINED _lrs_repo_notices_${plat})
        set(note "${_lrs_repo_notices_${plat}}")
    endif()
    if(EXISTS "${lib}" AND EXISTS "${dir}/.sha256" AND (NOT note OR EXISTS "${note}"))
        file(READ "${dir}/.sha256" have)
        if(have STREQUAL sha)
            set(${out} "${lib}" PARENT_SCOPE)
            set(${notices} "${note}" PARENT_SCOPE)
            return()
        endif()
    endif()
    if(XBOXRECOMP_LIBRASHADER_ARCHIVE)
        set(zip "${XBOXRECOMP_LIBRASHADER_ARCHIVE}")
    else()
        set(zip "${XBOXRECOMP_LIBRASHADER_CACHE}/${zipname}")
        if(NOT EXISTS "${zip}")
            message(STATUS "librashader: downloading ${zipname}")
            file(DOWNLOAD "${url}/${zipname}" "${zip}.part"
                 STATUS st TLS_VERIFY ON)
            list(GET st 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE "${zip}.part")
                # present.shader is opt-in: an offline or firewalled
                # configure builds without the library (the runtime says it
                # is missing), never fails. A wrong sha below stays fatal.
                message(WARNING "librashader: download of ${zipname} failed (${st});"
                        " building without it, so present.shader will say the"
                        " library is missing. Set XBOXRECOMP_LIBRASHADER_ARCHIVE"
                        " to a local copy to include it")
                return()
            endif()
            file(RENAME "${zip}.part" "${zip}")
        endif()
    endif()
    file(SHA256 "${zip}" got)
    if(NOT got STREQUAL sha)
        message(FATAL_ERROR "librashader: ${zip} has sha256 ${got}, the pin is ${sha}")
    endif()
    file(REMOVE_RECURSE "${dir}")
    # Only the library (and our zip's notices): the upstream zips also hold
    # a static archive and a PDB (121 MB and 7 MB on Windows) nothing here
    # uses.
    file(ARCHIVE_EXTRACT INPUT "${zip}" DESTINATION "${dir}" PATTERNS ${want})
    foreach(f IN LISTS want)
        if(NOT EXISTS "${dir}/${f}")
            message(FATAL_ERROR "librashader: ${zip} has no ${f}")
        endif()
    endforeach()
    # After the zip's own check, so a zip without THIRDPARTY.txt says so;
    # this one catches a missing repo notice (THIRDPARTY-macos.txt).
    if(note AND NOT EXISTS "${note}")
        message(FATAL_ERROR "librashader: no third-party notices at ${note}")
    endif()
    file(WRITE "${dir}/.sha256" "${sha}")
    set(${out} "${lib}" PARENT_SCOPE)
    set(${notices} "${note}" PARENT_SCOPE)
endfunction()

function(xboxrecomp_present_shaders target)
    if(NOT XBOXRECOMP_ENHANCE)
        return()
    endif()
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${root}/assets/shaders" "$<TARGET_FILE_DIR:${target}>/shaders"
        COMMENT "present.shader presets beside ${target}"
        VERBATIM)
    if(XBOXRECOMP_LIBRASHADER)
        _xboxrecomp_librashader_fetch(lib notices)
        if(lib)
            # The MPL-2.0 text, the third-party notices and the source link
            # travel in shaders/, so the packaging carries one directory and
            # the library.
            set(copy_notices "")
            if(notices)
                set(copy_notices COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${notices}"
                    "$<TARGET_FILE_DIR:${target}>/shaders/THIRDPARTY-librashader.txt")
            endif()
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${lib}" "$<TARGET_FILE_DIR:${target}>/"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${root}/LICENSES/MPL-2.0-librashader.txt"
                        "$<TARGET_FILE_DIR:${target}>/shaders/LICENSE-librashader.txt"
                ${copy_notices}
                VERBATIM)
        endif()
    endif()
endfunction()
