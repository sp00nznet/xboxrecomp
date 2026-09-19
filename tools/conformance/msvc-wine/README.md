# MSVC under Wine

Vendored from [msvc-wine](https://github.com/mstorsjo/msvc-wine) (ISC, see
`LICENSE.msvc-wine.txt`), trimmed to the minimum the conformance suite needs.

The snippet phase does not need any of this — a snippet is assembly *text*, so
GCC assembles the same bytes MSVC would and nothing about the comparison
changes. Run `bash tools/macos/setup.sh` once, then
`py -3 -m tools.conformance --only snippets`.

The corpus and XBE phases are different. They exist to exercise **MSVC's own
codegen** — the instruction selection, calling conventions and CRT helpers real
Xbox titles are built from. Compiling that corpus with GCC would test shapes no
title contains, which is a weaker test wearing the same name. So these images
run the real MSVC.

> **Licensing.** The ISC licence covers the *scripts* here, not the toolchain
> they download. Visual Studio is not redistributable; `vsdownload.py` fetches
> it from Microsoft under the licence you accept with `--accept-license`.
> Neither image should be pushed to a shared registry.

## Two images, and why

| | `Dockerfile.amd64` | `Dockerfile.i386` |
|---|---|---|
| platform | `linux/amd64` | `linux/386` |
| MSVC host | `Hostx64/x86` | `Hostx86/x86` |
| `cl.exe` | 3 s | works (~5 s) |
| `link.exe` | **2 s** | **never finishes** |
| runs 32-bit PEs | no | **yes** |
| does | compiles and links | executes the harnesses |
| size | 975 MB | 985 MB |
| clean build | ~300 s | ~250 s |

Both emit 32-bit x86 code, and both run the same MSVC. What differs is whether
the *tools themselves* are 32- or 64-bit PEs, and on Apple Silicon that decides
everything.

**`link.exe` cannot be used under 32-bit emulation.** Linking three trivial
functions did not complete in over three hours and left a half-written 2 MB
image. A bare `link /DLL /NOENTRY` with no libraries and no map file was the
*slowest* of the three variants tried, so it is not the flags or the SDK libs —
`link.exe` is I/O-bound over memory-mapped files, and each of those operations is
expensive through wine → QEMU i386 → arm64. `cl.exe` is compute-bound and
survives the same path fine.

**Rosetta cannot execute 32-bit x86 at all**, so the harnesses those tools build
cannot run on the amd64 image. Wine's 32-bit support runs in-process inside an
already-translated x86-64 process, and that mode switch is exactly what Rosetta
refuses:

    rosetta error: invalid gdt selector index 5
    wine: could not load kernel32.dll, status c0000135

Installing `wine32:i386` there does not help — it is not a missing package. (A
standalone 32-bit *ELF* does run under `linux/amd64`, because the kernel routes
it through a `binfmt_misc` qemu-i386 handler, a different path entirely. That is
an easy source of false hope.)

So the two images are complements rather than alternatives, and both corpus and
XBE use both: **amd64 builds, i386 runs.** A 32-bit PE built on one executes on
the other unmodified.

## Could the amd64 image be replaced by native wine?

Half of it, in principle. Worth recording because it looks like a clean win and
only half of it is.

The amd64 image exists to run `cl.exe` and `link.exe`, which are **64-bit** PEs.
Wine on macOS with Rosetta translating x86-64 can run those, so that half could
move to the host and the image could go away. The i386 image could not follow:
executing the 32-bit harnesses needs 32-bit x86, macOS has had no 32-bit process
support since Catalina, and Rosetta translates x86-64 only. Wine 9's new WoW64
avoids needing a 32-bit *Unix* process, but the instructions still have to
execute somewhere, and that is the part Rosetta refuses.

So the trade is: one fewer container, in exchange for a host toolchain to keep
working alongside a container you still need. The hard Docker dependency does
not move.

Two things to check before attempting it:

- **Homebrew's wine casks are disabled.** `wine-stable`, `wine@devel` and
  `wine@staging` were all disabled on 2026-09-01 for failing the macOS
  Gatekeeper check, and the disable is hard — no `--no-quarantine` flag gets
  past it. The remaining routes are an unsigned `.pkg` from winehq.org with the
  quarantine attribute stripped, or CrossOver.
