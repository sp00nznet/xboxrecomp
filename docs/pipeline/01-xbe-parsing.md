# Step 1: Extracting and Parsing XBE Files

## What is an XBE File?

An XBE (Xbox Executable) is the native executable format for the original Microsoft Xbox (2001). It is analogous to PE (Portable Executable) on Windows or ELF on Linux. Every game shipped as a disc image containing one or more XBE files, with `default.xbe` being the primary game executable.

XBE files contain:
- Machine code compiled for the Xbox's Intel Pentium III (x86, 32-bit)
- Read-only data (strings, constants, vtables)
- Initialized and uninitialized global data
- Kernel import tables (ordinal-based, resolved at load time by the Xbox kernel)
- Library version stamps (which XDK revision the game was built with)
- A signed certificate with the game's title ID and name

Because the Xbox CPU is standard x86, the code inside an XBE is directly comparable to Win32 x86 code -- this is what makes static recompilation feasible.

## Extracting XBE Files from Disc Images

Xbox games are distributed as disc images in XDVDFS (Xbox DVD File System) format. Two tools can extract them:

### extract-xiso

```bash
extract-xiso -x game.iso -d output_dir/
```

This extracts the full directory tree. The main executable is always `default.xbe` in the root. Games may also include additional XBEs for dashboards or updates.

### xdvdfs (Rust tool)

```bash
xdvdfs unpack game.iso output_dir/
```

A newer alternative that handles some edge cases with non-standard disc layouts.

After extraction, the game directory will contain `default.xbe` plus all game assets (textures, models, audio, level data). For Burnout 3: Takedown, the asset directory is approximately 4 GB.

## XBE Header Structure

The XBE format begins with a fixed header at offset 0. All multi-byte values are little-endian.

### Magic Number

The first 4 bytes are `58 42 45 48` -- the ASCII string `XBEH`. If these bytes don't match, the file is not a valid XBE.

### Key Header Fields

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x000  | 4    | Magic | `XBEH` (0x48454258) |
| 0x004  | 256  | Digital Signature | RSA-2048 signature (retail or debug) |
| 0x104  | 4    | Base Address | Virtual address where XBE expects to be loaded |
| 0x108  | 4    | Size of Headers | Total size of all headers |
| 0x10C  | 4    | Size of Image | Total virtual size when loaded |
| 0x110  | 4    | Size of Image Header | Size of this header alone |
| 0x114  | 4    | Timestamp | Build timestamp (Unix epoch) |
| 0x118  | 4    | Certificate Address | VA of the certificate structure |
| 0x11C  | 4    | Number of Sections | How many sections (.text, .rdata, etc.) |
| 0x120  | 4    | Section Headers Address | VA of the section header array |
| 0x124  | 4    | Init Flags | Initialization flags (mount utility drive, etc.) |
| 0x128  | 4    | Entry Point | Encoded entry point address |
| 0x130  | 4    | Kernel Thunk Table | Encoded address of kernel import table |
| 0x150  | 4    | Library Versions Address | VA of library version array |
| 0x154  | 4    | Number of Library Versions | Count of statically linked libraries |

### Base Address

The base address tells you where the Xbox kernel would load this executable in memory. Retail games almost universally use `0x00010000`. This is critical for the recompiler because all absolute addresses in the code (global variable references, string pointers, vtable entries) are relative to this base.

For example, Burnout 3 has base address `0x00010000`, so when the recompiled code references `MEM32(0x004D532C)`, that's a global variable in the .data section at that exact virtual address.

### Entry Point Decoding

The entry point field is XOR-encoded to prevent trivial patching:

- **Retail XBEs**: XOR with `0xA8FC57AB`
- **Debug XBEs**: XOR with `0x94859D4B`

To decode:
```python
raw_entry = header[0x128:0x12C]  # 4 bytes, little-endian
encoded = int.from_bytes(raw_entry, 'little')
entry_point = encoded ^ 0xA8FC57AB  # for retail
```

