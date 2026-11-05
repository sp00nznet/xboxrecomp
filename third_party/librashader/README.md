# librashader C header

`librashader.h` from the
[librashader](https://github.com/SnowflakePowered/librashader) v0.12.0 release
zips (C ABI 2, API 5), unmodified. The header is MIT (its comment); the
library is MPL-2.0 and is never linked: `src/kernel/nv2a_present_chain.c`
loads a prebuilt `librashader.dylib` / `librashader.dll` at run time, and
the packaging that ships it adds the MPL-2.0 notice and the source link.
macOS and aarch64 Windows take the upstream release; x86-64 Windows takes a
D3D11-only build of the same source that imports no D3DX9 or Visual C++
runtime, made by `build-windows.sh` here (it sits outside `tools/` so it
does not stale every game's generated code). `cmake/librashader.cmake` pins
both by sha256. `about.toml` and `about.hbs` drive the cargo-about run that
writes that zip's `THIRDPARTY.txt`; the licence of the MinGW-w64 runtime it
links is `LICENSES/MinGW-w64-runtime.txt` at the top of the repository.

Upstream's macOS zips carry no third-party notices, so `notices-macos.sh`
writes them for its dylib from the same source, config and pinned tools
(`about.toml`, `about-macos.hbs`), without building anything:
`THIRDPARTY-macos.txt` is that output, committed because staging takes it
from here (as `shaders/THIRDPARTY-librashader.txt`). Rerun the script on a
librashader bump; two runs give the same bytes.
