#!/bin/sh
# Build libvkd3d-shader for the dxvk-native build into OUT (default
# build/vkd3d-sniper/): lib/libvkd3d-shader.so*, include/vkd3d/, COPYING and
# SOURCE, which names the tarball and its sha256 for the configure check
# (cmake/vkd3d_shader.cmake, XBOXRECOMP_VKD3D_SHADER_DIR) and the package's
# source link. The version and sha256 are cmake/vkd3d_shader.cmake's. Under
# emulation on Apple Silicon; once per pin.
set -eu
here=$(cd "$(dirname "$0")/../.." && pwd)
out=${1:-$here/build/vkd3d-sniper}
pins=$here/cmake/vkd3d_shader.cmake
ver=$(sed -n 's/^set(XBOXRECOMP_VKD3D_SHADER_VERSION \([^)]*\))$/\1/p' "$pins")
sha=$(sed -n '/^set(XBOXRECOMP_VKD3D_SHADER_SHA256$/{n;s/^ *\([0-9a-f]\{64\}\))$/\1/p;}' "$pins")
[ -n "$ver" ] && [ -n "$sha" ] || { echo "vkd3d.sh: no pin in $pins" >&2; exit 1; }
if [ -f "$out/SOURCE" ] && [ "$(cat "$out/SOURCE")" = "$(printf 'vkd3d %s\nsha256 %s' "$ver" "$sha")" ]; then
    echo "vkd3d.sh: $out already holds vkd3d $ver"
    exit 0
fi
ctx=$(mktemp -d)
trap 'rm -f "$ctx/vkd3d-$ver.tar.xz"; rmdir "$ctx"' EXIT
tarball=${VKD3D_TARBALL:-}
if [ -n "$tarball" ]; then
    cp "$tarball" "$ctx/vkd3d-$ver.tar.xz"
else
    curl -fsSL "https://dl.winehq.org/vkd3d/source/vkd3d-$ver.tar.xz" -o "$ctx/vkd3d-$ver.tar.xz"
fi
got=$(shasum -a 256 "$ctx/vkd3d-$ver.tar.xz" 2>/dev/null || sha256sum "$ctx/vkd3d-$ver.tar.xz")
got=${got%% *}
[ "$got" = "$sha" ] || { echo "vkd3d.sh: vkd3d-$ver.tar.xz has sha256 $got, expected $sha" >&2; exit 1; }
docker build --platform linux/amd64 -f "$here/tools/linux/Dockerfile.vkd3d" \
    --build-arg VERSION="$ver" --build-arg SHA256="$sha" --build-arg JOBS="${JOBS:-8}" \
    --output "$out" "$ctx"
