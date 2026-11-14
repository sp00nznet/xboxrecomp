#!/bin/sh
#
# Builds the x86-64 Linux librashader zip that cmake/librashader.cmake pins,
# for the dxvk-native build (XBOXRECOMP_DXVK_NATIVE): librashader-v0.12.0,
# the same source as the Windows dll, with only the D3D11 runtime.
#
# Why a patched build: upstream compiles its D3D11 runtime on Windows only,
# and its Linux library has only the OpenGL and Vulkan runtimes, which cannot
# draw into dxvk-native's D3D11 swap chain. dxvk-native gives the same D3D11
# COM interfaces on Linux, so linux-d3d11.patch widens the runtime's
# target_os = "windows" gates (and the windows crate dependency) to Linux.
# The windows crates themselves are cfg(windows), so it also builds the
# registry's windows, windows-core and windows-strings with those gates
# opened (and three std::os::windows string conversions made portable), and
# an empty windows-future (WinRT async, unused); nothing else changes, and
# the game runs the same libra_d3d11 calls as on Windows. The two
# d3dcompiler_47 functions the runtime calls, D3DCompile and D3DCreateBlob,
# come from d3dcompiler_vkd3d.c over libvkd3d-shader.so.1, which the game
# already ships in lib/ (Wine's d3dcompiler_47 under Proton is the same
# compiler). MPL-2.0 asks for the source of a modified file, so the zip
# carries the patch next to the source link.
#
# Maintainers run it once per librashader, Rust or vkd3d bump, then upload
# the zip as a release asset (see cmake/librashader.cmake for the URL) and
# pin its sha256 there. Players and build machines only download the zip.
# Everything builds in the pinned Steam Linux Runtime sniper SDK image
# (Dockerfile.linux), so the library needs no glibc newer than sniper's
# 2.31; the Rust tarball, cargo-about and the source commit are pinned below.
#
#   third_party/librashader/build-linux.sh <vkd3d-dir> <out-dir>
#
# <vkd3d-dir> is tools/linux/vkd3d.sh's output (include/vkd3d/, lib/), the
# library the game ships. Needs docker or podman (DOCKER=podman) that can
# run x86-64 images, and python3.
set -eu

if [ $# -ne 2 ]; then
    awk 'NR<3 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' "$0"
    exit 2
fi
vkd3d=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)

version=0.12.0
# The tag's commit, so a moved tag cannot change the input.
commit=87e8a97b50516d997defeaa168173dcd185d4022
rust=1.99.1
rust_sha256=231fcae47ee950621374725b5fc3b5441d2fbda2955ac45dd7dc816d8876cfe7
cargo_about=0.9.3
release=xbr1
here=$(cd "$(dirname "$0")" && pwd)
zip="librashader-x86_64-linux-d3d11-v${version}-${release}.zip"
docker=${DOCKER:-docker}

[ -f "$vkd3d/include/vkd3d/vkd3d_shader.h" ] && [ -f "$vkd3d/lib/libvkd3d-shader.so.1" ] ||
    { echo "$vkd3d has no include/vkd3d/vkd3d_shader.h or lib/libvkd3d-shader.so.1" >&2; exit 1; }

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
ctx="$work/ctx"
mkdir -p "$ctx/vkd3d/include/vkd3d" "$ctx/vkd3d/lib"
cp "$here/linux-d3d11.patch" "$here/d3dcompiler_vkd3d.c" "$here/notices-linux.py" \
    "$here/about.toml" "$here/about.hbs" "$ctx/"
cp "$vkd3d/include/vkd3d/vkd3d_shader.h" "$vkd3d/include/vkd3d/vkd3d_types.h" \
    "$ctx/vkd3d/include/vkd3d/"
# By soname: the link records libvkd3d-shader.so.1, the file lib/ holds.
cp -L "$vkd3d/lib/libvkd3d-shader.so.1" "$ctx/vkd3d/lib/"
vkd3d_source=$(head -n 1 "$vkd3d/SOURCE" 2>/dev/null || echo "vkd3d (unknown)")

"$docker" build --platform linux/amd64 -f "$here/Dockerfile.linux" \
    --build-arg VERSION="$version" --build-arg COMMIT="$commit" \
    --build-arg RUST="$rust" --build-arg RUST_SHA256="$rust_sha256" \
    --build-arg CARGO_ABOUT="$cargo_about" --build-arg JOBS="${JOBS:-8}" \
    --output "type=local,dest=$work/stage" "$ctx"

stage="$work/stage"
gcc=$(cat "$stage/gcc.txt")
rm "$stage/gcc.txt"
cat > "$stage/SOURCE.txt" <<EOF
librashader ${version} (MPL-2.0), source:
https://github.com/SnowflakePowered/librashader/tree/librashader-v${version}
commit ${commit}, with linux-d3d11.patch (in this zip) applied: it builds
the D3D11 runtime on Linux, for dxvk-native: it opens the cfg(windows)
gates of librashader and of copies of the windows 0.62.2, windows-core
0.62.2 and windows-strings 0.5.1 crates, and replaces the unused
windows-future crate with an empty one; it changes nothing else.

Built by third_party/librashader/build-linux.sh in xboxrecomp, in the Steam
Linux Runtime sniper SDK image, with Rust ${rust}
(x86_64-unknown-linux-gnu) and ${gcc},
cargo features: --no-default-features --features runtime-d3d11.
D3DCompile and D3DCreateBlob: third_party/librashader/d3dcompiler_vkd3d.c
in xboxrecomp, over libvkd3d-shader.so.1 (${vkd3d_source}), loaded from
the game's lib/. THIRDPARTY.txt lists the code linked into the library and
its licences.
EOF
python3 -I - "$stage" "$out/$zip" <<'EOF'
import os, sys, zipfile
stage, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(stage)):
        info = zipfile.ZipInfo(name, date_time=(2026, 7, 4, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = (0o755 if name.endswith(".so") else 0o644) << 16
        with open(os.path.join(stage, name), "rb") as f:
            z.writestr(info, f.read())
EOF
( cd "$out" && { shasum -a 256 "$zip" 2>/dev/null || sha256sum "$zip"; } )
