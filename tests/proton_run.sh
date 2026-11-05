#!/bin/bash
# proton_run.sh <exe> [args...]: run a cross-built Windows test under Proton
# (umu-run) and exit with its exit code. Meant as CMAKE_CROSSCOMPILING_EMULATOR,
# so ctest can run the mingw-built tests on a Linux host:
#   cmake -S tests/fist -B build/fist-win \
#         -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-x86_64.cmake \
#         -DCMAKE_CROSSCOMPILING_EMULATOR=$PWD/tests/proton_run.sh
#   cmake --build build/fist-win && ctest --test-dir build/fist-win
#
# The exe runs from a generated batch file, which sends stdout/stderr to a
# file (plain umu-run drops the program's stderr) and appends the exit
# code; the file is printed afterwards. A batch file because Wine escapes
# the quotes of a quoted argument as \", which cmd.exe /c can't parse.
# Inside a distrobox, umu-run is reached through distrobox-host-exec.
# RECOMP_* and HLSL_* variables are passed on explicitly. Arguments are quoted (a %
# is doubled); ones holding a double quote are not supported.
#
# Environment: WINEPREFIX (default ~/.local/share/proton-test), PROTONPATH
# (GE-Proton), GAMEID (umu-default), PROTON_RUN_TIMEOUT (180 s),
# PROTON_RUN_LOCK (a file to flock around the run; unset: no lock).
set -u
[ $# -ge 1 ] || { echo "usage: proton_run.sh <exe> [args...]" >&2; exit 2; }
exe=$(readlink -f "$1"); shift
dir=$(dirname "$exe")
out=$(mktemp /tmp/proton_run.XXXXXX)
bat=$out.cmd
winpath() { local p="Z:$1"; echo "${p//\//\\}"; }
cmdline="\"$(winpath "$exe")\""
for a in "$@"; do cmdline="$cmdline \"${a//%/%%}\""; done
printf '@echo off\r\n%s > "%s" 2>&1\r\necho proton_run_exit=%%ERRORLEVEL%%>> "%s"\r\n' \
    "$cmdline" "$(winpath "$out")" "$(winpath "$out")" > "$bat"

envs=(WINEPREFIX="${WINEPREFIX:-$HOME/.local/share/proton-test}"
      PROTONPATH="${PROTONPATH:-GE-Proton}" GAMEID="${GAMEID:-umu-default}")
while IFS= read -r kv; do envs+=("$kv"); done < <(env | grep -E '^(RECOMP|HLSL)_' || true)

# env -C sets the directory again on the host side, since
# distrobox-host-exec does not keep the caller's cwd.
run=(env -C "$dir" "${envs[@]}" timeout -k 10 -s INT "${PROTON_RUN_TIMEOUT:-180}"
     umu-run cmd.exe /c "$(winpath "$bat")")
command -v umu-run >/dev/null || run=(distrobox-host-exec "${run[@]}")
cd "$dir" || exit 2
if [ -n "${PROTON_RUN_LOCK:-}" ]; then
    flock -w 3600 "$PROTON_RUN_LOCK" "${run[@]}" </dev/null >/dev/null 2>&1
else
    "${run[@]}" </dev/null >/dev/null 2>&1
fi
rc=$(grep -a -o 'proton_run_exit=[0-9-]*' "$out" | tail -1 | cut -d= -f2)
grep -a -v 'proton_run_exit=' "$out"
rm -f "$out" "$bat"
[ -n "$rc" ] || { echo "proton_run: no exit code (run failed or timed out)"; exit 125; }
# A Windows exit code is 32 bits and the shell keeps only the low byte, so
# a failure such as 256 would read as a pass.
if [ "$rc" -ne 0 ] && [ $((rc & 255)) -eq 0 ]; then exit 1; fi
exit "$rc"
