#!/usr/bin/env bash
#
# Builds the x86-64 Windows librashader zip that cmake/librashader.cmake
# pins, from the upstream librashader-v0.12.0 source, unmodified.
#
# Why not the upstream prebuilt: its librashader.dll is built with MSVC and
# every runtime, so it imports D3DX9_43.dll (only the D3D9 runtime's
# D3DXGetShaderConstantTable) and the VC++ 2015-2022 runtime (MSVCP140,
# VCRUNTIME140). A stock Windows 11 has neither, so LoadLibrary fails with
# error 126 and present.shader stays off. This build keeps only the D3D11
# runtime the toolkit uses and links the C++ runtime statically with
# llvm-mingw, so the dll imports only system dlls (kernel32, d3dcompiler_47,
# the UCRT api sets, ...).
#
# Maintainers run it once per librashader or toolchain bump, then upload the
# zip as a release asset (see cmake/librashader.cmake for the URL) and pin its
# sha256 there. Players and build machines only download the zip: no Rust.
# The Rust toolchain, the llvm-mingw release and cargo-about are pinned
# below, and the script stops on any other; with those, two runs on macOS
# arm64 gave the same zip bytes (other hosts are untested).
#
#   third_party/librashader/build-windows.sh <llvm-mingw-dir> <out-dir>
#
# Needs rustup (user space is enough: sh rustup-init.sh --no-modify-path),
# git, python3 and cargo-about (cargo install --locked cargo-about
# --version <pinned> --features cli). The Rust toolchain is installed by
# rustup on first use.
#
# The zip carries THIRDPARTY.txt: the licence and copyright of every crate
# and bundled C/C++ library that is linked into the dll (glslang, SPIRV-Cross,
# Mesa, the LLVM and MinGW-w64 runtimes, Rust's std), which a binary
# distribution must reproduce.
set -euo pipefail

