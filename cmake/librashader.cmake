# present.shader (src/kernel/nv2a_present_chain.c): the bundled slang
# presets and the librashader library, staged beside a game's executable.
#
# Every game gets it with no line of its own: when the top-level CMakeLists
# ends, each executable that links the enhancements layer (xbox_enhance,
# which holds the menu's Shaders page) is staged. Scanning at the end is what
# makes it automatic, since the toolkit is added before the game's targets
# exist; a game added later cannot forget it, as four did with an opt-in call.
#
#   xboxrecomp_present_shaders(<exe-target>)
#
# stages an executable that does not link the layer. On one that does it
# changes nothing, so a game's older call stays harmless.
#
# Before each build of a staged target its directory gets shaders/
# (assets/shaders, the PD/MIT presets) and, with XBOXRECOMP_LIBRASHADER on,
# librashader.dll or librashader.dylib. The runtime loads the library at run
# time and never links it (the D3D11 runtime does not build with MinGW), so
# the game builds and runs without it: present.shader then says the library
# is missing and the present stays stock.
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
    if(XBOXRECOMP_ENHANCE)
        set_property(TARGET ${target} PROPERTY XBOXRECOMP_PRESENT_SHADERS ON)
    endif()
endfunction()

# Every target under <dir>, its subdirectories included, into <out>.
function(_xboxrecomp_targets_under dir out)
    get_property(all DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    get_property(subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
    foreach(sub IN LISTS subs)
        _xboxrecomp_targets_under("${sub}" under)
        list(APPEND all ${under})
    endforeach()
    set(${out} "${all}" PARENT_SCOPE)
endfunction()

# The copies run in a custom target the executable depends on, not as its
# POST_BUILD: add_custom_command(TARGET) is refused for a target of another
# directory (CMP0040), and this runs in the top-level one while Burnout 3's
# executable lives in src/game. $<TARGET_FILE_DIR> adds no dependency back on
# the executable (CMP0112), and copy_directory makes the directory, so this
# can run before the link. Being a dependency it also runs for a build of
# just the executable, and puts back a shaders/ someone deleted.
function(_xboxrecomp_present_shaders_stage target)
    get_filename_component(root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set(dir "$<TARGET_FILE_DIR:${target}>")
    set(copies COMMAND ${CMAKE_COMMAND} -E copy_directory "${root}/assets/shaders" "${dir}/shaders")
    if(XBOXRECOMP_LIBRASHADER)
        _xboxrecomp_librashader_fetch(lib notices)
        if(lib)
            # The MPL-2.0 text, the third-party notices and the source link
            # travel in shaders/, so the packaging carries one directory and
            # the library.
            list(APPEND copies
                COMMAND ${CMAKE_COMMAND} -E copy_if_different "${lib}" "${dir}/"
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${root}/LICENSES/MPL-2.0-librashader.txt"
                        "${dir}/shaders/LICENSE-librashader.txt")
            if(notices)
                list(APPEND copies COMMAND ${CMAKE_COMMAND} -E copy_if_different
                     "${notices}" "${dir}/shaders/THIRDPARTY-librashader.txt")
            endif()
        endif()
    endif()
    add_custom_target(${target}_present_shaders ${copies}
        COMMENT "present.shader presets beside ${target}"
        VERBATIM)
    add_dependencies(${target} ${target}_present_shaders)
endfunction()

# <out> is 1 when <target> reaches xbox_enhance through its link closure.
# A game links the xboxrecomp umbrella, not the layer itself (cat, KUF), so
# its own LINK_LIBRARIES never name xbox_enhance: the INTERFACE ones of what
# it links are walked too. $<LINK_ONLY:> is what a static library's PRIVATE
# link becomes; any other generator expression and plain flags or files are
# not targets and are skipped, as are the ::@ markers of a link made from
# another directory.
function(_xboxrecomp_links_enhance target out)
    set(todo ${target})
    set(seen "")
    set(found 0)
    while(todo)
        list(POP_FRONT todo t)
        if(NOT TARGET "${t}" OR t IN_LIST seen)
            continue()
        endif()
        list(APPEND seen "${t}")
        get_target_property(real "${t}" ALIASED_TARGET)
        if(real)
            set(t "${real}")
        endif()
        if(t STREQUAL "xbox_enhance")
            set(found 1)
            break()
        endif()
        get_target_property(type "${t}" TYPE)
        set(libs "")
        if(NOT type STREQUAL "INTERFACE_LIBRARY")
            get_target_property(l "${t}" LINK_LIBRARIES)
            if(l)
                list(APPEND libs ${l})
            endif()
        endif()
        get_target_property(l "${t}" INTERFACE_LINK_LIBRARIES)
        if(l)
            list(APPEND libs ${l})
        endif()
        foreach(l IN LISTS libs)
            string(REGEX REPLACE "^\\$<LINK_ONLY:(.*)>$" "\\1" l "${l}")
            if(NOT l MATCHES "^(\\$<|::@)")
                list(APPEND todo "${l}")
            endif()
        endforeach()
    endwhile()
    set(${out} ${found} PARENT_SCOPE)
endfunction()

function(_xboxrecomp_present_shaders_all)
    if(NOT XBOXRECOMP_ENHANCE)
        return()
    endif()
    get_property(own GLOBAL PROPERTY _XBOXRECOMP_PRESENT_SHADERS_OWN)
    _xboxrecomp_targets_under("${CMAKE_SOURCE_DIR}" targets)
    foreach(t IN LISTS targets)
        get_target_property(type ${t} TYPE)
        if(NOT type STREQUAL "EXECUTABLE")
            continue()
        endif()
        get_target_property(asked ${t} XBOXRECOMP_PRESENT_SHADERS)
        # The toolkit's own executables (its in-tree test harnesses) link the
        # umbrella too, and never show a menu: only a name asks for them.
        set(mine FALSE)
        if(own)
            get_target_property(src ${t} SOURCE_DIR)
            cmake_path(IS_PREFIX own "${src}" NORMALIZE mine)
        endif()
        if(mine AND NOT asked)
            continue()
        endif()
        _xboxrecomp_links_enhance(${t} links)
        if(asked OR links)
            _xboxrecomp_present_shaders_stage(${t})
        endif()
    endforeach()
endfunction()

# The directory that included this, the toolkit's top: the scan runs in the
# game's top directory, which does not see this one's variables.
set_property(GLOBAL PROPERTY _XBOXRECOMP_PRESENT_SHADERS_OWN "${CMAKE_CURRENT_SOURCE_DIR}")
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _xboxrecomp_present_shaders_all)
