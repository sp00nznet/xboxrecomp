"""THIRDPARTY.txt for the Linux librashader.so (Dockerfile.linux runs it).

The same sections as build-windows.sh writes for the dll: the -sys crates'
native licence files that cargo-about does not read, the statically linked
C++ runtime, Rust's std and the crates. vkd3d-shader is not in it: the .so
only links against it, and the game ships it in lib/ with its own licence.

    python3 -I notices-linux.py META CRATES STDCXX_COPYRIGHT STDCXX_VERSION \
        RUST_APACHE RUST DEST
"""

import json
import os
import sys

meta, crates, stdcxx, stdcxx_version, std_apache, rust, dest = sys.argv[1:]
dirs = {p["name"]: os.path.dirname(p["manifest_path"]) for p in json.load(open(meta))["packages"]}


def text(path):
    with open(path, encoding="utf-8") as f:
        return f.read().rstrip() + "\n"


def section(title, body):
    return "\n\n" + "=" * 80 + "\n" + title + "\n" + "-" * 80 + "\n" + body


out = [
    "librashader.so third-party notices\n"
    "==================================\n\n"
    "librashader itself is MPL-2.0 (LICENSE.md), built with the changes in\n"
    "linux-d3d11.patch. The library also contains the code below; each part\n"
    "keeps its own licence. D3DCompile and D3DCreateBlob come from xboxrecomp's\n"
    "third_party/librashader/d3dcompiler_vkd3d.c (MIT, xboxrecomp's LICENSE),\n"
    "over libvkd3d-shader.so.1, which is not linked in.\n"
]
native = [
    ("glslang (bundled by glslang-sys, native/glslang/LICENSE.txt)", ("glslang-sys", "native/glslang/LICENSE.txt")),
    (
        "SPIRV-Cross (bundled by spirv-cross-sys), Apache-2.0, native/SPIRV-Cross/LICENSE",
        ("spirv-cross-sys", "native/SPIRV-Cross/LICENSE"),
    ),
    (
        "SPIRV-Cross (bundled by spirv-cross-sys), LicenseRef-KhronosFreeUse",
        ("spirv-cross-sys", "native/SPIRV-Cross/LICENSES/LicenseRef-KhronosFreeUse.txt"),
    ),
]
for title, (crate, rel) in native:
    out.append(section(title, text(os.path.join(dirs[crate], rel))))
out.append(
    section(
        "GNU libstdc++ " + stdcxx_version.strip() + " (Steam Runtime sniper SDK), linked statically, "
        "GPL-3.0-or-later WITH GCC-exception-3.1",
        text(stdcxx),
    )
)
out.append(
    section(
        "The Rust standard library " + rust + ", MIT OR Apache-2.0, used under Apache-2.0",
        "Copyright The Rust Project Contributors. The crates std bundles are\n"
        "listed in COPYRIGHT-library.html of the Rust " + rust + " distribution.\n\n" + text(std_apache),
    )
)
out.append("\n\n" + text(crates))
with open(dest, "w", encoding="utf-8", newline="\n") as f:
    f.write("".join(out))
