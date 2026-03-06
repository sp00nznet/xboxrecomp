# XBE File Format Reference

The XBE (Xbox Executable) format is the native executable format for the original Microsoft Xbox console. It is analogous to Windows PE (Portable Executable) but with Xbox-specific extensions for kernel thunks, digital signatures, and section management.

Reference: https://xboxdevwiki.net/Xbe

## Magic and Identification

- **Magic bytes**: `XBEH` (0x48454258 little-endian) at file offset 0
- File extension: `.xbe`
- Always found as `default.xbe` at the root of an Xbox disc image

## Header Layout

The XBE header begins at file offset 0 and contains the following fields:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x0000 | 4 | Magic | `XBEH` (0x48454258) |
| 0x0004 | 256 | DigitalSignature | RSA signature over header |
| 0x0104 | 4 | BaseAddress | Virtual base address (always 0x00010000) |
| 0x0108 | 4 | SizeOfHeaders | Total header size in bytes |
| 0x010C | 4 | SizeOfImage | Total image size when loaded |
| 0x0110 | 4 | SizeOfImageHeader | Size of the image header portion |
| 0x0114 | 4 | TimeDateStamp | Build timestamp (Unix epoch) |
| 0x0118 | 4 | CertificateAddress | VA of the certificate structure |
| 0x011C | 4 | NumberOfSections | Number of section headers |
| 0x0120 | 4 | SectionHeadersAddress | VA of the section header array |
| 0x0124 | 4 | InitFlags | Initialization flags |
| 0x0128 | 4 | EntryPoint | Encoded entry point address |
| 0x012C | 4 | TlsAddress | VA of TLS directory |
| 0x0130 | 4 | PeStackCommit | Default thread stack commit size |
| 0x0134 | 4 | PeHeapReserve | Default heap reserve size |
| 0x0138 | 4 | PeHeapCommit | Default heap commit size |
| 0x013C | 4 | PeSizeOfImage | PE-compatible image size |
| 0x0140 | 4 | PeChecksum | PE-compatible checksum |
| 0x0144 | 4 | PeTimeDateStamp | PE-compatible timestamp |
| 0x0148 | 4 | DebugPathnameAddress | VA of debug build path string |
| 0x014C | 4 | DebugFilenameAddress | VA of debug build filename string |
| 0x0150 | 4 | DebugUnicodeFilenameAddress | VA of Unicode debug filename |
| 0x0154 | 4 | KernelThunkAddress | Encoded kernel thunk table address |
| 0x0158 | 4 | NonKernelImportDirectoryAddress | Non-kernel import directory |
| 0x015C | 4 | NumberOfLibraryVersions | Number of library version entries |
| 0x0160 | 4 | LibraryVersionsAddress | VA of library version array |
| 0x0164 | 4 | KernelLibraryVersionAddress | VA of kernel lib version entry |
| 0x0168 | 4 | XapiLibraryVersionAddress | VA of XAPI lib version entry |
| 0x016C | 4 | LogoBitmapAddress | VA of the Xbox logo bitmap |
| 0x0170 | 4 | LogoBitmapSize | Size of the logo bitmap |

### Base Address

The base address is always **0x00010000** for all retail and debug Xbox executables. This is the virtual address at which the XBE image header is loaded. All other VA fields in the header are relative to this base. The Xbox kernel maps the executable starting at this fixed address.

### Entry Point Encoding

The entry point address at offset 0x0128 is XOR-encoded to prevent trivial modification:

- **Retail XBEs**: XOR key = `0xA8FC57AB`
- **Debug XBEs**: XOR key = `0x94859D4B`

To decode:

```c
uint32_t encoded_entry = *(uint32_t*)(xbe + 0x0128);

// Try retail key first
uint32_t entry_retail = encoded_entry ^ 0xA8FC57AB;
// If entry falls within .text section, it's retail
if (entry_retail >= text_va && entry_retail < text_va + text_size) {
    entry_point = entry_retail;
} else {
    // Try debug key
    entry_point = encoded_entry ^ 0x94859D4B;
}
```

### Kernel Thunk Address Encoding

The kernel thunk table address at offset 0x0154 is similarly XOR-encoded:

- **Retail XBEs**: XOR key = `0x5B6D40B6`
- **Debug XBEs**: XOR key = `0xEFB1F152`

The decoded address points to an array of 32-bit ordinal values (terminated by 0) that index into the Xbox kernel's export table.

## Section Table

The section header array begins at `SectionHeadersAddress` and contains `NumberOfSections` entries. Each section header is 56 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | Flags | Section flags (see below) |
| 0x04 | 4 | VirtualAddress | VA where section is loaded |
| 0x08 | 4 | VirtualSize | Size in memory (may exceed raw size) |
| 0x0C | 4 | RawAddress | File offset of section data |
| 0x10 | 4 | RawSize | Size of data in the XBE file |
| 0x14 | 4 | SectionNameAddress | VA of the section name string |
| 0x18 | 4 | SectionReferenceCount | Reference count for demand loading |
| 0x1C | 4 | HeadSharedPageRefCountAddress | VA of head page ref count |
| 0x20 | 4 | TailSharedPageRefCountAddress | VA of tail page ref count |
| 0x24 | 20 | SectionDigest | SHA-1 hash of section contents |

### Section Flags

| Bit | Name | Description |
|-----|------|-------------|
| 0 | Writable | Section is writable at runtime |
| 1 | Preload | Section is loaded at startup (vs demand-loaded) |
| 2 | Executable | Section contains executable code |
| 3 | InsertedFile | Section was inserted by the linker |
| 4 | HeadPageReadOnly | First page is read-only |
| 5 | TailPageReadOnly | Last page is read-only |

