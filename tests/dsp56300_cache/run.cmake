# cmake -DFIXTURE=<dir> -DWORK=<scratch dir> -DSOURCE=<filled cache> -P run.cmake
file(GLOB tarballs "${SOURCE}/dsp56300-*.tar.gz")
if(NOT tarballs)
    message(STATUS "skip: no dsp56300 tarball in '${SOURCE}'")
    cmake_language(EXIT 77)
endif()
# The fixture picks the triple itself; the scratch cache gets every tarball
# name the source has, so it finds its own.
file(REMOVE_RECURSE ${WORK})
set(cache ${WORK}/cache)
file(MAKE_DIRECTORY ${cache})

function(configure tree)
    execute_process(COMMAND ${CMAKE_COMMAND} -S ${FIXTURE} -B ${WORK}/${tree}
            -DSOURCE=${SOURCE} -DXBOXRECOMP_DSP56300_CACHE=${cache}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err)
    if(NOT rc EQUAL 0)
        message(FATAL_ERROR "configure ${tree} failed:\n${out}\n${err}")
    endif()
    set(last_out "${out}" PARENT_SCOPE)
endfunction()

# A corrupt tarball under the right name: downloaded again, then extracted.
foreach(t IN LISTS tarballs)
    get_filename_component(n "${t}" NAME)
    file(WRITE "${cache}/${n}" "not a tarball\n")
endforeach()
# Debris: an hour-old download and extraction go, a young download stays.
file(WRITE "${cache}/x.tar.gz.part-old" "")
file(MAKE_DIRECTORY "${cache}/.extract-x-old/x")
file(WRITE "${cache}/x.tar.gz.part-young" "")
execute_process(COMMAND touch -t 202001010000 "${cache}/x.tar.gz.part-old"
    "${cache}/.extract-x-old" RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "touch failed")
endif()

configure(stale)
set(fails 0)
if(NOT last_out MATCHES "not the pin; downloading it again")
    message(SEND_ERROR "stale: the corrupt tarball was not fetched again")
endif()
file(GLOB stamps "${cache}/dsp56300-*/.xboxrecomp-sha256")
if(NOT stamps)
    message(SEND_ERROR "stale: no extracted tree with a stamp")
endif()
foreach(gone x.tar.gz.part-old .extract-x-old)
    if(EXISTS "${cache}/${gone}")
        message(SEND_ERROR "sweep: ${gone} is older than an hour and stayed")
    endif()
endforeach()
if(NOT EXISTS "${cache}/x.tar.gz.part-young")
    message(SEND_ERROR "sweep: a young .part- was removed")
endif()

# A second tree on the same cache only reads it.
configure(second)
if(last_out MATCHES "downloading")
    message(SEND_ERROR "second: downloaded although the cache was filled")
endif()
message(STATUS "dsp56300_cache: done")
