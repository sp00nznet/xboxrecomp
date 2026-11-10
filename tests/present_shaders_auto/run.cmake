# One configure and build per case. XBOXRECOMP_LIBRASHADER is off so no case
# downloads anything; the presets are what tells a staged directory.
function(build name)
    set(b "${WORK}/${name}")
    file(REMOVE_RECURSE "${b}")
    execute_process(
        COMMAND ${CMAKE_COMMAND} -S "${FIXTURE}" -B "${b}" -G "${GEN}"
                -DCMAKE_C_COMPILER=${CC} -DTOOLKIT=${TOOLKIT}
                -DXBOXRECOMP_LIBRASHADER=OFF ${ARGN}
        RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(rc)
        message(FATAL_ERROR "${name}: configure failed:\n${out}")
    endif()
    execute_process(COMMAND ${CMAKE_COMMAND} --build "${b}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(rc)
        message(FATAL_ERROR "${name}: build failed:\n${out}")
    endif()
endfunction()

function(expect name exe staged)
    set(f "${WORK}/${name}/${exe}/shaders/crt-lottes.slangp")
    if(staged AND NOT EXISTS "${f}")
        message(FATAL_ERROR "${name}: ${exe} has no presets (${f})")
    elseif(NOT staged AND EXISTS "${f}")
        message(FATAL_ERROR "${name}: ${exe} should have no presets (${f})")
    endif()
endfunction()

# A game with no line, its executable in a subdirectory as Burnout 3's is. Its
# test executable does not link the layer.
build(auto -DXBOXRECOMP_ENHANCE=ON)
expect(auto game 1)
expect(auto game/tool 0)

# A game that links only the umbrella, which carries the layer.
build(umbrella -DXBOXRECOMP_ENHANCE=ON -DUMBRELLA=ON)
expect(umbrella game 1)
expect(umbrella game/tool 0)
expect(umbrella toolkit 0)

# The toolkit's own harness is staged only when named.
build(harness -DXBOXRECOMP_ENHANCE=ON -DCALL_HARNESS=ON)
expect(harness toolkit 1)

# A game's older call on the same target: one stage, no duplicate target.
build(called -DXBOXRECOMP_ENHANCE=ON -DCALL_GAME=ON)
expect(called game 1)

# An executable without the layer can still ask for it.
build(asked -DXBOXRECOMP_ENHANCE=ON -DCALL_TOOL=ON)
expect(asked game/tool 1)

# A layer-free build has no menu page to serve, and calling changes nothing.
build(off -DXBOXRECOMP_ENHANCE=OFF -DCALL_GAME=ON)
expect(off game 0)
build(off-umbrella -DXBOXRECOMP_ENHANCE=OFF -DUMBRELLA=ON)
expect(off-umbrella game 0)

# Restaging: the presets were staged only when the executable
# relinked, so an edited shader or notice shipped stale until a code change.
# The toolkit is copied so its presets can be edited; the build after an
# edit must restage that file without a link, and a build with no edit must
# rewrite nothing.
function(rebuild name what)
    execute_process(COMMAND ${CMAKE_COMMAND} --build "${WORK}/${name}"
                    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
    if(rc)
        message(FATAL_ERROR "${name}: ${what}: build failed:\n${out}")
    endif()
    if(out MATCHES "Linking")
        message(FATAL_ERROR "${name}: ${what}: relinked:\n${out}")
    endif()
endfunction()

set(tk "${WORK}/restage-toolkit")
file(REMOVE_RECURSE "${tk}")
file(MAKE_DIRECTORY "${tk}/assets")
file(COPY "${TOOLKIT}/cmake" DESTINATION "${tk}")
file(COPY "${TOOLKIT}/assets/shaders" DESTINATION "${tk}/assets")
set(TOOLKIT "${tk}")
build(restage -DXBOXRECOMP_ENHANCE=ON)
expect(restage game 1)
set(staged "${WORK}/restage/game/shaders")
set(lottes "${staged}/crt/shaders/crt-lottes.slang")
set(png "${staged}/anti-aliasing/shaders/smaa/SearchTex.png")
file(TIMESTAMP "${staged}/README.md" before "%s")
file(TIMESTAMP "${png}" png_before "%s")

# Timestamps have a resolution of one second here.
execute_process(COMMAND ${CMAKE_COMMAND} -E sleep 1.1)
file(APPEND "${tk}/assets/shaders/crt/shaders/crt-lottes.slang" "// edited\n")
rebuild(restage "after an edit")
file(READ "${lottes}" text)
if(NOT text MATCHES "// edited\n$")
    message(FATAL_ERROR "restage: the edited preset was not restaged (${lottes})")
endif()
file(TIMESTAMP "${staged}/README.md" after "%s")
file(TIMESTAMP "${png}" png_after "%s")
if(NOT before STREQUAL after OR NOT png_before STREQUAL png_after)
    message(FATAL_ERROR "restage: an unchanged staged file was rewritten")
endif()

# A preset added since configure, and a staged file someone deleted.
file(WRITE "${tk}/assets/shaders/added.slangp" "shaders = 0\n")
file(REMOVE "${staged}/README.md")
rebuild(restage "after an add and a delete")
foreach(f added.slangp README.md)
    if(NOT EXISTS "${staged}/${f}")
        message(FATAL_ERROR "restage: ${f} was not staged")
    endif()
endforeach()

# A linked directory is staged as its files, as copy_directory did; a link
# taken as one entry made the copy fail on a directory (POSIX only).
if(NOT WIN32)
    set(linked "${WORK}/restage-linked")
    file(REMOVE_RECURSE "${linked}")
    file(WRITE "${linked}/linked.slangp" "shaders = 0\n")
    file(CREATE_LINK "${linked}" "${tk}/assets/shaders/linked" SYMBOLIC)
    rebuild(restage "with a linked directory")
    if(NOT EXISTS "${staged}/linked/linked.slangp"
       OR IS_SYMLINK "${staged}/linked")
        message(FATAL_ERROR "restage: the linked directory was not staged as files")
    endif()
endif()
