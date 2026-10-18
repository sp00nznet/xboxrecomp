# Reproducible builds: the same sources and options give the same bytes,
# whatever folder the game, the toolkit and the build tree sit in and
# whenever the link runs. A packager compares a rebuilt program against the
# one a player has by its sha256 (an update ships only the files that
# changed), which only works when nothing but the inputs reach the output.
#
# What would leak otherwise:
#   - absolute source paths, through __FILE__ (asserts, log lines) and the
#     debug info: -ffile-prefix-map rewrites the game root to ".", the
#     toolkit to "tk", the build tree to "build" and the llvm-mingw root to
#     "llvm-mingw" (clang applies the most specific map, so a toolkit or a
#     build tree inside the game folder still gets its own name);
#   - Windows (lld through the mingw driver): the PE timestamp, and with it
#     the PDB GUID, which lld derives from the content only under
#     --no-insert-timestamp; the PDB's absolute path in the image (pdbaltpath
#     keeps only its name, which dbghelp finds beside the exe); and the link
#     directory lld records in the PDB (pdbsourcepath pins it). The PDB also
#     records the link command line, so a game passes --pdb= (lld names it
#     after the output) rather than an absolute path;
#   - macOS: archive member dates (ZERO_AR_DATE); ld64's LC_UUID is already a
#     hash of the content, and -reproducible is added where ld64 has it.
#
# XBOXRECOMP_PREFIX_MAP adds maps (OLD=NEW, a list) for other roots that can
# reach the compile lines, for example a toolchain kept outside the game.

include(CheckCCompilerFlag)
include(CheckLinkerFlag)

option(XBOXRECOMP_REPRODUCIBLE "Path-independent, timestamp-free build output" ON)
set(XBOXRECOMP_PREFIX_MAP "" CACHE STRING
    "Extra -ffile-prefix-map entries (OLD=NEW;...) for a reproducible build")

# xboxrecomp_reproducible(<interface target> <toolkit root>)
# Sets the flags on the toolkit's own libraries (directory scope, so it is
# called before their add_subdirectory) and, through the interface target,
# on the game that links it.
function(xboxrecomp_reproducible iface tk_root)
    if(NOT XBOXRECOMP_REPRODUCIBLE)
        return()
    endif()
    if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        return()
    endif()

    set(maps
        "${CMAKE_SOURCE_DIR}=."
        "${tk_root}=tk"
        "${CMAKE_BINARY_DIR}=build")
    if(MINGW)
        get_filename_component(_cc_bin "${CMAKE_C_COMPILER}" DIRECTORY)
        get_filename_component(_tc_root "${_cc_bin}/.." ABSOLUTE)
        # clang finds its headers from its resolved path, so a toolchain
        # reached through a link is mapped by both names.
        get_filename_component(_tc_real "${_tc_root}" REALPATH)
        list(APPEND maps "${_tc_root}=llvm-mingw")
        if(NOT _tc_real STREQUAL _tc_root)
            list(APPEND maps "${_tc_real}=llvm-mingw")
        endif()
    endif()
    list(APPEND maps ${XBOXRECOMP_PREFIX_MAP})

    check_c_compiler_flag("-ffile-prefix-map=/a=b" _xr_has_prefix_map)
    if(_xr_has_prefix_map)
        set(opts "")
        foreach(m IN LISTS maps)
            # The resource compiler takes no such flag.
            list(APPEND opts "$<$<COMPILE_LANGUAGE:C,CXX,OBJC,OBJCXX>:-ffile-prefix-map=${m}>")
        endforeach()
        add_compile_options(${opts})
        target_compile_options(${iface} INTERFACE ${opts})
    endif()

    if(MINGW)
        target_link_options(${iface} INTERFACE -Wl,--no-insert-timestamp)
        if(CMAKE_C_COMPILER_ID MATCHES "Clang")
            # The consumer's own output name (not lld's %_PDB%, which a
            # cmd.exe wrapper on a Windows host could try to expand).
            set(_pdb "$<TARGET_FILE_BASE_NAME:$<TARGET_PROPERTY:NAME>>.pdb")
            target_link_options(${iface} INTERFACE
                "-Wl,--Xlink=-pdbaltpath:${_pdb}"
                "-Wl,--Xlink=-pdbsourcepath:/build")
        endif()
    elseif(APPLE)
        # ar and ranlib write member dates unless told not to; the rules
        # CMake chose are kept, run under the variable.
        foreach(lang C OBJC)
            foreach(step CREATE APPEND FINISH)
                set(rule "${CMAKE_${lang}_ARCHIVE_${step}}")
                if(rule AND NOT rule MATCHES "ZERO_AR_DATE")
                    set(CMAKE_${lang}_ARCHIVE_${step}
                        "\"${CMAKE_COMMAND}\" -E env ZERO_AR_DATE=1 ${rule}" PARENT_SCOPE)
                endif()
            endforeach()
        endforeach()
        check_linker_flag(C "-Wl,-reproducible" _xr_has_ld_repro)
        if(_xr_has_ld_repro)
            target_link_options(${iface} INTERFACE -Wl,-reproducible)
        endif()
    endif()
endfunction()