### Common Sections

| Name | Typical VA Range | Purpose |
|------|-----------------|---------|
| .text | 0x00011000+ | Executable code (game + CRT + engine) |
| .rdata | after .text | Read-only data (strings, vtables, const data) |
| .data | after .rdata | Initialized read-write data |
| .bss | (virtual only) | Zero-initialized data (extends .data's virtual size) |
| XSIMAGE | varies | Xbox dashboard section |
| XSIMAGE1 | varies | Xbox dashboard section (alternate) |
| $$XONLINE | varies | Xbox Live online code |
| $$XTIMAGE | varies | Title image data |

Games using the XDK statically link libraries, which appear as named sections:

| Section Name | Library |
|-------------|---------|
| D3D | Direct3D 8 (LTCG variant) |
| DSOUND | DirectSound |
| XMV | Xbox Media Video |
| XONLINE | Xbox Live networking |
| XNET | Xbox networking stack |
| XGRPH | Xbox graphics utilities |
| XPP | Xbox peripheral port (input) |
| WMADEC | Windows Media Audio decoder |

## Library Version Table

Located at `LibraryVersionsAddress`, containing `NumberOfLibraryVersions` entries. Each entry is 16 bytes:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 8 | LibraryName | ASCII name, null-padded to 8 chars |
| 0x08 | 2 | MajorVersion | Major version number |
| 0x0A | 2 | MinorVersion | Minor version number |
| 0x0C | 2 | BuildVersion | Build number (often indicates XDK version) |
| 0x0E | 2 | Flags | Library flags (QFE version + approval) |

The build version often corresponds to the XDK version used to compile the game. Common values include 4361, 4627, 5028, 5455, 5558, 5788, 5849, 5933.

## Certificate

Located at `CertificateAddress`, the certificate contains game metadata:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | Size | Size of certificate structure |
| 0x04 | 4 | TimeDateStamp | Certificate timestamp |
| 0x08 | 4 | TitleId | Unique title identifier |
| 0x0C | 80 | TitleName | Unicode game title (null-terminated) |
| 0x5C | 64 | AlternateTitleIds | Array of 16 alternate title IDs |
| 0x9C | 4 | AllowedMedia | Bitmask of allowed media types |
| 0xA0 | 4 | GameRegion | Region bitmask (NA=1, Japan=2, RestOfWorld=4) |
| 0xA4 | 4 | GameRatings | Content ratings bitmask |
| 0xA8 | 4 | DiskNumber | Disc number (for multi-disc games) |
| 0xAC | 4 | Version | Certificate version |
| 0xB0 | 16 | LanKey | LAN encryption key |
| 0xC0 | 16 | SignatureKey | Title signature key |
| 0xD0 | 256 | AlternateSignatureKeys | 16 alternate signature keys |

### Allowed Media Types

| Bit | Media Type |
|-----|-----------|
| 0 | Hard disk |
| 1 | DVD-X2 (Xbox disc) |
| 2 | DVD-CD |
| 3 | CD-ROM |
| 4 | DVD-5 RO |
| 5 | DVD-9 RO |
| 6 | DVD-5 RW |
| 7 | DVD-9 RW |
| 11 | Nonsecure hard disk |
| 12 | Nonsecure mode |

## TLS (Thread Local Storage) Directory

Located at `TlsAddress`, the TLS directory defines thread-local storage:

| Offset | Size | Field | Description |
|--------|------|-------|-------------|
| 0x00 | 4 | RawDataStartAddress | VA of TLS data template start |
| 0x04 | 4 | RawDataEndAddress | VA of TLS data template end |
| 0x08 | 4 | TlsIndexAddress | VA of TLS index variable |
| 0x0C | 4 | TlsCallbackAddress | VA of TLS callback array |
| 0x10 | 4 | SizeOfZeroFill | Bytes of zero-fill after template |
| 0x14 | 4 | Characteristics | TLS characteristics flags |

The Xbox kernel uses the TLS directory to set up per-thread storage accessed via the FS segment register (fs:[0x28] on Xbox).

## Kernel Thunk Table

The kernel thunk table is the Xbox equivalent of an import address table. It is an array of 32-bit values at the decoded `KernelThunkAddress`:

- Before loading: each entry contains a kernel export ordinal (with bit 31 set)
- After loading: the Xbox kernel replaces each ordinal with the actual function/data pointer

The table is terminated by a 0 entry.

### Ordinal Format

```
Raw value:  0x80000001
Ordinal:    value & 0x7FFFFFFF = 1 (AvGetSavedDataAddress)
```

Games typically import 100-200 kernel functions. The Xbox kernel exports up to 366 functions and data objects. See [kernel-exports.md](kernel-exports.md) for the full list of commonly imported ordinals.

## Practical Notes for Recompilation

1. **Memory layout reproduction**: The base address 0x00010000 must be reproduced on the host system. Use `CreateFileMapping` + `MapViewOfFileEx` to place data sections at their original VAs.

2. **Section data**: Copy .rdata and .data from the XBE file at their raw offsets to their VAs. Zero-fill the BSS region (virtual size minus raw size in .data).

3. **Kernel thunks**: Decode the thunk table, read the ordinals, and replace each with a pointer to your host implementation of that kernel function.

4. **Entry point**: After decoding, the entry point is a VA within .text. For static recompilation, this is the function to call after all initialization is complete.

5. **XDK version**: The library build version (especially XAPILIB) indicates which XDK the game was compiled with, which affects structure layouts and available APIs.
