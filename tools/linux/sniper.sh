#!/bin/sh
# Build the toolkit and (CTEST=1, the default) its POSIX ctests for Linux
# x86-64 in the sniper SDK image; results land in OUT (default
# build-linux-sniper/): status.txt, the configure/build logs, ctest.txt and
# the ctest build trees. Under emulation on Apple Silicon.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$here/build-linux-sniper}
exec docker build --platform linux/amd64 -f "$here/tools/linux/Dockerfile.sniper" \
    --build-arg JOBS="${JOBS:-8}" --build-arg CTEST="${CTEST:-1}" \
    --output "$out" "$here"
