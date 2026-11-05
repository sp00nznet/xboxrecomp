#!/usr/bin/env bash
#
# Writes THIRDPARTY-macos.txt here: the third-party notices for upstream's
# prebuilt macOS librashader.dylib (0.12.0), which cmake/librashader.cmake
# stages unmodified and pins by sha256. Upstream's zips carry no such file,
# but the dylib links glslang, SPIRV-Cross, Rust's std and about 150 crates
# whose licences a binary distribution must reproduce. cmake stages the
# committed file as shaders/THIRDPARTY-librashader.txt, the name the Windows
# build's notices take.
#
# Upstream's release workflow (.github/workflows/build.yml at the tag) runs
# `cargo run -p librashader-build-script -- --profile optimized` on macOS
# runners, which builds librashader-capi with its default features
# (runtime-all: OpenGL, Vulkan and Metal on macOS) from the locked graph. This
# script reads the same graph and features for both macOS targets. Nothing
# is compiled.
#
# Maintainers run it once per librashader bump, with the Rust toolchain and
# cargo-about pinned in build-windows.sh (it installs the pinned Rust and
# stops on any other cargo-about), and commit the result.
#
#   third_party/librashader/notices-macos.sh [<out-file>]
#
# Needs rustup, git, python3 and cargo-about, and network for the crates.
set -euo pipefail

if [ $# -gt 1 ]; then
    awk 'NR<3 {next} /^#/ {sub(/^# ?/, ""); print; next} {exit}' "$0"
    exit 2
fi
here=$(cd "$(dirname "$0")" && pwd)
dest=${1:-$here/THIRDPARTY-macos.txt}
mkdir -p "$(dirname "$dest")"
dest="$(cd "$(dirname "$dest")" && pwd)/$(basename "$dest")"

version=0.12.0
commit=87e8a97b50516d997defeaa168173dcd185d4022
# Runs cargo and cargo-about only. Upstream built the dylib with Rust 1.97.0
# (its strings name rustc 2d8144b7, the 1.97.0 release commit); the std
# notice below names that release.
rust=1.99.1
upstream_rust=1.97.0
cargo_about=0.9.3

PATH="${CARGO_HOME:-$HOME/.cargo}/bin:$PATH"
got=$(cargo about --version 2>/dev/null || true)
[ "$got" = "cargo-about $cargo_about" ] ||
    { echo "need cargo-about $cargo_about, found '${got:-none}'" >&2; exit 1; }
rustup toolchain install "$rust" --profile minimal >/dev/null

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
git -c advice.detachedHead=false clone -q --depth 1 \
    --branch "librashader-v${version}" \
    https://github.com/SnowflakePowered/librashader.git "$work/src"
got=$(git -C "$work/src" rev-parse HEAD)
[ "$got" = "$commit" ] || { echo "tag is at $got, expected $commit" >&2; exit 1; }

# Every crate the locked graph names, so the runs below need no network.
(cd "$work/src" && cargo "+$rust" fetch --locked -q)
(cd "$work/src" && cargo "+$rust" about generate --frozen --fail \
    -c "$here/about.toml" -m librashader-capi/Cargo.toml \
    --target aarch64-apple-darwin --target x86_64-apple-darwin \
    "$here/about-macos.hbs" -o "$work/crates.txt")
(cd "$work/src" && cargo "+$rust" metadata --frozen --format-version 1 \
    --manifest-path librashader-capi/Cargo.toml) > "$work/meta.json"
# As in build-windows.sh: the -sys crates' native licence files, which
# cargo-about does not read, and std. libc++ is the system's dylib on macOS
# (the dylib imports /usr/lib/libc++.1.dylib), so no toolchain runtime is in it.
python3 -I - "$work/meta.json" "$work/crates.txt" \
    "$(rustup "+$rust" which rustc | xargs dirname | xargs dirname)/share/doc/rust/licenses/Apache-2.0.txt" \
    "$upstream_rust" "$dest" <<'EOF'
import json, os, sys
meta, crates, std_apache, rust, dest = sys.argv[1:]
dirs = {p["name"]: os.path.dirname(p["manifest_path"])
        for p in json.load(open(meta))["packages"]}

def text(path):
    with open(path, encoding="utf-8") as f:
        return f.read().rstrip() + "\n"

def section(title, body):
    return "\n\n" + "=" * 80 + "\n" + title + "\n" + "-" * 80 + "\n" + body

out = ["librashader.dylib third-party notices\n"
       "=====================================\n\n"
       "librashader itself is MPL-2.0 (LICENSE-librashader.txt). The dylib, upstream's\n"
       "macOS release of librashader 0.12.0, also contains the code below; each\n"
       "part keeps its own licence.\n"]
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
    "The Rust standard library " + rust + ", MIT OR Apache-2.0, used under Apache-2.0",
    "Copyright The Rust Project Contributors. The crates std bundles are\n"
    "listed in COPYRIGHT-library.html of the Rust " + rust + " distribution.\n\n"
    + text(std_apache)))
out.append("\n\n" + text(crates))
with open(dest, "w", encoding="utf-8", newline="\n") as f:
    f.write("".join(out))
EOF
shasum -a 256 "$dest"