For Burnout 3, the encoded value decodes to `0x001D2807`, which is the address of the game's startup function in the .text section.

### Kernel Thunk Table Decoding

Similarly, the kernel thunk table address at offset 0x130 is XOR-encoded:

- **Retail**: XOR with `0x5B6D40C6`
- **Debug**: XOR with `0xEFB1F152`

The decoded address points to an array of 32-bit ordinal values in the XBE's .rdata section. Each non-zero entry is `0x80000000 | ordinal`, where the ordinal indexes into the Xbox kernel's export table (366 possible exports, though most games use far fewer).

## Section Table

The section header array starts at the VA specified in the main header. Each section header is 56 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00   | 4    | Flags | Section flags (writable, preload, executable, etc.) |
| 0x04   | 4    | Virtual Address | Where this section is loaded in memory |
| 0x08   | 4    | Virtual Size | Size when loaded (may be larger than raw size for BSS) |
| 0x0C   | 4    | Raw Address | File offset of section data |
| 0x10   | 4    | Raw Size | Size of section data in the file |
| 0x14   | 4    | Section Name Address | VA of the section name string |
| 0x18   | 4    | Section Name Ref Count | Reference count |
| 0x1C   | 4    | Head Shared Page Ref Count Address | Shared page tracking |
| 0x20   | 4    | Tail Shared Page Ref Count Address | Shared page tracking |
| 0x24   | 20   | Section Digest | SHA-1 hash of section data |

### Typical Section Layout (Burnout 3)

| Section | VA | Virtual Size | Raw Size | Description |
|---------|----|-------------|----------|-------------|
| .text   | 0x00011000 | 0x002BC000 (2.73 MB) | 0x002BC000 | Game + CRT + RenderWare code |
| XMV     | 0x002CC200 | 0x00027D24 | 0x00027D24 | Xbox Media Video decoder |
| DSOUND  | 0x002F3F40 | 0x0000CB54 | 0x0000CB54 | DirectSound library |
| WMADEC  | 0x00300D00 | 0x00019D64 | 0x00019D64 | WMA audio decoder |
| XONLINE | 0x0031AA80 | 0x0001E75C | 0x0001E75C | Xbox Live networking |
| XNET    | 0x003391E0 | 0x00013108 | 0x00013108 | Network stack |
| D3D     | 0x0034C2E0 | 0x00014900 | 0x00014900 | D3D8 LTCG library |
| XGRPH   | 0x00360A60 | 0x0000206C | 0x0000206C | Graphics utilities |
| XPP     | 0x00362AE0 | 0x00008CD4 | 0x00008CD4 | Xbox Peripheral Port (input) |
| .rdata  | 0x0036B7C0 | 0x00046B94 | 0x00046B94 | Read-only data (strings, vtables) |
| .data   | 0x003B2360 | 0x003BD000 | 0x000680FC | Globals + BSS |
| DOLBY   | 0x0076B940 | 0x0000716C | 0x0000716C | Dolby audio |
| XON_RD  | 0x00772AC0 | 0x00001528 | 0x00001528 | Xbox Online read-only data |

Note that the named XDK library sections (XMV, DSOUND, etc.) are statically linked -- their code is baked directly into the executable. This is different from Windows DLLs. The recompiler must handle all of this code.

The gap between .data's initialized size (0x000680FC) and its virtual size (0x003BD000) is the BSS region -- zero-initialized global data.

## Kernel Import Table

The kernel import table is an array of 32-bit values at the decoded thunk table address. The Xbox has 366 possible kernel exports, but most games import a subset. Burnout 3 imports 147 kernel functions.

Each entry in the table is either:
- `0x00000000` -- unused slot (ordinal not imported)
- `0x80000000 | ordinal` -- unresolved import for that ordinal

At runtime, the Xbox kernel replaces each entry with the actual function pointer. In our recompiler, we replace each entry with a synthetic VA (`0xFE000000 + slot_index * 4`) that routes to our Win32 reimplementation when called via `RECOMP_ICALL`.

### Example Kernel Imports

