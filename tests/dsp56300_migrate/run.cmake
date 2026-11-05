# Drives fixture/ through the cases of xboxrecomp_dsp56300_migrate.
# cmake -DFIXTURE=<dir> -DWORK=<scratch dir> -P run.cmake
set(fails 0)

# Configure tree <tree> with the given -D arguments.
function(configure tree)
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${FIXTURE} -B ${WORK}/${tree} ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "configure ${tree} ${ARGN} failed:\n${out}\n${err}")
    endif()
    set(last_out "${out}" PARENT_SCOPE)
endfunction()

function(expect tree what want)
    file(READ ${WORK}/${tree}/result.txt got)
    if(got)
        set(got ON)
    else()
        set(got OFF)
    endif()
    if(NOT got STREQUAL want)
        message(SEND_ERROR "${tree}: ${what}: XBOXRECOMP_APU_DSP is ${got}, expected ${want}")
    else()
        message(STATUS "ok   ${tree}: ${what}: ${got}")
    endif()
endfunction()

file(REMOVE_RECURSE ${WORK})

# A tree from before: the old default OFF turns ON once, with a line.
configure(old -DSTAGE=old)
expect(old "the old toolkit" OFF)
configure(old -DSTAGE=new)
expect(old "first configure on the new toolkit" ON)
if(NOT last_out MATCHES "was the old default OFF")
    message(SEND_ERROR "old: no status line for the migration")
endif()
# Then an OFF chosen afterwards wins, and stays.
configure(old -DSTAGE=new -DXBOXRECOMP_APU_DSP=OFF)
expect(old "OFF given after the migration" OFF)
configure(old -DSTAGE=new)
expect(old "and on the next configure" OFF)

# An OFF given in the migrating configure itself is kept.
configure(cli -DSTAGE=old)
configure(cli -DSTAGE=new -DXBOXRECOMP_APU_DSP=OFF)
expect(cli "OFF on the command line of the first new configure" OFF)
configure(cli -DSTAGE=new)
expect(cli "and on the next configure" OFF)

# An OFF given with -D under the old toolkit turns ON too: the old option()
# replaced the -D entry's help string with its own, so the cache cannot tell
# it from the old default. Expected; only an OFF given in the migrating
# configure or later is kept.
configure(oldcli -DSTAGE=old -DXBOXRECOMP_APU_DSP=OFF)
expect(oldcli "OFF given with -D under the old toolkit" OFF)
configure(oldcli -DSTAGE=new)
expect(oldcli "first configure on the new toolkit" ON)

# A tree whose game already handled it (BLiNX 2 sets the marker) keeps OFF.
configure(marked -DSTAGE=old)
configure(marked -DSTAGE=new -DXBOXRECOMP_APU_DSP_MIGRATED=ON)
expect(marked "marker set by the game" OFF)

# A target without an archive keeps OFF, quietly.
configure(noarch -DSTAGE=old)
configure(noarch -DSTAGE=new -DHAS_ARCHIVE=OFF)
expect(noarch "target without an archive" OFF)
if(last_out MATCHES "was the old default OFF")
    message(SEND_ERROR "noarch: migrated without an archive")
endif()

# A fresh tree takes the default, and an explicit OFF holds.
configure(fresh -DSTAGE=new)
expect(fresh "fresh tree" ON)
configure(fresh_off -DSTAGE=new -DXBOXRECOMP_APU_DSP=OFF)
configure(fresh_off -DSTAGE=new)
expect(fresh_off "fresh tree, OFF on the command line" OFF)
