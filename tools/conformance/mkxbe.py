"""Build a minimal synthetic XBE with real x86 leaf functions in .text.

Enough of the format for tools/xbe_parser to load and for the conformance XBE
phase to find, lift and execute functions. Not a valid title -- no signature,
no imports, no TLS, no certificate beyond the few fields the parser reads.

Usage:

    python3 tools/conformance/mkxbe.py [OUT]

    OUT   where to write it (default: test.xbe beside this script, so it
          lands in the same place wherever you run it from)

Then point the conformance XBE phase at it:
    python3 -m tools.conformance --xbe tools/conformance/test.xbe

What it is for: exercising the XBE phase without a real game. It proves the
plumbing -- load, scan, lift, execute, compare -- on five trivial leaf
functions. It is not evidence that a real title's code survives that path,
where rejection rates and the crash-quarantine loop actually matter.

Adding functions: append raw bytes to FUNCS. Each must begin with the prologue
bytes 55 8B EC, which is *MSVC's* encoding of `push ebp; mov ebp, esp`. GCC
encodes the identical instruction as 55 89 E5, and xbe_run.scan() seeds function
discovery from the first form only -- a function written with the second is
never found, and every later stage reports a clean zero rather than an error.
"""
import os, struct, sys

BASE = 0x00010000
HDR_SIZE = 0x1000          # headers occupy the first page
TEXT_VA = 0x00011000
TEXT_OFF = 0x1000

FUNCS = [
    # add3(a,b,c)
    bytes.fromhex("558bec" "8b4508" "03450c" "034510" "5d" "c3"),
    # submul(a,b) = (a-b)*3
    bytes.fromhex("558bec" "8b4508" "2b450c" "6bc003" "5d" "c3"),
    # shifts(a,b) = a << (b & 31)
    bytes.fromhex("558bec" "8b4508" "8b4d0c" "d3e0" "5d" "c3"),
    # cmpsel(a,b) = min(a,b) -- exercises the cmp+cmov peephole
    bytes.fromhex("558bec" "8b4508" "8b4d0c" "39c8" "0f4dc1" "5d" "c3"),
    # bits(a,b) = (~(a & b)) squared
    bytes.fromhex("558bec" "8b4508" "23450c" "f7d0" "0fafc0" "5d" "c3"),
]

def build():
    text = b""
    for f in FUNCS:
        text += f
        text += b"\xcc" * ((16 - len(f) % 16) % 16)     # pad to 16
    text_size = len(text)

    cert_va = BASE + 0x200
    sec_hdrs_va = BASE + 0x400
    name_va = BASE + 0x600

    hdr = bytearray(HDR_SIZE)
    hdr[0:4] = b"XBEH"
    struct.pack_into("<I", hdr, 0x0104, BASE)              # base_address
    struct.pack_into("<I", hdr, 0x0108, HDR_SIZE)          # headers_size
    struct.pack_into("<I", hdr, 0x010C, HDR_SIZE + text_size)   # image_size
    struct.pack_into("<I", hdr, 0x0110, 0x178)             # image_header_size
    struct.pack_into("<I", hdr, 0x0114, 0)                 # timestamp
    struct.pack_into("<I", hdr, 0x0118, cert_va)           # certificate_addr
    struct.pack_into("<I", hdr, 0x011C, 1)                 # num_sections
    struct.pack_into("<I", hdr, 0x0120, sec_hdrs_va)       # section_headers_addr
    struct.pack_into("<I", hdr, 0x0124, 0)                 # init_flags
    struct.pack_into("<I", hdr, 0x0128, TEXT_VA)           # entry_point (raw)
    struct.pack_into("<I", hdr, 0x013C, BASE)              # pe_base_address
    struct.pack_into("<I", hdr, 0x0140, HDR_SIZE + text_size)   # pe_size_of_image

    # Certificate: size + timestamp + title id, then a UTF-16 title.
    c = cert_va - BASE
    struct.pack_into("<I", hdr, c + 0x00, 0x1D0)
    struct.pack_into("<I", hdr, c + 0x08, 0x12345678)
    title = "SYNTH".encode("utf-16-le")
    hdr[c + 0x0C:c + 0x0C + len(title)] = title

    # One executable section.
    s = sec_hdrs_va - BASE
    struct.pack_into("<I", hdr, s + 0x00, 0x00000006)      # flags: exec|preload
    struct.pack_into("<I", hdr, s + 0x04, TEXT_VA)
    struct.pack_into("<I", hdr, s + 0x08, text_size)
    struct.pack_into("<I", hdr, s + 0x0C, TEXT_OFF)
    struct.pack_into("<I", hdr, s + 0x10, text_size)
    struct.pack_into("<I", hdr, s + 0x14, name_va)
    n = name_va - BASE
    hdr[n:n + 6] = b".text\x00"

    return bytes(hdr) + text


if __name__ == "__main__":
    default_out = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "test.xbe")
    if len(sys.argv) > 1 and sys.argv[1] in ("-h", "--help"):
        print(__doc__.strip())
        sys.exit(0)
    out = sys.argv[1] if len(sys.argv) > 1 else default_out
    data = build()
    open(out, "wb").write(data)
    print(f"wrote {out}: {len(data)} bytes, {len(FUNCS)} functions "
          f"in .text @ 0x{TEXT_VA:08X}")