if [ $# -ne 2 ]; then
    awk 'NR<3 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' "$0"
    exit 2
fi
llvm_mingw=$(cd "$1" && pwd)
mkdir -p "$2"
out=$(cd "$2" && pwd)

version=0.12.0
# The tag's commit, so a moved tag cannot change the input.
commit=87e8a97b50516d997defeaa168173dcd185d4022
rust=1.99.1
target=x86_64-pc-windows-gnullvm
# llvm-mingw 20260922 (ucrt): its clang, and the MinGW-w64 runtime linked
# statically into the dll (LICENSES/MinGW-w64-runtime.txt in this repository
# is that runtime's licence file).
llvm_mingw_release=20260922
clang_version="clang version 23.1.2 (https://github.com/llvm/llvm-project.git 85ac560262434c9ccfc0c183ec22d4138ed647fb)"
cargo_about=0.9.3
release=xbr1
here=$(cd "$(dirname "$0")" && pwd)
zip="librashader-x86_64-windows-d3d11-v${version}-${release}.zip"

PATH="$llvm_mingw/bin:${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
command -v x86_64-w64-mingw32-clang >/dev/null ||
    { echo "no x86_64-w64-mingw32-clang in $llvm_mingw/bin" >&2; exit 1; }
got=$(x86_64-w64-mingw32-clang --version | head -n 1)
[ "$got" = "$clang_version" ] ||
    { echo "$llvm_mingw is not llvm-mingw $llvm_mingw_release: $got" >&2; exit 1; }
got=$(cargo about --version 2>/dev/null || true)
[ "$got" = "cargo-about $cargo_about" ] ||
    { echo "need cargo-about $cargo_about, found '${got:-none}'" >&2; exit 1; }
rustup toolchain install "$rust" --profile minimal --target "$target" >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 \
    --branch "librashader-v${version}" \
    https://github.com/SnowflakePowered/librashader.git "$work/src"
got=$(git -C "$work/src" rev-parse HEAD)
[ "$got" = "$commit" ] || { echo "tag is at $got, expected $commit" >&2; exit 1; }

export CC_x86_64_pc_windows_gnullvm=x86_64-w64-mingw32-clang
export CXX_x86_64_pc_windows_gnullvm=x86_64-w64-mingw32-clang++
export AR_x86_64_pc_windows_gnullvm=llvm-ar
# Empty: cc would add -lstdc++ (a dynamic libc++.dll with llvm-mingw); the
# static libc++ and libunwind come from the link args below instead.
export CXXSTDLIB_x86_64_pc_windows_gnullvm=
export CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_LINKER=x86_64-w64-mingw32-clang
# Build paths are remapped so the dll holds no host paths and the bytes do
# not depend on where it was built.
export CARGO_TARGET_X86_64_PC_WINDOWS_GNULLVM_RUSTFLAGS="\
-C target-feature=+crt-static -C link-arg=-static \
-C link-arg=-Wl,-Bstatic -C link-arg=-lc++ -C link-arg=-lunwind \
-C link-arg=-Wl,--no-insert-timestamp \
--remap-path-prefix=${CARGO_HOME:-$HOME/.cargo}=/cargo \
--remap-path-prefix=$work/src=/src"
export SOURCE_DATE_EPOCH=1783123200     # the v0.12.0 release day
export CARGO_TARGET_DIR="$work/target"

(cd "$work/src" && cargo "+$rust" build --locked -p librashader-capi \
    --profile optimized --target "$target" \
    --no-default-features --features runtime-d3d11)

# Same layout as the upstream zips for the files we use: librashader.dll at
# the top (upstream's build script renames librashader_capi.dll the same
# way), plus the MPL-2.0 text, where the source is, and the third-party
# notices.
stage="$work/stage"
mkdir -p "$stage"
cp "$CARGO_TARGET_DIR/$target/optimized/librashader_capi.dll" "$stage/librashader.dll"
cp "$work/src/LICENSE.md" "$stage/LICENSE.md"

# The crates, from the same locked graph and features as the build.
# --frozen: the build fetched every crate, and no network lookup (such as
# ClearlyDefined) can change the text.
(cd "$work/src" && cargo "+$rust" about generate --frozen --fail \
    -c "$here/about.toml" -m librashader-capi/Cargo.toml \
    --no-default-features --features runtime-d3d11 \
    "$here/about.hbs" -o "$work/crates.txt")
# The -sys crates build C/C++ code whose licence files sit in their native/
# trees, which cargo-about does not read (it does find Mesa's per-file
# headers in spirv-to-dxil-sys). Add those, the toolchain runtimes and std.
(cd "$work/src" && cargo "+$rust" metadata --frozen --format-version 1 \
    --manifest-path librashader-capi/Cargo.toml) > "$work/meta.json"
python3 -I - "$work/meta.json" "$work/crates.txt" "$llvm_mingw/LICENSE.TXT" \
    "$here/../../LICENSES/MinGW-w64-runtime.txt" \
    "$(rustup "+$rust" which rustc | xargs dirname | xargs dirname)/share/doc/rust/licenses/Apache-2.0.txt" \
    "$rust" "$stage/THIRDPARTY.txt" <<'EOF'
import json, os, sys
meta, crates, llvm, mingw, std_apache, rust, dest = sys.argv[1:]
dirs = {p["name"]: os.path.dirname(p["manifest_path"])
        for p in json.load(open(meta))["packages"]}

def text(path):
    with open(path, encoding="utf-8") as f:
        return f.read().rstrip() + "\n"

def section(title, body):
    return "\n\n" + "=" * 80 + "\n" + title + "\n" + "-" * 80 + "\n" + body

out = ["librashader.dll third-party notices\n"
       "===================================\n\n"
       "librashader itself is MPL-2.0 (LICENSE.md). The dll also contains the\n"
       "code below; each part keeps its own licence.\n"]
native = [
    ("glslang (bundled by glslang-sys, native/glslang/LICENSE.txt)",
     ("glslang-sys", "native/glslang/LICENSE.txt")),
    ("SPIRV-Cross (bundled by spirv-cross-sys), Apache-2.0, native/SPIRV-Cross/LICENSE",
     ("spirv-cross-sys", "native/SPIRV-Cross/LICENSE")),
    ("SPIRV-Cross (bundled by spirv-cross-sys), LicenseRef-KhronosFreeUse",
     ("spirv-cross-sys", "native/SPIRV-Cross/LICENSES/LicenseRef-KhronosFreeUse.txt")),
]
for title, (crate, rel) in native:
    out.append(section(title, text(os.path.join(dirs[crate], rel))))
out.append(section(
    "LLVM libc++, libc++abi, libunwind and compiler-rt (llvm-mingw), "
    "linked statically", text(llvm)))
out.append(section("MinGW-w64 runtime (llvm-mingw), linked statically", text(mingw)))
out.append(section(
    "The Rust standard library " + rust + ", MIT OR Apache-2.0, used under Apache-2.0",
    "Copyright The Rust Project Contributors. The crates std bundles are\n"
    "listed in COPYRIGHT-library.html of the Rust " + rust + " distribution.\n\n"
    + text(std_apache)))
out.append("\n\n" + text(crates))
with open(dest, "w", encoding="utf-8", newline="\n") as f:
    f.write("".join(out))
EOF
cat > "$stage/SOURCE.txt" <<EOF
librashader ${version} (MPL-2.0), unmodified source:
https://github.com/SnowflakePowered/librashader/tree/librashader-v${version}
commit ${commit}

Built by third_party/librashader/build-windows.sh in xboxrecomp with Rust ${rust},
target ${target}, llvm-mingw ${llvm_mingw_release}
(${clang_version}),
cargo features: --no-default-features --features runtime-d3d11.
THIRDPARTY.txt lists the code linked into the dll and its licences.
EOF
python3 -I - "$stage" "$out/$zip" <<'EOF'
import os, sys, zipfile
stage, dest = sys.argv[1], sys.argv[2]
with zipfile.ZipFile(dest, "w", zipfile.ZIP_DEFLATED) as z:
    for name in sorted(os.listdir(stage)):
        info = zipfile.ZipInfo(name, date_time=(2026, 7, 4, 0, 0, 0))
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0o644 << 16
        with open(os.path.join(stage, name), "rb") as f:
            z.writestr(info, f.read())
EOF
( cd "$out" && shasum -a 256 "$zip" )
