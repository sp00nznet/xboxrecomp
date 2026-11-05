# dsp56300: the MIT DSP56300 JIT (https://github.com/xemu-project/dsp56300,
# formerly mborgerson/dsp56300) that XBOXRECOMP_APU_DSP links for the MCPX
# APU's GP and EP. It is Rust with a C FFI; the build takes the release's
# prebuilt static library per target instead of building Rust, so the game's
# build machines and the wizard need no Rust toolchain. Every archive is
# pinned by sha256, as wizard/pins.json pins the wizard's own downloads.
#
#   xboxrecomp_dsp56300(<target-var> <reason-var>)
#   xboxrecomp_dsp56300_migrate(<has-archive>)
#
# On success <target-var> is the imported target dsp56300::emu (the library,
# include/dsp56300.h and the system libraries the bundled Rust std needs) and
# <reason-var> is empty. When this target has no archive (iPadOS, MSVC, a
# universal macOS build), <target-var> is empty and <reason-var> says why;
# the caller decides whether that is an error. A failed download and an
# archive whose sha256 does not match are always configure errors: the
# option was asked for on a target that has it, so building without it
# would hide the failure.
#
# xboxrecomp_dsp56300_migrate runs before option(XBOXRECOMP_APU_DSP): it turns
# a cached OFF from before the JIT default ON, once per build tree, when
# <has-archive> is true. An OFF is kept only when it is given in the
# migrating configure or later (see the function).
#
# Cache variables:
#   XBOXRECOMP_DSP56300_ARCHIVE  a local copy of the release tarball for this
#                                target (offline builds); still sha-checked
#   XBOXRECOMP_DSP56300_CACHE    where downloads and the extracted tree go
#                                (default <build>/_deps/dsp56300, so a
#                                configure writes nothing outside its tree;
#                                several trees, and parallel configures, may
#                                share one directory: the extraction is
#                                renamed into place whole, and a tree whose
#                                stamp matches is only read)

set(XBOXRECOMP_DSP56300_VERSION 1.0.0)
set(_dsp56300_url_base
    "https://github.com/xemu-project/dsp56300/releases/download/v${XBOXRECOMP_DSP56300_VERSION}")
# sha256 of each release archive, from the release's asset digests
# (checked against downloaded copies of each archive).
set(_dsp56300_sha256_aarch64-apple-darwin
    a97c73ad57a8f53b7412c671ae42cad2626bfbaab3c4492ccc1d9e91d8d001e3)
set(_dsp56300_sha256_x86_64-apple-darwin
    db41ee8a7229c100aa7165eb434ceae76fb3bbf994916ab0de8ea05fdb967a48)
set(_dsp56300_sha256_x86_64-pc-windows-gnu
    6ec091d3e4b6e6e6276b474c852736449d7f538a1f8826a89e3cb6e787bc5ac6)
set(_dsp56300_sha256_aarch64-pc-windows-gnullvm
    4c000ab2cb76f376b7397281c4d75973683131b2fcb0ab63149daa8d647365dd)
set(_dsp56300_sha256_x86_64-unknown-linux-gnu
    e16ef45da9572815d43ab89d030029b898e71226a265468a22591e0171df4b60)
set(_dsp56300_sha256_aarch64-unknown-linux-gnu
    c58db50c3d39d8c4a5f28d01cfeed9849082422420c55e0c0d85bc1eabb5bd42)

# The option's help string. The one before the JIT default, when the option was
# opt-in and defaulted OFF, is how the migration tells an OFF the old
# toolkit's option() left in the cache from an OFF given with -D in this
# configure, which still carries CMake's own help string.
set(XBOXRECOMP_APU_DSP_HELP
    "Build the MCPX APU DSP emulation (dsp56300 JIT; ON where an archive exists)")
set(_dsp56300_old_help "Build the opt-in MCPX APU DSP emulation (dsp56300 JIT)")

set(XBOXRECOMP_DSP56300_ARCHIVE "" CACHE FILEPATH
    "Local dsp56300 release tarball for this target (default: download it)")