- **Upstream PR [#232](https://github.com/mstorsjo/msvc-wine/pull/232)**
  ("macOS Apple Silicon support", open at the time of writing) is the relevant
  work. It makes `install.sh` treat an arm64 *Darwin* host as `host=x64`, which
  is the same conclusion these images reach — use the x64 toolchain under
  Rosetta, never arm64. It does not affect the containers (`uname` inside them
  is `x86_64`/`i686` on Linux, never `arm64`/`Darwin`), and it does not replace
  the `MSVC_HOST_ARCH` patch below, which exists because upstream has no x86
  host branch at all. Note its README recommends `brew install wine-stable`,
  which no longer works.

The one route that would remove **both** images is CrossOver, which claims
32-bit Windows app support on Apple Silicon through its own thunking rather than
Rosetta. Commercial, and unverified here.

## Building

Neither image is built automatically — each pulls a base image and downloads
roughly 1.5 GB of toolchain, which is not something a test run should do behind
your back.

```bash
cd tools/conformance/msvc-wine

# corpus: compile and link
docker build --platform linux/amd64 -t xboxrecomp-msvc-amd64 -f Dockerfile.amd64 .

# XBE (later): compile, and execute 32-bit PEs
docker build --platform linux/386  -t xboxrecomp-msvc-wine  -f Dockerfile.i386  .
```

The tools land on `PATH` at `/opt/msvc/bin/x86`, and `/w` is the working
directory, so mount a build tree there:

```bash
docker run --rm --platform linux/amd64 -v "$PWD":/w -w /w xboxrecomp-msvc-amd64 \
    sh -c 'cl /nologo /c /O2 /GS- /arch:IA32 corpus.c'
```

`/arch:IA32` is not optional for corpus work: the Xbox CPU is a Pentium III, so
SSE1 and no SSE2. Modern MSVC defaults to SSE2 and puts doubles in XMM, which no
real Xbox binary contains — without the flag the corpus would test instructions
the target cannot execute and skip the x87 paths every title actually uses.

## Image size

Both images are under 1 GB. Together they started at ~13.7 GB:

| | before | after |
|---|---|---|
| `xboxrecomp-msvc-amd64` | 6.9 GB | **975 MB** |
| `xboxrecomp-msvc-wine` (i386) | 6.79 GB | **985 MB** |

Getting there took three separate fixes, each of which looked like it had
worked before it had:

| step | `/opt/msvc` | image |
|---|---|---|
| as downloaded | 3.3 G | 6.9 GB |
| SDK download filter | 3.1 G | 6.9 GB |
| + prune after install | 1.6 G | 6.7 GB |
| + multi-stage build | 1.6 G | **975 MB** |

The same three applied to the i386 image took it from 6.79 GB to 985 MB, so the
diagnosis generalises rather than being particular to one image.

Measure with `docker image inspect --format '{{.Size}}'`, not the `SIZE` column
of `docker images`. That column counts shared parent layers, so with both images
built it reports ~4.2 GB each — it is double-counting the base they have in
common, not a regression.

**`--architecture` does not filter the SDK.** It selects MSVC toolchains only;
the Windows SDK arrives as one set of per-architecture MSIs, so an x86-only
image was carrying a 219 MB arm64 link library. `MSVC_SKIP_SDK_ARCHS` (a local
patch to `vsdownload.py`) drops those at download time.

**Some arm64 libraries have no architecture in their payload name.** 82 MB of
them ship inside arch-neutral MSIs, so no download filter can reach them —
hence the `rm -rf` after install as well.

**Deleting in a later layer does not shrink the image.** The download layer
commits ~3.3 GB; a later `rm` only writes whiteouts over it. That is why the
prune alone moved the image by 200 MB while halving the filesystem, and why the
build is staged: the toolchain is assembled and pruned in a throwaway stage and
only the result is copied into the final image.

What is removed is everything unreachable from "compile C for 32-bit x86 and
link one DLL": `VC/lib/onecore` and `VC/lib/x64` (target libraries for platforms
we never target — the x64 *host* tools are executables and do not link against
them), the arm/arm64 SDK libraries and tools, `References`, `Redist`, `Testing`,
and `MSBuild`, which arrives despite `--with-msbuild no`. The SDK's x64 `bin`
stays, because that is where `rc.exe` and `mt.exe` run from on this host.

Verified after pruning: corpus 211 function vectors and XBE 55, both 0
mismatches.

## Local patches to `install.sh`

Two, both marked `LOCAL PATCH (xboxrecomp)` in the file:

- **`MSVC_HOST_ARCH`.** Upstream hardcodes `host=x64` (with an `arm64` branch).
  With only `Hostx86` unpacked, its `Host$host/$arch/cl.exe` test never matches,
  every architecture hits `continue`, and `bin/` is never created — the build
  still exits 0, leaving a multi-gigabyte image with no usable toolchain.
  `Dockerfile.i386` sets `MSVC_HOST_ARCH=x86`; the amd64 image needs nothing.
