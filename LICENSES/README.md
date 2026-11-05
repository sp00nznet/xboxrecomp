# Third-party licence texts

`LGPL-2.1.txt` is the verbatim GNU Lesser General Public License v2.1, from

    https://www.gnu.org/licenses/old-licenses/lgpl-2.1.txt

It applies to the files listed under "LGPL-2.1-or-later" in the top-level
NOTICE — the MCPX APU sources and `src/nv2a/nv2a_regs.h`, extracted from xemu
and remaining the copyright of espes, Jannik Vogel and Matt Borgerson.

Shipping the verbatim text alongside those files is an LGPL requirement, not
a courtesy.

The rest are for dsp56300, linked only in builds with
`-DXBOXRECOMP_APU_DSP=ON` (see "dsp56300" in the top-level NOTICE):

- `MIT-dsp56300.txt`: dsp56300's own licence, verbatim from its v1.0.0
  release archive.
- `THIRDPARTY-dsp56300-emu.txt`: the archive's third-party file, verbatim:
  every bundled crate with its licence and copyright.
- `Apache-2.0.txt`, `LLVM-exception.txt`, `Unicode-3.0.txt`,
  `Zlib-foldhash.txt`: the licence texts those crates use, taken from that
  file.

The enhancements overlay, compiled only with `-DXBOXRECOMP_ENHANCE=ON`
(see "the enhancements overlay" in the top-level NOTICE), vendors these
components under `third_party/`:

- `MIT-microui.txt`: microui's licence, verbatim from its repository.
- `MIT-stb.txt`: the stb licence file, verbatim (MIT or public domain).
- `OFL-1.1-atkinson-hyperlegible.txt`: the Atkinson Hyperlegible font
  licence, verbatim from google/fonts (also beside the font as `OFL.txt`).
- `ISC-MIT-lucide.txt`: Lucide's `LICENSE`, verbatim from its 1.57.0
  release (ISC, with Feather's MIT notice for the icons Lucide took from
  it); also beside the subset font as `third_party/lucide/LICENSE`.
- `CC0-1.0-kenney-input.txt`: the Kenney Input Prompts `License.txt`,
  verbatim (also `third_party/kenney-input/LICENSE`), followed by the CC0
  1.0 legal code from creativecommons.org. CC0 asks for no credit; the
  NOTICE gives it anyway.

`present.shader` (also `-DXBOXRECOMP_ENHANCE=ON`; see "MPL-2.0 —
librashader" in the top-level NOTICE) loads the prebuilt librashader at run
time:

- `MPL-2.0-librashader.txt`: librashader's `LICENSE.md`, verbatim from the
  `librashader-v0.12.0` tag. It ships as `shaders/LICENSE-librashader.txt`
  beside the library; the source is at https://github.com/SnowflakePowered/librashader/tree/librashader-v0.12.0.
  The x86-64 Windows library is built from that source, unmodified, by
  `third_party/librashader/build-windows.sh`; its zip carries the same licence
  and a `SOURCE.txt` with the link.
- That zip's `THIRDPARTY.txt` (staged as `shaders/THIRDPARTY-librashader.txt`)
  holds the licence and copyright of every crate and C/C++ library linked
  into the dll; it is generated with each build, not kept here (about 800 KB,
  most of it per-file Mesa MIT notices). The top-level NOTICE lists them by
  licence.
- Upstream's macOS zips have no such file. The same notices for its dylib
  are `third_party/librashader/THIRDPARTY-macos.txt` (about 260 KB, written
  by `notices-macos.sh` there), staged under the same name; they are kept
  there, not here, since only macOS packages ship them.

Every Windows executable (llvm-mingw) links part of the toolchain's runtime
statically (see "the toolchain runtime in every Windows executable" in the
top-level NOTICE):

- `MinGW-w64-runtime.txt`: the MinGW-w64 runtime's licence and the notices
  of the parts it took from elsewhere, verbatim `COPYING.MinGW-w64-runtime.txt`
  from mingw-w64 commit 96f1cb7de334c47808377ada40d6819aca1b48a9 (the same
  file llvm-mingw 20260922 installs). `third_party/librashader/build-windows.sh`
  puts it in the Windows librashader zip's `THIRDPARTY.txt` too.
- `LLVM-LICENSE.txt`: LLVM's libunwind and compiler-rt builtins, verbatim
  `LICENSE.TXT` from llvm-mingw 20260922: Apache-2.0 with LLVM Exceptions,
  and the Legacy LLVM (University of Illinois/NCSA) licence, which still
  covers code not yet relicensed and requires its copyright notice in
  binary redistributions. It is here, not only in the librashader zip, so a
  package built without librashader carries it too.

The packaging copies this whole directory into every game package, macOS
ones included.
