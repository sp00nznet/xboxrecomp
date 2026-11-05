#!/usr/bin/env bash
# Every ctest a POSIX host can run, in one command, with one verdict.
#
#   tests/posix_ctest.sh [BUILD_DIR]      (default: build/posix-ctest)
#
# Two kinds of test dir live under tests/:
#   - standalone projects (they call project()): configured and built one by
#     one. A harness that cannot run here refuses at configure time with
#     message(FATAL_ERROR) in its own CMakeLists.txt and is reported as n/a;
#     any other configure error (a missing package, say) is a FAIL;
#   - in-tree dirs (no project(), pulled in by the runtime's own CMakeLists
#     with add_subdirectory): run from one build of the toolkit root, n/a when
#     this host's build does not add them. Run on their own they fail with
#     "No cmake_minimum_required", which is how a plain loop over tests/*
#     used to report 15 false failures.
# Every dir gets exactly one line: pass, skip, n/a or FAIL. Logs go to
# BUILD_DIR/<name>.{cfg,build,ctest}.log and the lines to summary.txt. Exit 0
# only when nothing failed. JOBS (default 6) sets how many standalone dirs
# build at once.
# On macOS, export DEVELOPER_DIR=/Library/Developer/CommandLineTools first or
# the link step can pick up a mismatched SDK.
set -u
here=$(cd "$(dirname "$0")" && pwd)
root=$(dirname "$here")
B=${1:-$root/build/posix-ctest}
mkdir -p "$B"
B=$(cd "$B" && pwd)
gen=()
command -v ninja >/dev/null && gen=(-G Ninja)
# Audio tests must never open a real device on a developer's machine. Both
# spellings: SDL3 reads SDL_AUDIO_DRIVER, and a tree still on SDL2 (or
# sdl2-compat) reads SDL_AUDIODRIVER.
export SDL_AUDIO_DRIVER=${SDL_AUDIO_DRIVER:-${SDL_AUDIODRIVER:-dummy}}
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-$SDL_AUDIO_DRIVER}
# The tests read the runtime's env rows, so a RECOMP_* left in the shell
# (RECOMP_MENU=0 from a golden run) turns passing tests into failures. Each
# test sets the rows it needs itself.
while IFS= read -r v; do
    unset "$v"
done < <(compgen -e | grep '^RECOMP_' || true)
# A test that maps an XBE and then uses the path layer without a save dir
# gets the default hard disk, which is under the developer's own data dir
# (<XDG>/xboxrecomp/<title id>; the shared root's saves are copied into it on
# first use). Keep that in the build dir; tests that need their own still set
# XDG_DATA_HOME themselves.
export XDG_DATA_HOME=$B/xdg-data
mkdir -p "$XDG_DATA_HOME"

is_project() {
    grep -qiE '^[[:space:]]*project[[:space:]]*\(' "$1/CMakeLists.txt"
}

# Build phase, in parallel: prints "n/a"/"FAIL" lines, or "ok <name> <dir>"
# for a dir whose tests should run. build_one NAME [SRC_DIR [CMAKE_ARGS...]]
# builds a variant of another dir under its own name.
build_one() {
    local t=$1 d=$here/${2:-$1} log=$B/$1.cfg.log
    shift; [ $# -gt 0 ] && shift
    gen=($gen_str)
    # The dsp56300 archive (cmake/dsp56300.cmake, about 12 MB packed and 26 MB
    # unpacked) once per suite, not once per build that links it: every
    # project that adds the toolkit links it now that XBOXRECOMP_APU_DSP
    # defaults ON. apu_dsp_link fills the cache before the parallel
    # phase, which then only reads it (its stamp matches).
    set -- "$@" "-DXBOXRECOMP_DSP56300_CACHE=$B/_deps/dsp56300"
    # The same for the librashader release: present_chain_lrs is the one
    # suite project that fetches it for this host.
    set -- "$@" "-DXBOXRECOMP_LIBRASHADER_CACHE=$B/_deps/librashader"
    if ! cmake -S "$d" -B "$B/$t" ${gen[@]+"${gen[@]}"} -DCMAKE_BUILD_TYPE=Release "$@" \
            >"$log" 2>&1; then
        # n/a only when the one error is the harness's own refusal: an error
        # from a find module or the runtime's CMake is a broken host.
        if [ "$(grep -c '^CMake Error' "$log")" = 1 ] &&
            grep -qE '^CMake Error at CMakeLists.txt:[0-9]+ \(message\):' "$log"; then
            echo "n/a   $t: $(sed -n '/^CMake Error at CMakeLists.txt:[0-9]* (message):/,/^$/p' "$log" |
                sed '1d;/^$/d;s/^ *//' | tr '\n' ' ' | sed 's/ $//')"
        else
            echo "FAIL  $t: configure, see $log"
        fi
        return
    fi
    if ! cmake --build "$B/$t" >"$B/$t.build.log" 2>&1; then
        echo "FAIL  $t: build, see $B/$t.build.log"
        return
    fi
    echo "ok $t $B/$t"
}
export -f build_one is_project
export here B
export gen_str="${gen[*]-}"