- **`sed s/x64/$host/g`.** Without `/g` only the first match per line is
  rewritten, and the `WINEPATH` line names `x64` twice (`Hostx64\x64`), leaving
  the DLL search path pointing at a directory that does not exist. A no-op for
  x64 hosts.

Both images also run `install.sh` under `bash`: it is `#!/usr/bin/env sh` but
uses `&>`, which misparses under Debian/Ubuntu's dash and fails the build over a
step the script itself treats as optional.

## Status

**Corpus** — the toolchain half is proven. On the amd64 image, compiling and
linking the corpus with its real flags takes **3 s and 2 s**, against *hours,
never completing* on i386, and the project's own `image.parse_map` /
`parse_pe` / `corpus_run.lift_all` consume the result unmodified: base
`0x30000000` matching `corpus_run.IMAGE_BASE`, `.text`/`.rdata`/`.data` at the
expected VAs, every symbol resolved, and a fourth function followed in through a
call. The confirmed table row above is now measured, not predicted.

Running the harness needs the **other** image, and that is not a limitation of
how these are configured — it is Rosetta. Adding `wine32:i386` to the amd64
image does not help; a 32-bit PE there dies with

    rosetta error: invalid gdt selector index 5
    wine: could not load kernel32.dll, status c0000135

Wine runs 32-bit code *inside* an already-running Rosetta-translated x86-64
process, and that mode switch is precisely what Rosetta does not support. (A
standalone 32-bit **ELF** does run under `linux/amd64`, because the kernel routes
it through a `binfmt_misc` qemu-i386 handler instead — a different path entirely,
and an easy source of false hope.)

So corpus is a two-image pipeline, both halves measured:

| step | image | time |
|---|---|---|
| `cl` + `link` → dll, map, harness.exe | amd64 | 3 s + 2 s |
| `wine corpus_harness.exe` | i386 | 3 s (hello-world) |

A 32-bit PE built on the amd64 image runs on the i386 image unmodified.

`_run_corpus` is wired across both, and the phase passes: **211 function
vectors, 0 mismatches**, with `_c_nested` lifting to 212 lines of C and
`_c_cmp64` to 94. Both phases together take about 16 seconds:

```
5261 vectors, 0 mismatches            # snippets, GCC container
211 function vectors, 0 mismatches    # corpus, MSVC containers
```

Two details in that wiring worth keeping. The runtime headers are copied into
the work directory and passed as `/I.` rather than mounted and passed by path,
because an include path would otherwise have to survive translation into wine's
view of the filesystem. And the harness run is wrapped in `timeout 900`: a fault
under QEMU dumps core and then *hangs* instead of exiting, so without it a
crashing corpus function would wedge the run rather than fail it.

**XBE (phase 3)** — working, on the same two-image split as corpus:

```
test.xbe: 5 entry points, 5 functions in their call closure, 5 comparable, 0 rejected
55 function vectors from the title, 0 mismatches
```

The four porting items this file used to list turned out to be unnecessary. They
assumed the harness had to be rebuilt against GCC — but built with the *real*
MSVC on the amd64 image, `xbe_harness.c` keeps its `windows.h`, its `__asm {}`
blocks and its `__try`/`__except` exactly as written, and `/EHa` still means what
it means. Porting them to POSIX signals would have been a rewrite of the one file
whose entire job is surviving hostile code, to no benefit.

Phase 3 also never links a PE — one `cl` to one exe — so it was never at risk
from the `link.exe` problem in the first place.

The run is wrapped in `timeout 600` and the title is mounted read-only at
`/xbe`. The timeout is not belt-and-braces: this harness deliberately calls a
title's functions with arguments they never expected, and under QEMU a fault
dumps core and then *hangs* rather than exiting, which would strand the
quarantine loop that re-runs skipping whatever crashed.

### Testing it without a title

`tools/conformance/mkxbe.py` builds a small synthetic XBE — headers,
section table, and five framed leaf functions — so the phase can be exercised
without a real game:

```bash
python3 tools/conformance/mkxbe.py        # writes tools/conformance/test.xbe
python3 -m tools.conformance --xbe tools/conformance/test.xbe
```

One detail cost some time and is worth knowing if you extend it: `scan()` seeds
function discovery from the prologue bytes **`55 8B EC`**, which is *MSVC's*
encoding of `push ebp; mov ebp, esp`. GCC encodes the identical instruction as
`55 89 E5`. A fixture built with the second one is never found at all — the scan
returns nothing and every later stage reports a clean zero.