| Ordinal | Name | Category |
|---------|------|----------|
| 1       | AvGetSavedDataAddress | AV/Video |
| 49       | HalReadSMBusValue | Hardware |
| 102      | KeTickCount (data) | Timing |
| 116      | MmAllocateContiguousMemory | Memory |
| 190      | NtCreateFile | File I/O |
| 218      | NtReadFile | File I/O |
| 255      | PsCreateSystemThreadEx | Threading |
| 302      | RtlEnterCriticalSection | Sync |

Some ordinals are **data exports**, not functions. For example, ordinal 156 (KeTickCount) is a pointer to a counter variable, and ordinal 164 (LaunchDataPage) is a pointer to a data structure. The thunk entry for these must point to actual readable memory, not a synthetic function VA.

## Library Versions

The XBE header contains an array of library version structures, each identifying a statically linked library:

```
struct XBE_LibraryVersion {
    char name[8];       // Library name (null-padded)
    uint16_t major;     // Major version
    uint16_t minor;     // Minor version
    uint16_t build;     // Build number
    uint16_t flags;     // QFE flags
};
```

For Burnout 3, these reveal XDK version 5849:

| Library | Version |
|---------|---------|
| XAPILIB | 1.0.5849 |
| D3D8LTCG | 1.0.5849 |
| DSOUND | 1.0.5849 |
| XMV | 1.0.5849 |
| XONLINE | 1.0.5849 |
| XNET | 1.0.5849 |

The XDK version is useful for matching known library signatures -- different XDK builds produce slightly different compiled code for the same CRT/library functions.

## Certificate

The certificate structure contains identifying information:

| Field | Value (Burnout 3) |
|-------|-------------------|
| Title ID | Game-specific 32-bit identifier |
| Title Name | "Burnout 3 Takedown" (Unicode) |
| Allowed Media | DVD disc, hard drive |
| Game Region | All regions |
| Game Ratings | Content ratings |

The title name can be useful for verification -- confirming you have the right XBE.

## Using the XBE Parser Tool

The project includes a Python-based XBE parser:

```bash
py -3 -m tools.xbe_parser "path/to/default.xbe"
```

This outputs:
1. **Header summary**: base address, entry point (decoded), image size
2. **Section table**: each section's VA, virtual size, raw offset, raw size, and flags
3. **Kernel imports**: ordinal number, resolved name, and thunk table offset for each import
4. **Library versions**: name and version for each statically linked library
5. **Certificate**: title ID and title name

### Example Output

```
XBE Header:
  Base Address:    0x00010000
  Entry Point:     0x001D2807  (decoded from 0xA52E7FAC ^ 0xA8FC57AB)
  Image Size:      0x00774000
  Sections:        13
  Kernel Imports:  147

Sections:
  .text    VA=0x00011000  VSize=0x002BC000  Raw=0x00001000  RSize=0x002BC000  [exec]
  .rdata   VA=0x0036B7C0  VSize=0x00046B94  Raw=0x0035C000  RSize=0x00046B94  [read]
  .data    VA=0x003B2360  VSize=0x003BD000  Raw=0x003A3000  RSize=0x000680FC  [read,write]
  ...

Kernel Imports:
  [  1] AvGetSavedDataAddress         @ thunk+0x000
  [ 49] HalReadSMBusValue             @ thunk+0x030
  ...
```

## Output for Downstream Steps

The XBE parser produces several data structures consumed by later pipeline stages:

- **Section map**: VA ranges and file offsets for each section -- needed by the disassembler to know where code lives vs. data
- **Kernel import list**: ordinal-to-name mapping -- needed by the kernel shim to wire up Win32 replacements
- **Entry point**: the starting address for recursive descent disassembly
- **Base address**: used to calculate `g_xbox_mem_offset` and set up memory mapping
- **Library versions**: used by function identification to match known library code signatures

The XBE parser is the foundation of the entire pipeline. Every subsequent step depends on understanding the executable's memory layout, section boundaries, and import requirements.