built=$B/built.txt
: >"$built"
[ -f "$here/apu_dsp_link/CMakeLists.txt" ] && build_one apu_dsp_link >>"$built"
for d in "$here"/*/; do
    [ -f "$d/CMakeLists.txt" ] && is_project "$d" && basename "$d"
done | grep -vxE 'apu_dsp_emu|apu_dsp_link' |
    xargs -P "${JOBS:-6}" -I{} bash -c 'build_one "$@"' _ {} >>"$built"
[ -f "$here/apu_dsp_emu/CMakeLists.txt" ] && build_one apu_dsp_emu >>"$built"
# template_main once more with the enhancements layer in: the template starts
# it only under XBOXRECOMP_ENHANCE, and a new game may build either way.
if [ -f "$here/template_main/CMakeLists.txt" ]; then
    build_one template_main_enhance template_main -DXBOXRECOMP_ENHANCE=ON >>"$built"
    # And without the APU DSP: template_main itself links dsp56300 by
    # default, and a game may still build stub-only (XBOXRECOMP_APU_DSP=OFF).
    build_one template_main_stub template_main -DXBOXRECOMP_APU_DSP=OFF >>"$built"
fi
# input_actions once more under ThreadSanitizer: its live rebuild case races
# a polling thread and a flip thread against rebuilds (n/a without TSan).
if [ -f "$here/input_actions/CMakeLists.txt" ]; then
    build_one input_actions_tsan input_actions -DINPUT_ACTIONS_TSAN=ON >>"$built"
fi
for d in "$here"/*/; do
    [ -f "$d/CMakeLists.txt" ] || echo "n/a   $(basename "$d"): no CMakeLists.txt, not a ctest"
done >>"$built"

# The root build also reads the pinned x86-64 Windows librashader.dll's
# imports (librashader_imports), from a zip cached once per BUILD_DIR.
if cmake -S "$root" -B "$B/_root" ${gen[@]+"${gen[@]}"} -DCMAKE_BUILD_TYPE=Release \
        -DXBOXRECOMP_LIBRASHADER_TEST_WIN64=ON \
        "-DXBOXRECOMP_LIBRASHADER_CACHE=$B/_deps/librashader" \
        "-DXBOXRECOMP_DSP56300_CACHE=$B/_deps/dsp56300" \
        >"$B/_root.cfg.log" 2>&1 &&
    cmake --build "$B/_root" >"$B/_root.build.log" 2>&1; then
    root_ok=1
else
    root_ok=0
fi
for d in "$here"/*/; do
    t=$(basename "$d")
    [ -f "$d/CMakeLists.txt" ] && ! is_project "$d" || continue
    if [ $root_ok = 0 ]; then
        echo "FAIL  $t: toolkit root configure or build, see $B/_root.*.log"
        continue
    fi
    f=$(find "$B/_root" -path "*/$t/CTestTestfile.cmake" | head -1)
    if [ -n "$f" ]; then
        echo "ok $t $(dirname "$f")"
    else
        echo "n/a   $t: not in this host's build"
    fi
done >>"$built"

# Test phase, one dir at a time: guest_cpu and the scheduler tests time
# hand-overs in milliseconds and fail on a host busy with the builds above.
{
    grep -v '^ok ' "$built"
    grep '^ok ' "$built" | while read -r _ t dir; do
        log=$B/$t.ctest.log
        if ctest --test-dir "$dir" --timeout 600 --no-tests=error \
                --output-on-failure >"$log" 2>&1; then
            if grep -q '(Skipped)' "$log"; then
                echo "skip  $t: $(grep -c '(Skipped)' "$log") skipped; $(grep -E 'tests passed' "$log" | tail -1)"
            else
                echo "pass  $t: $(grep -E 'tests passed' "$log" | tail -1)"
            fi
        elif grep -q 'No tests were found' "$log"; then
            echo "FAIL  $t: no tests registered"
        else
            echo "FAIL  $t: $(grep -E 'tests passed|tests failed' "$log" | tail -1)"
        fi
    done
} | awk '{ n = $2; sub(/:$/, "", n); print n "\t" $0 }' | LC_ALL=C sort -t "$(printf '\t')" -k1,1 |
    cut -f2- >"$B/summary.txt"

# One line per dir, or a dir slipped through (or was counted twice).
for d in "$here"/*/; do
    t=$(basename "$d")
    c=$(awk -v t="$t:" '$2 == t' "$B/summary.txt" | wc -l | tr -d ' ')
    [ "$c" = 1 ] || echo "FAIL  $t: $c summary lines, expected 1" >>"$B/summary.txt"
done
cat "$B/summary.txt"

! grep -q '^FAIL' "$B/summary.txt"
