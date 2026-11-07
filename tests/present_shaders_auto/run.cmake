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