set(XBOXRECOMP_DSP56300_CACHE "${CMAKE_BINARY_DIR}/_deps/dsp56300" CACHE PATH
    "Download and extraction directory for the dsp56300 release")

# The Rust target triple of the archive this build links, or empty.
function(_xboxrecomp_dsp56300_triple out)
    set(triple "")
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
    if(arch AND CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(triple ${arch}-apple-darwin)
    elseif(arch AND WIN32 AND NOT MSVC)
        # The archives are built for the GNU ABI: x86-64 against mingw-w64,
        # arm64 against llvm-mingw (gnullvm). MSVC links neither.
        if(arch STREQUAL "x86_64")
            set(triple x86_64-pc-windows-gnu)
        else()
            set(triple aarch64-pc-windows-gnullvm)
        endif()
    elseif(arch AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(triple ${arch}-unknown-linux-gnu)
    endif()
    set(${out} "${triple}" PARENT_SCOPE)
endfunction()

# The sha256 a tree's stamp names, or empty.
function(_xboxrecomp_dsp56300_stamp stamp out)
    set(have "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" have)
        string(STRIP "${have}" have)
    endif()
    set(${out} "${have}" PARENT_SCOPE)
endfunction()

# A tree configured before the JIT default caches the old option()'s OFF, and a
# changed default never replaces a cached value: every dev, bench and build
# host tree would stay stub-only (packaging passes the value explicitly).
# Once per tree, that OFF becomes ON, with a line. The old option() also
# rewrote the help string of an OFF given with -D under the old toolkit, so
# that OFF looks like the default and turns ON too: the old cache cannot tell
# them apart. An OFF is kept only when given in the migrating configure
# itself (it still has CMake's help string) or later: the marker is cached
# on every configure. BLiNX 2 sets the marker itself for a tree its own
# CAT_APU_DSP_MIGRATED step already handled.
function(xboxrecomp_dsp56300_migrate has_archive)
    if(NOT XBOXRECOMP_APU_DSP_MIGRATED AND has_archive
       AND DEFINED CACHE{XBOXRECOMP_APU_DSP} AND NOT "$CACHE{XBOXRECOMP_APU_DSP}")
        get_property(help CACHE XBOXRECOMP_APU_DSP PROPERTY HELPSTRING)
        if(help STREQUAL _dsp56300_old_help)
            set(XBOXRECOMP_APU_DSP ON CACHE BOOL "${XBOXRECOMP_APU_DSP_HELP}" FORCE)
            message(STATUS "dsp56300: XBOXRECOMP_APU_DSP was the old default OFF; "
                "now ON, the default (-DXBOXRECOMP_APU_DSP=OFF for the stub only)")
        endif()
    endif()
    set(XBOXRECOMP_APU_DSP_MIGRATED ON CACHE INTERNAL
        "XBOXRECOMP_APU_DSP's cached former default OFF was turned ON once")
endfunction()

# Downloads and extractions cut short (a killed configure) leave
# .part-*/.extract-* entries in a cache that may be shared and is never
# cleaned otherwise. One older than an hour belongs to no live configure.
function(_xboxrecomp_dsp56300_sweep)
    file(GLOB debris LIST_DIRECTORIES true
        "${XBOXRECOMP_DSP56300_CACHE}/*.part-*"
        "${XBOXRECOMP_DSP56300_CACHE}/.extract-*")
    string(TIMESTAMP now "%s" UTC)
    foreach(entry IN LISTS debris)
        file(TIMESTAMP "${entry}" mtime "%s" UTC)
        if(mtime AND now GREATER_EQUAL mtime)
            math(EXPR age "${now} - ${mtime}")
            if(age GREATER 3600)
                file(REMOVE_RECURSE "${entry}")
            endif()
        endif()
    endforeach()
endfunction()

# Moves a finished private download onto the cached tarball. Two trees
# downloading at once (a first build-all on a shared cache) race here; on
# Windows the loser's rename fails while the winner's file is open, so a
# tarball already in place with the pinned sha is as good as our own.
function(_xboxrecomp_dsp56300_place_tarball part tarball sha)
    if(CMAKE_VERSION VERSION_LESS 3.21)
        file(RENAME "${part}" "${tarball}")
        return()
    endif()
    file(RENAME "${part}" "${tarball}" RESULT moved)
    if(NOT moved EQUAL 0)
        set(got "")
        if(EXISTS "${tarball}")
            file(SHA256 "${tarball}" got)
        endif()
        file(REMOVE "${part}")
        if(NOT got STREQUAL sha)
            message(FATAL_ERROR "dsp56300: could not move the download to "
                "${tarball} (${moved})")
        endif()
    endif()
endfunction()

function(xboxrecomp_dsp56300 out_target out_reason)
    set(${out_target} "" PARENT_SCOPE)
    if(TARGET dsp56300::emu)
        set(${out_target} dsp56300::emu PARENT_SCOPE)
        set(${out_reason} "" PARENT_SCOPE)
        return()
    endif()

    _xboxrecomp_dsp56300_triple(triple)
    if(NOT triple)
        string(CONCAT why "no dsp56300 ${XBOXRECOMP_DSP56300_VERSION} archive for "
            "${CMAKE_SYSTEM_NAME} ${CMAKE_SYSTEM_PROCESSOR} ${CMAKE_OSX_ARCHITECTURES}")
        set(${out_reason} "${why}" PARENT_SCOPE)
        return()
    endif()
    set(sha ${_dsp56300_sha256_${triple}})
    set(name dsp56300-${XBOXRECOMP_DSP56300_VERSION}-${triple})
    set(tarball "${XBOXRECOMP_DSP56300_CACHE}/${name}.tar.gz")
    set(root "${XBOXRECOMP_DSP56300_CACHE}/${name}")
    set(stamp "${root}/.xboxrecomp-sha256")
    _xboxrecomp_dsp56300_sweep()

    # Extracted before, from an archive with this sha: nothing to do.
    _xboxrecomp_dsp56300_stamp("${stamp}" have)
    if(NOT have STREQUAL sha)
        set(downloaded FALSE)
        if(XBOXRECOMP_DSP56300_ARCHIVE)
            set(tarball "${XBOXRECOMP_DSP56300_ARCHIVE}")
        elseif(EXISTS "${tarball}")
            # A cached tarball that does not match the pin (corrupt, or left
            # by a toolkit that renamed without checking) would fail every
            # tree that shares the cache: fetch it again instead.
            file(SHA256 "${tarball}" got)
            if(NOT got STREQUAL sha)
                message(STATUS "dsp56300: ${tarball} has sha256 ${got}, "
                    "not the pin; downloading it again")
                file(REMOVE "${tarball}")
            endif()
        endif()
        if(NOT XBOXRECOMP_DSP56300_ARCHIVE AND NOT EXISTS "${tarball}")
            # A private name per configure: trees sharing the cache may
            # download at once, and the rename below is whole either way.
            string(RANDOM LENGTH 8 tag)
            set(part "${tarball}.part-${tag}")
            message(STATUS "dsp56300: downloading ${name}.tar.gz")
            file(DOWNLOAD "${_dsp56300_url_base}/${name}.tar.gz" "${part}"
                STATUS st TLS_VERIFY ON INACTIVITY_TIMEOUT 60)
            list(GET st 0 code)
            if(NOT code EQUAL 0)
                file(REMOVE "${part}")
                list(GET st 1 msg)
                message(FATAL_ERROR "dsp56300: downloading ${name}.tar.gz failed "
                    "(${msg}); set XBOXRECOMP_DSP56300_ARCHIVE to a local copy of "
                    "${_dsp56300_url_base}/${name}.tar.gz, or build with "
                    "XBOXRECOMP_APU_DSP=OFF")
            endif()
            _xboxrecomp_dsp56300_place_tarball("${part}" "${tarball}" "${sha}")
            set(downloaded TRUE)
        endif()
        file(SHA256 "${tarball}" got)
        if(NOT got STREQUAL sha)
            # Our own download goes, so the next configure fetches it again;
            # a tarball the user named is theirs to keep.
            if(downloaded)
                file(REMOVE "${tarball}")
            endif()
            message(FATAL_ERROR "dsp56300: ${tarball} has sha256 ${got}, "
                "expected ${sha} (the pin in cmake/dsp56300.cmake)")
        endif()
        # Extract beside the final tree, stamp it, then rename it into place
        # whole, so a configure sharing this cache never links a half
        # extraction. Losing the rename to another configure is fine once
        # the winner's stamp matches.
        string(RANDOM LENGTH 8 tag)
        set(tmp "${XBOXRECOMP_DSP56300_CACHE}/.extract-${name}-${tag}")
        file(REMOVE_RECURSE "${tmp}")
        file(ARCHIVE_EXTRACT INPUT "${tarball}" DESTINATION "${tmp}")
        if(NOT EXISTS "${tmp}/${name}/lib/libdsp56300_emu_ffi.a")
            file(REMOVE_RECURSE "${tmp}")
            message(FATAL_ERROR "dsp56300: ${tarball} has no lib/libdsp56300_emu_ffi.a")
        endif()
        file(WRITE "${tmp}/${name}/.xboxrecomp-sha256" "${sha}\n")
        _xboxrecomp_dsp56300_stamp("${stamp}" have)
        if(NOT have STREQUAL sha)
            if(EXISTS "${root}")
                # A stale tree (another sha, or no stamp): replaced.
                file(REMOVE_RECURSE "${root}")
            endif()
            # No NO_REPLACE: CMake does it by hard link on macOS, which a
            # directory refuses (EPERM). A plain rename never replaces a
            # non-empty directory either (ENOTEMPTY, or Windows' refusal),
            # so a lost race is the error, and the stamp then decides.
            if(CMAKE_VERSION VERSION_LESS 3.21)
                file(RENAME "${tmp}/${name}" "${root}")
            else()
                file(RENAME "${tmp}/${name}" "${root}" RESULT moved)
                _xboxrecomp_dsp56300_stamp("${stamp}" have)
                if(NOT have STREQUAL sha)
                    file(REMOVE_RECURSE "${tmp}")
                    message(FATAL_ERROR "dsp56300: could not move the extracted "
                        "${name} to ${root} (${moved})")
                endif()
            endif()
        endif()
        file(REMOVE_RECURSE "${tmp}")
    endif()

    add_library(dsp56300::emu STATIC IMPORTED GLOBAL)
    set_target_properties(dsp56300::emu PROPERTIES
        IMPORTED_LOCATION "${root}/lib/libdsp56300_emu_ffi.a"
        INTERFACE_INCLUDE_DIRECTORIES "${root}/include")
    # What the bundled Rust std needs from the system (rustc's
    # native-static-libs for each target).
    if(APPLE)
        set_property(TARGET dsp56300::emu PROPERTY INTERFACE_LINK_LIBRARIES
            "-framework CoreFoundation" "-framework Security")
    elseif(WIN32)
        # The unwinder: llvm-mingw's libunwind, linked statically, or it
        # imports libunwind.dll, which a player's machine does not have;
        # GCC MinGW's libgcc_eh (untested).
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            set(unwind -l:libunwind.a)
        else()
            set(unwind gcc_eh)
        endif()
        set_property(TARGET dsp56300::emu PROPERTY INTERFACE_LINK_LIBRARIES
            ${unwind} ws2_32 userenv ntdll bcrypt advapi32 kernel32 synchronization)
    else()
        set_property(TARGET dsp56300::emu PROPERTY INTERFACE_LINK_LIBRARIES
            gcc_s util rt pthread m dl)
    endif()
    message(STATUS "dsp56300: ${name} (${root})")
    set(${out_target} dsp56300::emu PARENT_SCOPE)
    set(${out_reason} "" PARENT_SCOPE)
endfunction()
