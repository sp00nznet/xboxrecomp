# Bundled present shaders

RetroArch slang presets run by librashader when `present.shader` names one
(`docs/runtime/config.md`). The files are unmodified copies from
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) at commit
`2b0530bac0e8083d9d643c194929dc0a6b69bd35`, in the same layout, so the
presets' relative paths resolve as upstream. The top-level `<name>.slangp`
files are ours: a `#reference` line each, so a bare name in the config maps
to `shaders/<name>.slangp` beside the executable. `smaa.slangp` and
`cas.slangp` are full presets of ours instead, and `xboxrecomp/` holds our
own passes (below).

Two of them set calmer curvature than upstream, which bends a 4:3 picture a
lot on a small screen. The in-game Shaders page resets to these values, and
`[present.shader_params.<name>]` in the config overrides them:

| Name | Parameter | Ours | Upstream |
|---|---|---|---|
| `crt-lottes` | `warpX` | 0.01 | 0.031 |
| `crt-lottes` | `warpY` | 0.02 | 0.041 |
| `crt-lottes-fast` | `CURVATURE` | 0.01 | 0.02 |

`param-kinds.toml` is ours too: it tells the in-game Shaders page which
parameters are a list of named modes (crt-lottes-fast's `MASK`,
crt-lottes's `shadowMask`), with the names taken from the shaders' own
source comments (`docs/runtime/config.md`, "Parameter kinds").

Only public-domain, MIT and Unlicense files are bundled; each file keeps its
licence header.

| Name | Upstream preset | Licence |
|---|---|---|
| `crt-lottes` | `crt/crt-lottes.slangp` | public domain (Timothy Lottes) |
| `crt-lottes-fast` | `crt/crt-lottes-fast.slangp` | public domain (Timothy Lottes, CRTS) |
| `crt-hyllian` | `crt/crt-hyllian-fast.slangp` | MIT (Hyllian) |
| `fsr` | `edge-smoothing/fsr/fsr.slangp` | Unlicense; `ffx_a.h`, `ffx_fsr1.h` MIT (AMD) |
| `smaa` | after `anti-aliasing/smaa.slangp` | passes Unlicense (Jonatas Esteves); `SMAA.hlsl`, `AreaTex.png`, `SearchTex.png` MIT (iryoku/smaa, `LICENSE-SMAA.txt`) |
| `cas` | none: `xboxrecomp/cas.slang`, ours | MIT (AMD FidelityFX CAS, `xboxrecomp/LICENSE-FidelityFX-CAS.txt`) |

The five-pass `crt/crt-hyllian.slangp` is not bundled: its support passes and
LUT images carry no licence. Point `present.shader` at your own copy to use it.

`smaa` differs from upstream's preset in three lines, all about filtering.
Upstream reads the first pass's output nearest (`filter_linear0 = false`),
which RetroArch ignores, since it samples a pass's output with the filter of
the pass that reads it. librashader uses the filter of the pass that drew
it, so under librashader the neighbourhood blend lands on whole texels and
changes about 0.1% of the pixels instead of about 10%. `filter_linear0 =
true` gives the same picture on both. The lookup textures are set as SMAA
reads them: `areaTex` bilinear, `searchTex` point. The first pass is our
`xboxrecomp/passthrough.slang` in place of upstream's `stock.slang`, which
has no licence header. SMAA's textures came from iryoku/smaa (`71c806a`)
through slang-shaders as PNGs: SearchTex holds iryoku's `SearchTex.h` in
red, AreaTex its `AreaTex.h` in red and green, with one texel off (x 159,
y 0: 0,0 for 66,61); restoring it changed no pixel on six test frames.
Linear `searchTex` changes a few hundred pixels per 640x480 frame by 1 to 7
levels against point.

`cas` is a one-pass port of FidelityFX CAS's sharpen-only path. Its
`CAS_AMOUNT` (0.4) mixes the result with the input, since even at
`CAS_SHARPNESS` 0 full CAS rings a 480-line HUD; at the defaults HUD edge
energy stays within 1.3 times stock.

`smaa` and `cas` name their last pass's scale (`source`, 1.0), so in a
profile the next stage gets the frame at its own size.

## librashader

The presets run in [librashader](https://github.com/SnowflakePowered/librashader)
v0.12.0, `librashader.dll` / `librashader.dylib` / `librashader.so` beside
the executable: on macOS the upstream prebuilt library, on x86-64 Windows a
build of the unmodified source with only the D3D11 runtime and a static C++
runtime, so it needs nothing beyond a stock Windows 10 or 11 (arm64 Windows
still takes the upstream library, which needs the DirectX 9 and Visual C++
runtimes). On x86-64 Linux (the dxvk-native build) it is the same D3D11
runtime, built for Linux with `librashader-linux-d3d11.patch` here, which
only lets that runtime compile off Windows, so presets draw through
dxvk-native as they do on Windows; its shader compiler is the game's own
vkd3d-shader (`lib/libvkd3d-shader.so.1`), the one Proton's d3dcompiler
uses. It is MPL-2.0: the licence is `LICENSE-librashader.txt` in this
directory, and the source is at
https://github.com/SnowflakePowered/librashader/tree/librashader-v0.12.0.
On macOS, x86-64 Windows and Linux, `THIRDPARTY-librashader.txt` here lists
the licences and copyrights of the crates and C/C++ libraries built into it.
If the library cannot load, the game names the reason in its log and
presents without the shader.
