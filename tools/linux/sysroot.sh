#!/bin/sh
# Export the sniper SDK's headers and libraries to OUT (default
# build-sysroot-sniper/) for tools/linux/sniper-clang.cmake. Once per image
# digest; nothing from the source tree goes into the build.
#
# The export goes through a tar: the local directory exporter stalled for
# half an hour inside usr/include. Headers that differ only in case (netfilter's
# xt_MARK.h and xt_mark.h) collapse to one on the Mac's disk; the build uses
# neither.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$here/build-sysroot-sniper}
ctx=$(mktemp -d)
trap 'rm -f "$ctx/sysroot.tar"; rmdir "$ctx"' EXIT
docker build --platform linux/amd64 -f "$here/tools/linux/Dockerfile.sysroot" \
    --output "type=tar,dest=$ctx/sysroot.tar" "$ctx"
mkdir -p "$out"
tar -xf "$ctx/sysroot.tar" -C "$out"
